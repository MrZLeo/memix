#include "memix/view.hpp"
#include "view_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = memix::fs;

static_assert(memix::to_string(memix::ViewTargetKind::FileTree) == "file_tree");
static_assert(memix::parse_view_target_kind("context_pack")
              == memix::ViewTargetKind::ContextPack);
static_assert(memix::to_string(memix::ViewVersionKind::Generation)
              == "generation");
static_assert(!memix::parse_view_input_kind("environment"));

namespace {

fs::path unique_test_path(std::string_view label) {
  std::random_device random_device;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const memix::FsResult<fs::path> temp_dir = fs::temp_directory_path();
  REQUIRE(temp_dir);
  return *temp_dir
       / std::format("memix-view-{}-{}-{}", label, nanos, random_device());
}

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label)
      : path_(unique_test_path(label)) {
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    make_tree_writable();
    static_cast<void>(fs::remove_all(path_));
  }

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  void make_tree_writable() const noexcept {
    std::error_code error;
    std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, error);
    error.clear();
    const std::filesystem::recursive_directory_iterator end;
    std::filesystem::recursive_directory_iterator iterator{
        path_, std::filesystem::directory_options::skip_permission_denied,
        error};
    while (!error && iterator != end) {
      const std::filesystem::file_status status =
          iterator->symlink_status(error);
      if (!error && std::filesystem::is_directory(status)) {
        std::filesystem::permissions(iterator->path(),
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
      }
      if (error) {
        error.clear();
      }
      iterator.increment(error);
    }
  }

  fs::path path_;
};

void write_file(const fs::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  REQUIRE(output);
  output << contents;
  REQUIRE(output);
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool path_exists(const fs::path &path) {
  const memix::FsResult<bool> exists = fs::exists(path);
  REQUIRE(exists);
  return *exists;
}

bool owner_can_write(const fs::path &path) {
  const memix::FsResult<fs::file_status> status = fs::status(path);
  REQUIRE(status);
  const fs::perms permissions = status->permissions();
  return (permissions & fs::perms::owner_write) != fs::perms::none;
}

memix::FileIngestResult
ingest_context_file(const memix::Store &store, memix::MemoryObjectType type,
                    const fs::path &source, const fs::path &context_path,
                    std::string_view message) {
  const memix::StoreResult<memix::FileIngestResult> result = store.ingest_file({
      .type = type,
      .source_path = source,
      .context_path = context_path,
      .message = message,
  });
  REQUIRE(result.has_value());
  return *result;
}

std::size_t count_temp_directories(const fs::path &parent) {
  if (!path_exists(parent)) {
    return 0;
  }

  std::size_t count = 0;
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{parent, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    const bool is_directory = entry.is_directory(entry_error);
    REQUIRE_FALSE(entry_error);
    if (is_directory && entry.path().filename().string().starts_with(".tmp-")) {
      ++count;
    }
  }
  REQUIRE_FALSE(iterator_error);
  return count;
}

} // namespace

TEST_CASE("view expression lowering parses path locators with defaults",
          "[view]") {
  TemporaryDirectory directory("lowering");
  const memix::ViewService views{memix::Store(directory.path() / "store")};

  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      views.parse_expression(
          R"({"schema":"memix.view.expr.v2","inputs":[)"
          R"({"kind":"context","locator":"rules/repo/AGENTS.md"},)"
          R"({"kind":"context","locator":"chat/task/2026-05-20/@latest"}]})");
  REQUIRE(expression.has_value());
  REQUIRE(expression->schema == "memix.view.expr.v2");
  REQUIRE(expression->target == memix::ViewTargetKind::FileTree);

  const memix::ViewResult<memix::ViewIr> ir = views.lower_to_ir(*expression);
  REQUIRE(ir.has_value());
  REQUIRE(ir->schema == "memix.view.select.v1");
  REQUIRE(ir->inputs.size() == 2);

  REQUIRE(ir->inputs[0].locator == "rules/repo/AGENTS.md@latest");
  REQUIRE(ir->inputs[0].type == memix::MemoryObjectType::Rules);
  REQUIRE(ir->inputs[0].path == fs::path("repo/AGENTS.md"));
  REQUIRE(ir->inputs[0].version.kind == memix::ViewVersionKind::Ref);
  REQUIRE(ir->inputs[0].version.value == "latest");
  REQUIRE_FALSE(ir->inputs[0].recursive);

  REQUIRE(ir->inputs[1].locator == "chat/task/2026-05-20/@latest");
  REQUIRE(ir->inputs[1].type == memix::MemoryObjectType::Chat);
  REQUIRE(ir->inputs[1].path == fs::path("task/2026-05-20"));
  REQUIRE(ir->inputs[1].recursive);

  const memix::ViewResult<std::string> ir_json = memix::view_ir_json(*ir);
  REQUIRE(ir_json.has_value());
  REQUIRE(ir_json->find("\"schema\":\"memix.view.select.v1\"")
          != std::string::npos);
  REQUIRE(ir_json->find("\"locator\":\"rules/repo/AGENTS.md@latest\"")
          != std::string::npos);
}

