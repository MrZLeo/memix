#include "memix/view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
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

  ~TemporaryDirectory() { static_cast<void>(fs::remove_all(path_)); }

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
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
      views.parse_expression("{\"sources\":[\"agents/repo/AGENTS.md\","
                             "\"chat/task/2026-05-20/@latest\"]}");
  REQUIRE(expression.has_value());
  REQUIRE(expression->schema == "memix.view.expr.v1");
  REQUIRE(expression->target == memix::ViewTargetKind::FileTree);

  const memix::ViewResult<memix::ViewIr> ir = views.lower_to_ir(*expression);
  REQUIRE(ir.has_value());
  REQUIRE(ir->schema == "memix.view.ir.v1");
  REQUIRE(ir->inputs.size() == 2);

  REQUIRE(ir->inputs[0].locator == "agents/repo/AGENTS.md@latest");
  REQUIRE(ir->inputs[0].type == memix::MemoryObjectType::Agents);
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
  REQUIRE(ir_json->find("\"schema\":\"memix.view.ir.v1\"")
          != std::string::npos);
  REQUIRE(ir_json->find("\"locator\":\"agents/repo/AGENTS.md@latest\"")
          != std::string::npos);
}

TEST_CASE("view expression lowering rejects invalid locators", "[view]") {
  TemporaryDirectory directory("invalid-locators");
  const memix::ViewService views{memix::Store(directory.path() / "store")};

  const auto unknown_type = views.lower_to_ir(
      *views.expression_from_sources({"unknown/repo/AGENTS.md"}));
  REQUIRE_FALSE(unknown_type.has_value());
  REQUIRE(unknown_type.error().kind == memix::ErrorKind::InvalidArgument);

  const auto parent_path = views.lower_to_ir(
      *views.expression_from_sources({"agents/repo/../AGENTS.md"}));
  REQUIRE_FALSE(parent_path.has_value());
  REQUIRE(parent_path.error().kind == memix::ErrorKind::InvalidArgument);

  const auto empty_path =
      views.lower_to_ir(*views.expression_from_sources({"agents/@latest"}));
  REQUIRE_FALSE(empty_path.has_value());
  REQUIRE(empty_path.error().kind == memix::ErrorKind::InvalidArgument);
}

TEST_CASE("view directory locators do not select an exact file path",
          "[view]") {
  TemporaryDirectory directory("directory-exact-file");
  const fs::path source = directory.path() / "repo";
  write_file(source, "not a directory\n");

  const memix::Store store(directory.path() / "store");
  ingest_context_file(store, memix::MemoryObjectType::Agents, source, "repo",
                      "add file");

  const memix::ViewService views(store);
  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      views.expression_from_sources({"agents/repo/"});
  REQUIRE(expression.has_value());

  const memix::ViewResult<memix::ViewEnvironment> environment =
      views.build(*expression);
  REQUIRE_FALSE(environment.has_value());
  REQUIRE(environment.error().kind == memix::ErrorKind::ContextPathNotFound);
}

TEST_CASE("view optional file inputs skip missing paths", "[view]") {
  TemporaryDirectory directory("optional-file");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "present rules\n");

  const memix::Store store(directory.path() / "store");
  const memix::FileIngestResult ingest =
      ingest_context_file(store, memix::MemoryObjectType::Agents, source,
                          "repo/AGENTS.md", "present");

  memix::ViewIr ir;
  ir.inputs = {
      memix::ViewIrInput{
          .locator = "agents/repo/AGENTS.md@generation/000001",
          .type = memix::MemoryObjectType::Agents,
          .path = "repo/AGENTS.md",
          .version =
              memix::ViewVersionSelector{
                  .kind = memix::ViewVersionKind::Generation,
                  .value = ingest.generation.id,
              },
      },
      memix::ViewIrInput{
          .locator = "agents/repo/MISSING.md@generation/000001",
          .type = memix::MemoryObjectType::Agents,
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
  REQUIRE(read_file(derivation->tree_path / "agents" / "repo" / "AGENTS.md")
          == "present rules\n");
  REQUIRE_FALSE(
      path_exists(derivation->tree_path / "agents" / "repo" / "MISSING.md"));
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
  ingest_context_file(store, memix::MemoryObjectType::Agents, repo_source,
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
          "agents/repo/AGENTS.md",
          "chat/task/2026-05-20/",
      });
  REQUIRE(expression.has_value());

  const memix::ViewResult<memix::ViewEnvironment> first =
      views.build(*expression);
  REQUIRE(first.has_value());
  REQUIRE(first->inserted);
  REQUIRE(first->bindings.size() == 1);
  REQUIRE(first->closure.derivations.size() == 1);
  REQUIRE(first->closure.artifacts.size() == 2);
  REQUIRE(first->closure.raw_objects.size() == 3);
  REQUIRE(first->derivation.outputs.size() == 2);
  REQUIRE(first->derivation.resolved_files.size() == 3);

  REQUIRE(
      read_file(first->derivation.tree_path / "agents" / "repo" / "AGENTS.md")
      == "repo rules\n");
  REQUIRE(
      read_file(
          first->derivation.tree_path / "chat" / "task" / "2026-05-20" / "a.md")
      == "chat a\n");
  REQUIRE(
      read_file(
          first->derivation.tree_path / "chat" / "task" / "2026-05-20" / "b.md")
      == "chat b\n");
  REQUIRE_FALSE(path_exists(first->derivation.tree_path
                            / "chat"
                            / "task"
                            / "2026-05-21"
                            / "other.md"));

  const std::string manifest = read_file(first->derivation.manifest_path);
  REQUIRE(manifest.find("\"schema\":\"memix.view.derivation.v1\"")
          != std::string::npos);
  REQUIRE(manifest.find("\"policy_snapshot\":null") != std::string::npos);
  REQUIRE(manifest.find("\"resolved_inputs\"") != std::string::npos);
  REQUIRE_FALSE(owner_can_write(first->derivation.manifest_path));
  REQUIRE_FALSE(owner_can_write(first->derivation.tree_path.parent_path()));
  REQUIRE_FALSE(owner_can_write(first->derivation.tree_path));
  REQUIRE_FALSE(owner_can_write(first->derivation.tree_path / "agents"));
  REQUIRE_FALSE(owner_can_write(
      first->derivation.tree_path / "chat" / "task" / "2026-05-20"));

  const std::string environment_manifest = read_file(first->manifest_path);
  REQUIRE(environment_manifest.find("\"schema\":\"memix.view.environment.v1\"")
          != std::string::npos);
  REQUIRE(environment_manifest.find("\"raw_objects\"") != std::string::npos);
  REQUIRE_FALSE(owner_can_write(first->manifest_path));

  const memix::ViewResult<memix::ViewEnvironment> second =
      views.build(*expression);
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second->inserted);
  REQUIRE(second->id == first->id);
  REQUIRE(second->derivation.id == first->derivation.id);

  const memix::ViewResult<memix::ViewEnvironment> resolved =
      views.resolve_environment(first->id.substr(0, 12));
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->id == first->id);
  REQUIRE(resolved->derivation.id == first->derivation.id);
  REQUIRE(resolved->derivation.outputs.size() == 2);
  REQUIRE(resolved->derivation.resolved_files.size() == 3);
  REQUIRE(read_file(
              resolved->derivation.tree_path / "agents" / "repo" / "AGENTS.md")
          == "repo rules\n");

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