TEST_CASE("selection rejects the obsolete Environment-as-input model",
          "[view]") {
  TemporaryDirectory directory("typed-expression");
  const memix::ViewService views{memix::Store(directory.path() / "store")};
  const auto expression = views.parse_expression(
      R"({"schema":"memix.view.expr.v2","inputs":[)"
      R"({"kind":"context","locator":"rules/repo/AGENTS.md"},)"
      R"({"kind":"environment","environment":"abcd","output":"file_tree",)"
      R"("mount":"prior"}]})");
  REQUIRE_FALSE(expression);
  CHECK(expression.error().message.find("unsupported selection input kind")
        != std::string::npos);
}

TEST_CASE("selection expressions reject obsolete and ambiguous JSON",
          "[view]") {
  TemporaryDirectory directory("latest-expression");
  const memix::ViewService views{memix::Store(directory.path() / "store")};
  const std::vector<std::string> invalid{
      R"({"schema":"memix.view.expr.v1","sources":[]})",
      R"({"sources":["rules/a.md"]})",
      R"({"schema":"memix.view.expr.v2","sources":[]})",
      R"({"schema":"memix.view.expr.v2","inputs":[],"profile":"ignored"})",
      R"({"schema":"memix.view.expr.v2","inputs":[],"schema":"memix.view.expr.v2"})",
      R"({"schema":"memix.view.expr.v2","inputs":[{"kind":"context","locator":"rules/a.md","mount":"ignored"}]})",
      R"({"schema":"memix.view.expr.v2","inputs":[{"kind":"context","kind":"context","locator":"rules/a.md"}]})",
  };
  for (const std::string &json : invalid) {
    INFO(json);
    const auto parsed = views.parse_expression(json);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().kind == memix::ErrorKind::InvalidData);
  }
  memix::AuthoredViewExpression obsolete;
  obsolete.schema = "memix.view.expr.v1";
  const auto lowered = views.lower_to_ir(obsolete);
  REQUIRE_FALSE(lowered);
  CHECK(lowered.error().message.find("unsupported view expression schema")
        != std::string::npos);

  const auto plain = views.parse_expression(
      "rules/repo/AGENTS.md,chat/task/2026-05-20/@latest");
  REQUIRE(plain);
  CHECK(plain->schema == "memix.view.expr.v2");
  CHECK(plain->inputs.size() == 2);
  REQUIRE(views.lower_to_ir(*plain));
}

TEST_CASE("view expression lowering rejects invalid locators", "[view]") {
  TemporaryDirectory directory("invalid-locators");
  const memix::ViewService views{memix::Store(directory.path() / "store")};

  const auto unknown_type = views.lower_to_ir(
      *views.expression_from_sources({"unknown/repo/AGENTS.md"}));
  REQUIRE_FALSE(unknown_type.has_value());
  REQUIRE(unknown_type.error().kind == memix::ErrorKind::InvalidArgument);

  const auto parent_path = views.lower_to_ir(
      *views.expression_from_sources({"rules/repo/../AGENTS.md"}));
  REQUIRE_FALSE(parent_path.has_value());
  REQUIRE(parent_path.error().kind == memix::ErrorKind::InvalidArgument);

  const auto empty_path =
      views.lower_to_ir(*views.expression_from_sources({"rules/@latest"}));
  REQUIRE_FALSE(empty_path.has_value());
  REQUIRE(empty_path.error().kind == memix::ErrorKind::InvalidArgument);
}

TEST_CASE("view directory locators do not select an exact file path",
          "[view]") {
  TemporaryDirectory directory("directory-exact-file");
  const fs::path source = directory.path() / "repo";
  write_file(source, "not a directory\n");

  const memix::Store store(directory.path() / "store");
  ingest_context_file(store, memix::MemoryObjectType::Rules, source, "repo",
                      "add file");

  const memix::ViewService views(store);
  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      views.expression_from_sources({"rules/repo/"});
  REQUIRE(expression.has_value());

  const memix::ViewResult<memix::ViewEnvironment> environment =
      compose_for_test(views, *expression);
  REQUIRE_FALSE(environment.has_value());
  REQUIRE(environment.error().kind == memix::ErrorKind::ContextPathNotFound);
}

TEST_CASE("view optional file inputs skip missing paths", "[view]") {
  TemporaryDirectory directory("optional-file");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "present rules\n");

  const memix::Store store(directory.path() / "store");
  const memix::FileIngestResult ingest =
      ingest_context_file(store, memix::MemoryObjectType::Rules, source,
                          "repo/AGENTS.md", "present");

  memix::ViewIr ir;
  ir.inputs = {
      memix::ViewIrInput{
          .locator = "rules/repo/AGENTS.md@generation/000001",
          .type = memix::MemoryObjectType::Rules,
          .path = "repo/AGENTS.md",
          .version =
              memix::ViewVersionSelector{
                  .kind = memix::ViewVersionKind::Generation,
                  .value = ingest.generation.id,
              },
      },
      memix::ViewIrInput{
          .locator = "rules/repo/MISSING.md@generation/000001",
          .type = memix::MemoryObjectType::Rules,
          .path = "repo/MISSING.md",
          .version =
              memix::ViewVersionSelector{
                  .kind = memix::ViewVersionKind::Generation,
                  .value = ingest.generation.id,
              },
          .required = false,
      },
  };

  const memix::ViewResult<memix::ViewDerivation> derivation =
      memix::ViewService(store).build_ir(ir);
  REQUIRE(derivation.has_value());
  REQUIRE(derivation->resolved_files.size() == 1);
  REQUIRE(derivation->resolved_files[0].source_path
          == fs::path("repo/AGENTS.md"));
  REQUIRE(read_file(derivation->tree_path / "rules" / "repo" / "AGENTS.md")
          == "present rules\n");
  REQUIRE_FALSE(
      path_exists(derivation->tree_path / "rules" / "repo" / "MISSING.md"));
}

TEST_CASE("build_ir rejects non-selection shapes before touching the store",
          "[view][recipe]") {
  TemporaryDirectory directory("invalid-build-recipe");
  const memix::ViewService views{memix::Store(directory.path() / "absent")};
  memix::ViewIr recipe;
  SECTION("composition") { recipe.schema = "memix.view.compose.v1"; }
  SECTION("unknown schema") { recipe.schema = "memix.view.unknown.v1"; }
  SECTION("selection with composition bindings") {
    constexpr std::size_t kDigestLength = 64;
    recipe.bindings.push_back({
        .mount = "mounted",
        .derivation_id = std::string(kDigestLength, 'a'),
        .output_role = "file_tree",
    });
  }
  const auto result = views.build_ir(recipe);
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == memix::ErrorKind::InvalidArgument);
  CHECK_FALSE(path_exists(views.store().root()));
}

TEST_CASE("build_ir cannot publish a Derivation for a valid composition",
          "[view][recipe]") {
  TemporaryDirectory directory("composition-build-recipe");
  const memix::ViewService views{memix::Store(directory.path() / "store")};
  const auto selected = views.select(memix::AuthoredViewExpression{});
  REQUIRE(selected);
  const auto manifest = read_file(selected->manifest_path);
  memix::ViewIr recipe;
  recipe.schema = "memix.view.compose.v1";
  recipe.bindings.push_back({
      .mount = "mounted",
      .derivation_id = selected->id,
      .output_role = "file_tree",
  });

  const auto result = views.build_ir(recipe);
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == memix::ErrorKind::InvalidArgument);
  const auto derivations = views.list_derivations();
  REQUIRE(derivations);
  REQUIRE(derivations->size() == 1);
  CHECK(derivations->front().id == selected->id);
  CHECK(read_file(selected->manifest_path) == manifest);
  const auto quests = views.list_quests();
  REQUIRE(quests);
  CHECK(quests->size() == 1);
  const auto environments = views.list_environments();
  REQUIRE(environments);
  CHECK(environments->empty());
  CHECK(count_temp_directories(views.store().root() / "views/derivations")
        == 0);
}

TEST_CASE("selection publishes frozen directories and reuses them unchanged",
          "[view][publication]") {
  TemporaryDirectory directory("selection-publication");
  const auto source = directory.path() / "AGENTS.md";
  write_file(source, "repo rules\n");
  const memix::Store store(directory.path() / "store");
  ingest_context_file(store, memix::MemoryObjectType::Rules, source,
                      "repo/nested/AGENTS.md", "repo");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/nested/AGENTS.md"});
  REQUIRE(expression);

  // A first publication exercises the root rename on macOS as well as Linux.
  const auto first = views.select(*expression);
  INFO((first ? "" : first.error().message));
  REQUIRE(first);
  CHECK(first->inserted);
  const auto root = first->manifest_path.parent_path();
  for (const auto &path : {
           root,
           first->manifest_path,
           first->tree_path,
           first->tree_path / "rules",
           first->tree_path / "rules/repo",
           first->tree_path / "rules/repo/nested",
       }) {
    INFO(path.string());
    CHECK_FALSE(owner_can_write(path));
  }
  const auto manifest = read_file(first->manifest_path);
  const auto second = views.select(*expression);
  REQUIRE(second);
  CHECK_FALSE(second->inserted);
  CHECK(second->id == first->id);
  CHECK(read_file(second->manifest_path) == manifest);
  CHECK_FALSE(owner_can_write(root));
  CHECK(read_file(second->tree_path / "rules/repo/nested/AGENTS.md")
        == "repo rules\n");
  CHECK(count_temp_directories(root.parent_path()) == 0);
  CHECK(views.list_environments()->empty());
}