TEST_CASE("concurrent view builds report one insertion and clean temps",
          "[view]") {
  constexpr int kBuilderCount = 16;

  TemporaryDirectory directory("concurrent-environment");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "repo rules\n");

  const memix::Store store(directory.path() / "store");
  ingest_context_file(store, memix::MemoryObjectType::Agents, source,
                      "repo/AGENTS.md", "repo");

  const memix::ViewService views(store);
  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      views.expression_from_sources({"agents/repo/AGENTS.md"});
  REQUIRE(expression.has_value());

  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  std::vector<memix::ViewResult<memix::ViewEnvironment>> results(kBuilderCount);
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
          thread_views.build(*expression);
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
  std::size_t derivation_insertions = 0;
  std::string environment_id;
  std::string derivation_id;
  for (const memix::ViewResult<memix::ViewEnvironment> &result : results) {
    const std::string error_message = result ? "" : result.error().message;
    INFO(error_message);
    REQUIRE(result.has_value());
    environment_insertions += result->inserted ? 1U : 0U;
    derivation_insertions += result->derivation.inserted ? 1U : 0U;
    if (environment_id.empty()) {
      environment_id = result->id;
      derivation_id = result->derivation.id;
    }
    REQUIRE(result->id == environment_id);
    REQUIRE(result->derivation.id == derivation_id);
  }

  REQUIRE(environment_insertions == 1);
  REQUIRE(derivation_insertions == 1);
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
      ingest_context_file(store, memix::MemoryObjectType::Agents, legacy_source,
                          "repo/AGENTS.md", "legacy");

  const memix::ViewService views(store);
  const memix::ViewResult<memix::AuthoredViewExpression> latest_expression =
      views.expression_from_sources({"agents/repo/AGENTS.md@latest"});
  REQUIRE(latest_expression.has_value());
  const memix::ViewResult<memix::ViewEnvironment> first =
      views.build(*latest_expression);
  REQUIRE(first.has_value());

  ingest_context_file(store, memix::MemoryObjectType::Agents, modern_source,
                      "repo/AGENTS.md", "modern");
  const memix::ViewResult<memix::ViewEnvironment> second =
      views.build(*latest_expression);
  REQUIRE(second.has_value());
  REQUIRE(second->id != first->id);
  REQUIRE(second->derivation.id != first->derivation.id);
  REQUIRE(
      read_file(first->derivation.tree_path / "agents" / "repo" / "AGENTS.md")
      == "pip workflow\n");
  REQUIRE(
      read_file(second->derivation.tree_path / "agents" / "repo" / "AGENTS.md")
      == "uv workflow\n");

  const memix::ViewResult<memix::AuthoredViewExpression> pinned_expression =
      views.expression_from_sources(
          {"agents/repo/AGENTS.md@generation/000001"});
  REQUIRE(pinned_expression.has_value());
  const memix::ViewResult<memix::ViewEnvironment> pinned =
      views.build(*pinned_expression);
  REQUIRE(pinned.has_value());
  REQUIRE(
      read_file(pinned->derivation.tree_path / "agents" / "repo" / "AGENTS.md")
      == "pip workflow\n");
  REQUIRE(pinned->derivation.resolved_files[0].source_generation
          == legacy.generation.id);
}