TEST_CASE("view file tree environment records file and directory closure",
          "[view]") {
  TemporaryDirectory directory("environment");
  const fs::path repo_source = directory.path() / "AGENTS.md";
  const fs::path chat_a_source = directory.path() / "chat-a.md";
  const fs::path chat_b_source = directory.path() / "chat-b.md";
  const fs::path other_source = directory.path() / "other.md";
  write_file(repo_source, "repo rules\n");
  write_file(chat_a_source, "chat a\n");
  write_file(chat_b_source, "chat b\n");
  write_file(other_source, "other\n");

  const memix::Store store(directory.path() / "store");
  ingest_context_file(store, memix::MemoryObjectType::Rules, repo_source,
                      "repo/AGENTS.md", "repo");
  ingest_context_file(store, memix::MemoryObjectType::Chat, chat_a_source,
                      "task/2026-05-20/a.md", "chat a");
  ingest_context_file(store, memix::MemoryObjectType::Chat, chat_b_source,
                      "task/2026-05-20/b.md", "chat b");
  ingest_context_file(store, memix::MemoryObjectType::Chat, other_source,
                      "task/2026-05-21/other.md", "other");

  const memix::ViewService views(store);
  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      views.expression_from_sources({
          "rules/repo/AGENTS.md",
          "chat/task/2026-05-20/",
      });
  REQUIRE(expression.has_value());

  const memix::ViewResult<memix::ViewEnvironment> first =
      compose_for_test(views, *expression);
  REQUIRE(first.has_value());
  REQUIRE(first->inserted);
  REQUIRE(first->bindings.size() == 1);
  REQUIRE(first->closure.derivations.size() == 1);
  REQUIRE(first->closure.artifacts.size() == 2);
  REQUIRE(first->closure.raw_objects.size() == 3);
  REQUIRE(first->derivations.at(0).outputs.size() == 2);
  REQUIRE(first->resolved_files.size() == 3);

  REQUIRE(
      read_file(
          first->derivations.at(0).tree_path / "rules" / "repo" / "AGENTS.md")
      == "repo rules\n");
  REQUIRE(read_file(first->derivations.at(0).tree_path
                    / "chat"
                    / "task"
                    / "2026-05-20"
                    / "a.md")
          == "chat a\n");
  REQUIRE(read_file(first->derivations.at(0).tree_path
                    / "chat"
                    / "task"
                    / "2026-05-20"
                    / "b.md")
          == "chat b\n");
  REQUIRE_FALSE(path_exists(first->derivations.at(0).tree_path
                            / "chat"
                            / "task"
                            / "2026-05-21"
                            / "other.md"));

  const std::string manifest =
      read_file(first->derivations.at(0).manifest_path);
  REQUIRE(manifest.find("\"schema\":\"memix.view.derivation.v4\"")
          != std::string::npos);
  REQUIRE(manifest.find("\"operator_version\":\"select-raw-files-v1\"")
          != std::string::npos);
  REQUIRE(manifest.find("\"resolved_inputs\"") != std::string::npos);
  REQUIRE_FALSE(owner_can_write(first->derivations.at(0).manifest_path));
  REQUIRE_FALSE(
      owner_can_write(first->derivations.at(0).tree_path.parent_path()));
  REQUIRE_FALSE(owner_can_write(first->derivations.at(0).tree_path));
  REQUIRE_FALSE(owner_can_write(first->derivations.at(0).tree_path / "rules"));
  REQUIRE_FALSE(owner_can_write(
      first->derivations.at(0).tree_path / "chat" / "task" / "2026-05-20"));

  const std::string environment_manifest = read_file(first->manifest_path);
  REQUIRE(environment_manifest.find("\"schema\":\"memix.view.environment.v3\"")
          != std::string::npos);
  REQUIRE(environment_manifest.find("\"raw_objects\"") != std::string::npos);
  REQUIRE_FALSE(owner_can_write(first->manifest_path));

  const memix::ViewResult<memix::ViewEnvironment> second =
      compose_for_test(views, *expression);
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second->inserted);
  REQUIRE(second->id == first->id);
  REQUIRE(second->derivations.at(0).id == first->derivations.at(0).id);

  const memix::ViewResult<memix::ViewEnvironment> resolved =
      views.resolve_environment(first->id.substr(0, 12));
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->id == first->id);
  REQUIRE(resolved->derivations.at(0).id == first->derivations.at(0).id);
  REQUIRE(resolved->derivations.at(0).outputs.size() == 2);
  REQUIRE(resolved->resolved_files.size() == 3);
  REQUIRE(read_file(resolved->derivations.at(0).tree_path
                    / "rules"
                    / "repo"
                    / "AGENTS.md")
          == "repo rules\n");

  const memix::ViewResult<std::vector<memix::ViewEnvironmentInfo>>
      environments = views.list_environments();
  REQUIRE(environments.has_value());
  REQUIRE(environments->size() == 1);
  REQUIRE((*environments)[0].id == first->id);
  REQUIRE((*environments)[0].file_count == 3);
  REQUIRE((*environments)[0].total_size_bytes == 25);

  const memix::ViewResult<std::vector<memix::ViewCatalogEntry>> catalog =
      views.environment_tree(first->id, memix::ViewCatalogOptions{
                                            .path = "chat/task",
                                            .depth = 2,
                                        });
  REQUIRE(catalog.has_value());
  REQUIRE(catalog->size() == 4);
  REQUIRE((*catalog)[0].path == fs::path("chat/task"));
  REQUIRE((*catalog)[0].directory);
  REQUIRE((*catalog)[0].file_count == 2);
  REQUIRE((*catalog)[1].path == fs::path("chat/task/2026-05-20"));
  REQUIRE((*catalog)[1].directory);

  const auto catalog_file = views.read_output(
      {.kind = memix::ViewObjectKind::Environment, .id = first->id},
      "chat/task/2026-05-20/a.md");
  REQUIRE(catalog_file);
  REQUIRE(catalog_file->contents == "chat a\n");

  const std::string copied_environment_id(64, 'f');
  const fs::path copied_environment_path =
      first->manifest_path.parent_path()
      / std::format("{}.json", copied_environment_id);
  REQUIRE(fs::copy_file(first->manifest_path, copied_environment_path)
              .value_or(false));
  const memix::ViewResult<memix::ViewEnvironment> mismatched =
      views.resolve_environment(copied_environment_id);
  REQUIRE_FALSE(mismatched.has_value());
  REQUIRE(mismatched.error().kind == memix::ErrorKind::InvalidData);
}

TEST_CASE(
    "environments mount independent derivations without a synthetic owner",
    "[view][d2]") {
  TemporaryDirectory directory("direct-composition");
  const auto source = directory.path() / "input.md";
  const memix::Store store(directory.path() / "store");
  const memix::ViewService views(store);
  write_file(source, "rules\n");
  ingest_context_file(store, memix::MemoryObjectType::Rules, source,
                      "repo/AGENTS.md", "rules");
  write_file(source, "design\n");
  ingest_context_file(store, memix::MemoryObjectType::Documents, source,
                      "design.md", "design");
  const auto a =
      views.select(*views.expression_from_sources({"rules/repo/AGENTS.md"}));
  const auto b =
      views.select(*views.expression_from_sources({"documents/design.md"}));
  REQUIRE(a);
  REQUIRE(b);
  // Golden select-v1 recipe identity; authoring syntax is not part of the
  // basis.
  CHECK(a->ir_hash
        == "4945d5d145b26d670dc512e494cf1d11336b6d2fa10e7c7fa514cf768e457c55");
  REQUIRE(views.list_environments()->empty());
  const auto original = read_file(a->manifest_path);
  const auto direct = views.read_output(
      {.kind = memix::ViewObjectKind::Derivation, .id = a->id},
      "rules/repo/AGENTS.md");
  REQUIRE(direct);
  CHECK(direct->contents == "rules\n");
  const std::vector<memix::ViewEnvironmentBinding> bindings{
      {.mount = "prior", .derivation_id = a->id, .output_role = "file_tree"},
      {.mount = {}, .derivation_id = b->id, .output_role = "file_tree"},
      {.mount = "again", .derivation_id = a->id, .output_role = "file_tree"}};
  const auto environment = views.compose(
      bindings, {.intent = "composition guidance", .name = "assembly"});
  REQUIRE(environment);
  CHECK(environment->bindings.size() == 3);
  CHECK(environment->derivations.size() == 2);
  CHECK(environment->resolved_files.size() == 3);
  CHECK(environment->closure.environments.empty());
  CHECK(environment->closure.derivations.size() == 2);
  CHECK(environment->closure.raw_objects.size() == 2);
  REQUIRE(views.list_derivations()->size() == 2);
  CHECK(read_file(a->manifest_path) == original);
  auto reversed = bindings;
  std::ranges::reverse(reversed);
  const auto repeated = views.compose(reversed);
  REQUIRE(repeated);
  CHECK(repeated->id == environment->id);
  CHECK_FALSE(repeated->inserted);
  const auto read = views.read_output(
      {.kind = memix::ViewObjectKind::Environment, .id = environment->id},
      "prior/rules/repo/AGENTS.md");
  REQUIRE(read);
  CHECK(read->contents == direct->contents);
  const auto changed = views.compose(
      bindings, {.intent = "changed composition guidance", .name = "assembly"});
  REQUIRE(changed);
  CHECK(changed->id != environment->id);
  CHECK(changed->ir_hash == environment->ir_hash);
  CHECK(read_file(a->manifest_path) == original);
  auto moved = bindings;
  moved[0].mount = "moved";
  const auto remounted = views.compose(moved);
  REQUIRE(remounted);
  CHECK(remounted->id != environment->id);
  CHECK(remounted->ir_hash != environment->ir_hash);
  CHECK(views.list_derivations()->size() == 2);
  CHECK_FALSE(views.compose({{.mount = {},
                              .derivation_id = environment->id,
                              .output_role = "file_tree"}}));
  const auto listing = views.list_environments();
  REQUIRE(listing);
  const auto found = std::ranges::find(*listing, environment->id,
                                       &memix::ViewEnvironmentInfo::id);
  REQUIRE(found != listing->end());
  CHECK(found->binding_count == 3);
  CHECK(found->file_count == 3);
  CHECK(found->total_size_bytes == 19);
  CHECK(found->upstream_environment_count == 0);
}

TEST_CASE(
    "composition rejects collisions unsupported outputs and unsafe mounts",
    "[view][d2]") {
  TemporaryDirectory directory("collision");
  const auto source = directory.path() / "input";
  const memix::Store store(directory.path() / "store");
  const memix::ViewService views(store);
  write_file(source, "same bytes\n");
  ingest_context_file(store, memix::MemoryObjectType::Rules, source, "a.md",
                      "rules");
  const auto expression = views.expression_from_sources({"rules/a.md"});
  REQUIRE(expression);
  const auto a = views.select(*expression, {.intent = "a", .name = {}});
  const auto b = views.select(*expression, {.intent = "b", .name = {}});
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(a->id != b->id);
  const auto collided = views.compose(
      {{.mount = {}, .derivation_id = a->id, .output_role = "file_tree"},
       {.mount = {}, .derivation_id = b->id, .output_role = "file_tree"}});
  REQUIRE_FALSE(collided);
  CHECK(collided.error().kind == memix::ErrorKind::Conflict);
  CHECK_FALSE(views.compose(
      {{.mount = {}, .derivation_id = a->id, .output_role = "file_tree"},
       {.mount = ".", .derivation_id = a->id, .output_role = "file_tree"}}));
  CHECK_FALSE(views.compose(
      {{.mount = {}, .derivation_id = a->id, .output_role = "file_tree"},
       {.mount = "rules/a.md",
        .derivation_id = b->id,
        .output_role = "file_tree"}}));
  for (const auto *mount : {"/absolute", "../escape", "a/../b", "a//b"}) {
    CHECK_FALSE(views.compose({{.mount = mount,
                                .derivation_id = a->id,
                                .output_role = "file_tree"}}));
  }
  CHECK_FALSE(views.compose({}));
  CHECK_FALSE(views.compose(
      {{.mount = {}, .derivation_id = a->id, .output_role = "context_pack"}}));
  CHECK_FALSE(views.compose(
      {{.mount = {}, .derivation_id = "missing", .output_role = "file_tree"}}));
  CHECK(views.list_environments()->empty());
  const auto root = views.compose(
      {{.mount = {}, .derivation_id = a->id, .output_role = "file_tree"}});
  const auto dot = views.compose(
      {{.mount = ".", .derivation_id = a->id, .output_role = "file_tree"}});
  REQUIRE(root);
  REQUIRE(dot);
  CHECK(dot->id == root->id);
}

TEST_CASE("concurrent view builds report one insertion and clean temps",
          "[view]") {
  constexpr int kBuilderCount = 16;

  TemporaryDirectory directory("concurrent-environment");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "repo rules\n");

  const memix::Store store(directory.path() / "store");
  ingest_context_file(store, memix::MemoryObjectType::Rules, source,
                      "repo/AGENTS.md", "repo");

  const memix::ViewService views(store);
  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      views.expression_from_sources({"rules/repo/AGENTS.md"});
  REQUIRE(expression.has_value());

  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  std::array<memix::ViewResult<memix::ViewEnvironment>, kBuilderCount>
      results{};
  std::vector<std::thread> builders;
  builders.reserve(kBuilderCount);

  for (int index = 0; index < kBuilderCount; ++index) {
    builders.emplace_back([&, index]() {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const memix::ViewService thread_views{memix::Store(store.root())};
      results.at(static_cast<std::size_t>(index)) =
          compose_for_test(thread_views, *expression);
    });
  }

  while (ready.load(std::memory_order_acquire) != kBuilderCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);

  for (std::thread &builder : builders) {
    builder.join();
  }

  std::size_t environment_insertions = 0;
  std::string environment_id;
  std::string derivation_id;
  for (const memix::ViewResult<memix::ViewEnvironment> &result : results) {
    const std::string error_message = result ? "" : result.error().message;
    INFO(error_message);
    REQUIRE(result.has_value());
    environment_insertions += result->inserted ? 1U : 0U;
    if (environment_id.empty()) {
      environment_id = result->id;
      derivation_id = result->derivations.at(0).id;
    }
    REQUIRE(result->id == environment_id);
    REQUIRE(result->derivations.at(0).id == derivation_id);
    const auto &derivation = result->derivations.at(0);
    CHECK_FALSE(owner_can_write(derivation.manifest_path.parent_path()));
    CHECK_FALSE(owner_can_write(derivation.manifest_path));
    CHECK_FALSE(owner_can_write(derivation.tree_path / "rules/repo"));
  }

  REQUIRE(environment_insertions == 1);
  REQUIRE(views.list_derivations()->size() == 1);
  REQUIRE(views.list_environments()->size() == 1);
  REQUIRE(count_temp_directories(store.root() / "views" / "derivations") == 0);
  REQUIRE(count_temp_directories(store.root() / "views" / "environments") == 0);
}

TEST_CASE("view latest source changes produce a new derivation", "[view]") {
  TemporaryDirectory directory("latest-changes");
  const fs::path legacy_source = directory.path() / "legacy.md";
  const fs::path modern_source = directory.path() / "modern.md";
  write_file(legacy_source, "pip workflow\n");
  write_file(modern_source, "uv workflow\n");

  const memix::Store store(directory.path() / "store");
  const memix::FileIngestResult legacy =
      ingest_context_file(store, memix::MemoryObjectType::Rules, legacy_source,
                          "repo/AGENTS.md", "legacy");

  const memix::ViewService views(store);
  const memix::ViewResult<memix::AuthoredViewExpression> latest_expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(latest_expression.has_value());
  const memix::ViewResult<memix::ViewEnvironment> first =
      compose_for_test(views, *latest_expression);
  REQUIRE(first.has_value());

  ingest_context_file(store, memix::MemoryObjectType::Rules, modern_source,
                      "repo/AGENTS.md", "modern");
  const memix::ViewResult<memix::ViewEnvironment> second =
      compose_for_test(views, *latest_expression);
  REQUIRE(second.has_value());
  REQUIRE(second->id != first->id);
  REQUIRE(second->derivations.at(0).id != first->derivations.at(0).id);
  REQUIRE(
      read_file(
          first->derivations.at(0).tree_path / "rules" / "repo" / "AGENTS.md")
      == "pip workflow\n");
  REQUIRE(
      read_file(
          second->derivations.at(0).tree_path / "rules" / "repo" / "AGENTS.md")
      == "uv workflow\n");

  const memix::ViewResult<memix::AuthoredViewExpression> pinned_expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@generation/000001"});
  REQUIRE(pinned_expression.has_value());
  const memix::ViewResult<memix::ViewEnvironment> pinned =
      compose_for_test(views, *pinned_expression);
  REQUIRE(pinned.has_value());
  REQUIRE(
      read_file(
          pinned->derivations.at(0).tree_path / "rules" / "repo" / "AGENTS.md")
      == "pip workflow\n");
  REQUIRE(pinned->resolved_files[0].source_generation == legacy.generation.id);
}

TEST_CASE("view delivery verifies payloads instead of trusting cached trees",
          "[view][d2]") {
  TemporaryDirectory directory("verified-delivery");
  const auto source = directory.path() / "input";
  const auto root = directory.path() / "store";
  const memix::Store store(root);
  const memix::ViewService views(store);
  write_file(source, "good\n");
  ingest_context_file(store, memix::MemoryObjectType::Rules, source, "a.md",
                      "rules");
  const auto expression = views.expression_from_sources({"rules/a.md"});
  REQUIRE(expression);
  const auto d = views.select(*expression);
  REQUIRE(d);
  const auto e = views.compose({{.mount = "mounted",
                                 .derivation_id = d->id,
                                 .output_role = "file_tree"}});
  REQUIRE(e);
  const auto cached = d->tree_path / "rules/a.md";
  REQUIRE(fs::permissions(cached.parent_path(), fs::perms::owner_all));
  REQUIRE(fs::remove(cached).value_or(false));
  write_file(source, "not the blob\n");
  REQUIRE(fs::create_symlink(source, cached));
  const memix::ViewReference direct{.kind = memix::ViewObjectKind::Derivation,
                                    .id = d->id};
  const memix::ViewReference assembled{
      .kind = memix::ViewObjectKind::Environment, .id = e->id};
  REQUIRE(views.read_output(direct, "rules/a.md")->contents == "good\n");
  REQUIRE(views.read_output(assembled, "mounted/rules/a.md")->contents
          == "good\n");
  const auto workspace = directory.path() / "copy";
  REQUIRE(views.activate(e->id, {.profile = "copy",
                                 .into = workspace,
                                 .mode = memix::ActivationMode::Copy}));
  REQUIRE(read_file(workspace / "mounted/rules/a.md") == "good\n");
  const auto &blob = d->resolved_files.front().blob;
  const auto payload = blob.object_dir / blob.content_path;
  REQUIRE(fs::permissions(payload, fs::perms::owner_all));
  SECTION("same-size corruption") {
    // Hash checks, not just size checks, must reject the modified payload.
    write_file(payload, "evil\n");
  }
  SECTION("truncated payload") { write_file(payload, "bad"); }
  REQUIRE_FALSE(views.resolve_environment(e->id));
  REQUIRE_FALSE(views.list_environments());
  REQUIRE_FALSE(views.view_ls(assembled));
  REQUIRE_FALSE(views.view_tree(assembled));
  REQUIRE_FALSE(views.read_output(direct, "rules/a.md"));
  REQUIRE_FALSE(views.read_output(assembled, "mounted/rules/a.md"));
  REQUIRE_FALSE(views.select(*expression));
  REQUIRE_FALSE(views.compose(
      {{.mount = "new", .derivation_id = d->id, .output_role = "file_tree"}}));
  REQUIRE_FALSE(views.activate(e->id, {.profile = "copy",
                                       .into = workspace,
                                       .mode = memix::ActivationMode::Copy}));
  CHECK(read_file(workspace / "mounted/rules/a.md") == "good\n");
  CHECK(views.quest_stats(d->ir_hash)->read_count == 1);
  CHECK(views.quest_stats(e->ir_hash)->read_count == 1);
  CHECK(views.quest_stats(d->ir_hash)->resolution_count == 1);
}

TEST_CASE(
    "latest manifests reject forged closure ownership and unsupported payloads",
    "[view][d2]") {
  TemporaryDirectory directory("manifest-validation");
  const auto source = directory.path() / "input";
  const memix::Store store(directory.path() / "store");
  const memix::ViewService views(store);
  write_file(source, "payload\n");
  const auto ingest = ingest_context_file(store, memix::MemoryObjectType::Rules,
                                          source, "a.md", "rules");
  const auto d = views.select(*views.expression_from_sources({"rules/a.md"}));
  REQUIRE(d);
  const auto e = views.compose(
      {{.mount = {}, .derivation_id = d->id, .output_role = "file_tree"}});
  REQUIRE(e);
  SECTION("closure must be exact even though derived closure is not in the E "
          "basis") {
    auto changed = *e;
    changed.closure.raw_objects.push_back(std::string(64, 'f'));
    const auto text = memix::view_environment_json(changed);
    REQUIRE(text);
    REQUIRE(fs::permissions(e->manifest_path, fs::perms::owner_all));
    write_file(e->manifest_path, *text);
    const auto rejected = views.resolve_environment(e->id);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.contains("closure"));
  }
  SECTION("output artifact must belong to its directly bound D") {
    auto changed = *d;
    changed.outputs.front().artifact = std::string(64, 'f');
    const auto text = memix::view_derivation_json(changed);
    REQUIRE(text);
    REQUIRE(fs::permissions(d->manifest_path, fs::perms::owner_all));
    write_file(d->manifest_path, *text);
    CHECK_FALSE(views.resolve_derivation(d->id));
    CHECK_FALSE(views.resolve_environment(e->id));
  }
  SECTION("unsupported external output does not fall back to raw data") {
    auto text = read_file(d->manifest_path);
    const auto at = text.find("\"kind\":\"raw_blob\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("\"kind\":\"raw_blob\"").size(),
                 "\"kind\":\"local\"");
    REQUIRE(fs::permissions(d->manifest_path, fs::perms::owner_all));
    write_file(d->manifest_path, text);
    CHECK_FALSE(views.resolve_derivation(d->id));
    CHECK(read_file(d->manifest_path) == text);
  }
  SECTION("unknown root fields are unsupported") {
    auto text = read_file(d->manifest_path);
    text.insert(1, "\"future_field\":true,");
    REQUIRE(fs::permissions(d->manifest_path, fs::perms::owner_all));
    write_file(d->manifest_path, text);
    CHECK_FALSE(views.resolve_derivation(d->id));
  }
  SECTION(
      "nested unknown fields must not disappear during identity verification") {
    auto text = read_file(d->manifest_path);
    const auto at = text.find("\"resolved_inputs\":[{");
    REQUIRE(at != std::string::npos);
    text.insert(at + std::string_view("\"resolved_inputs\":[{").size(),
                "\"unhashed\":true,");
    REQUIRE(fs::permissions(d->manifest_path, fs::perms::owner_all));
    write_file(d->manifest_path, text);
    CHECK_FALSE(views.resolve_derivation(d->id));
  }
  SECTION("missing D cannot be substituted by its cached tree or a closure "
          "member") {
    REQUIRE(
        fs::permissions(d->manifest_path.parent_path(), fs::perms::owner_all));
    REQUIRE(fs::remove(d->manifest_path).value_or(false));
    CHECK_FALSE(views.resolve_environment(e->id));
  }
}
