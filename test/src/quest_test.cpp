#include "memix/error.hpp"
#include "memix/view.hpp"
#include "view_test_helpers.hpp"

#include "detail/file_lock.hpp"
#include "detail/hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = memix::fs;

namespace {

fs::path unique_test_path(std::string_view label) {
  std::random_device random_device;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const memix::FsResult<fs::path> temp_dir = fs::temp_directory_path();
  REQUIRE(temp_dir);
  return *temp_dir
       / std::format("memix-quest-{}-{}-{}", label, nanos, random_device());
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

void append_file(const fs::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::app);
  REQUIRE(output);
  output << contents;
  REQUIRE(output);
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

std::string read_cli_text_file(const fs::path &path) {
  std::ifstream input(path);
  REQUIRE(input);
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

memix::FileIngestResult
ingest_context_file(const memix::Store &store, const fs::path &source,
                    const fs::path &context_path, std::string_view message) {
  const memix::StoreResult<memix::FileIngestResult> result = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = source,
      .context_path = context_path,
      .message = message,
  });
  REQUIRE(result);
  return *result;
}

std::size_t
history_line_count(const fs::path &store_root, std::string_view ir_hash) {
  const std::string history =
      read_file(store_root / "views" / "quests" / ir_hash / "history.jsonl");
  return static_cast<std::size_t>(std::ranges::count(history, '\n'));
}

fs::path receipts_path(const fs::path &store_root, std::string_view ir_hash) {
  return store_root / "views" / "quests" / ir_hash / "receipts.jsonl";
}

std::vector<std::string> jsonl_lines(const fs::path &path) {
  const std::string contents = read_file(path);
  std::vector<std::string> lines;
  std::string_view remaining = contents;
  while (!remaining.empty()) {
    const std::size_t end = remaining.find('\n');
    const std::string_view line = remaining.substr(0, end);
    if (!line.empty()) {
      lines.emplace_back(line);
    }
    if (end == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(end + 1);
  }
  return lines;
}

std::string json_string_field(std::string_view json, std::string_view key) {
  const std::string prefix = std::format(R"("{}":")", key);
  const std::size_t begin = json.find(prefix);
  REQUIRE(begin != std::string_view::npos);
  const std::size_t value_begin = begin + prefix.size();
  const std::size_t end = json.find('"', value_begin);
  REQUIRE(end != std::string_view::npos);
  return std::string(json.substr(value_begin, end - value_begin));
}

void erase_json_array_field(std::string &json, std::string_view key) {
  const std::string prefix = std::format(R"(,"{}":[)", key);
  const std::size_t begin = json.find(prefix);
  REQUIRE(begin != std::string::npos);
  const std::size_t array_begin = begin + prefix.size() - 1;
  std::size_t depth = 0;
  for (std::size_t index = array_begin; index < json.size(); ++index) {
    if (json[index] == '[') {
      ++depth;
    } else if (json[index] == ']') {
      REQUIRE(depth > 0);
      --depth;
      if (depth == 0) {
        json.erase(begin, index - begin + 1);
        return;
      }
    }
  }
  FAIL("unterminated JSON array");
}

const memix::QuestInfo &
quest_with_hash(const std::vector<memix::QuestInfo> &quests,
                std::string_view ir_hash) {
  const auto found =
      std::ranges::find(quests, ir_hash, &memix::QuestInfo::ir_hash);
  REQUIRE(found != quests.end());
  return *found;
}

std::string shell_quote(std::string_view value) {
#ifdef _WIN32
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('"');
#else
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
#endif
  return quoted;
}

struct CliResult {
  int status = 0;
  std::string output;
  std::string error;
};

CliResult run_cli(const fs::path &directory, const fs::path &store_root,
                  const std::vector<std::string> &arguments) {
  const fs::path output_path = directory / "cli-stdout.txt";
  const fs::path error_path = directory / "cli-stderr.txt";
  std::string_view binary = MEMIX_TEST_BINARY;
  if (const char *override_binary = std::getenv("MEMIX_TEST_BINARY_OVERRIDE");
      override_binary != nullptr && override_binary[0] != '\0') {
    binary = override_binary;
  }
  std::string command = shell_quote(binary);
  command += " view --store-root ";
  command += shell_quote(store_root.string());
  for (const std::string &argument : arguments) {
    command.push_back(' ');
    command += shell_quote(argument);
  }
  command += " >";
  command += shell_quote(output_path.string());
  command += " 2>";
  command += shell_quote(error_path.string());
#ifdef _WIN32
  command.insert(command.begin(), '"');
  command.push_back('"');
#endif
  const int status = std::system(command.c_str()); // NOLINT(cert-env33-c)
  return CliResult{
      .status = status,
      .output = read_cli_text_file(output_path),
      .error = read_cli_text_file(error_path),
  };
}

struct QuestFixture {
  TemporaryDirectory directory{"d2"};
  fs::path root = directory.path() / "store";
  fs::path source = directory.path() / "input";
  memix::Store store{root};
  memix::ViewService views{store};
  memix::AuthoredViewExpression expression;

  QuestFixture() {
    write_file(source, "one\n");
    ingest_context_file(store, source, "repo/AGENTS.md", "initial");
    auto parsed = views.expression_from_sources({"rules/repo/AGENTS.md"});
    REQUIRE(parsed);
    expression = std::move(*parsed);
  }

  memix::ViewDerivation select(std::string intent = {}) {
    auto result =
        views.select(expression, {.intent = std::move(intent), .name = {}});
    REQUIRE(result);
    return std::move(*result);
  }

  memix::ViewEnvironment compose(const memix::ViewDerivation &d) {
    auto result = views.compose(
        {{.mount = {}, .derivation_id = d.id, .output_role = "file_tree"}});
    REQUIRE(result);
    return std::move(*result);
  }

  void update(std::string_view contents) {
    write_file(source, contents);
    ingest_context_file(store, source, "repo/AGENTS.md", "updated");
  }
};

memix::ViewReference reference(const memix::ViewDerivation &d) {
  return {.kind = memix::ViewObjectKind::Derivation, .id = d.id};
}

memix::ViewReference reference(const memix::ViewEnvironment &e) {
  return {.kind = memix::ViewObjectKind::Environment, .id = e.id};
}

} // namespace

TEST_CASE("quest definition persists canonical IR and updates annotations",
          "[quest]") {
  TemporaryDirectory directory("define");
  const fs::path store_root = directory.path() / "store";
  const memix::ViewService views{memix::Store(store_root)};
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);

  const auto first = views.define_quest(*expression, "Repository guidance", "");
  REQUIRE(first);
  REQUIRE_FALSE(first->existed);
  const fs::path ir_path = first->quest_dir / "ir.json";
  const std::string ir_json = read_file(ir_path);
  REQUIRE(memix::detail::sha256_text(ir_json) == first->ir_hash);

  const auto initial_quests = views.list_quests();
  REQUIRE(initial_quests);
  const memix::QuestInfo &initial =
      quest_with_hash(*initial_quests, first->ir_hash);
  REQUIRE(initial.intent == "Repository guidance");
  REQUIRE(initial.name == "rules/repo/AGENTS.md@latest");
  const std::string created_at = initial.created_at;
  REQUIRE_FALSE(created_at.empty());

  const auto second =
      views.define_quest(*expression, "Updated guidance", "repo rules");
  REQUIRE(second);
  REQUIRE(second->existed);
  const auto updated_quests = views.list_quests();
  REQUIRE(updated_quests);
  const memix::QuestInfo &updated =
      quest_with_hash(*updated_quests, first->ir_hash);
  REQUIRE(updated.intent == "Updated guidance");
  REQUIRE(updated.name == "repo rules");
  REQUIRE(updated.created_at == created_at);

  const auto omitted = views.define_quest(*expression, "", "");
  REQUIRE(omitted);
  const auto preserved_quests = views.list_quests();
  REQUIRE(preserved_quests);
  const memix::QuestInfo &preserved =
      quest_with_hash(*preserved_quests, first->ir_hash);
  REQUIRE(preserved.intent == "Updated guidance");
  REQUIRE(preserved.name == "repo rules");
  REQUIRE(preserved.created_at == created_at);

  const auto cleared =
      views.define_quest(*expression, memix::QuestDefinitionOptions{
                                          .intent = std::string{},
                                          .name = std::string{},
                                      });
  REQUIRE(cleared);
  const auto cleared_quests = views.list_quests();
  REQUIRE(cleared_quests);
  const memix::QuestInfo &cleared_info =
      quest_with_hash(*cleared_quests, first->ir_hash);
  REQUIRE(cleared_info.intent.empty());
  REQUIRE(cleared_info.name.empty());
  REQUIRE(cleared_info.created_at == created_at);
}

TEST_CASE("build auto-upserts quests and records every resolution", "[quest]") {
  TemporaryDirectory directory("build-history");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);

  const auto first = views.select(*expression);
  REQUIRE(first);
  REQUIRE(first->inserted);
  REQUIRE(history_line_count(store_root, first->ir_hash) == 1);
  const auto initial_quests = views.list_quests();
  REQUIRE(initial_quests);
  const memix::QuestInfo &initial =
      quest_with_hash(*initial_quests, first->ir_hash);
  REQUIRE(initial.intent.empty());
  REQUIRE(initial.name == "rules/repo/AGENTS.md@latest");
  const std::string created_at = initial.created_at;

  const auto second =
      views.select(*expression, memix::BuildOptions{
                                    .intent = "Repository build",
                                    .name = {},
                                });
  REQUIRE(second);
  REQUIRE(second->inserted);
  REQUIRE(second->id != first->id);
  REQUIRE(second->id != first->id);
  REQUIRE(second->ir_hash == first->ir_hash);
  REQUIRE(history_line_count(store_root, first->ir_hash) == 2);

  const auto third = views.select(*expression);
  REQUIRE(third);
  REQUIRE_FALSE(third->inserted);
  REQUIRE(third->id == second->id);
  REQUIRE(history_line_count(store_root, first->ir_hash) == 3);

  const auto quests = views.list_quests();
  REQUIRE(quests);
  const memix::QuestInfo &quest = quest_with_hash(*quests, first->ir_hash);
  REQUIRE(quest.resolution_count == 3);
  REQUIRE(quest.intent == "Repository build");
  REQUIRE(quest.name == "rules/repo/AGENTS.md@latest");
  REQUIRE(quest.created_at == created_at);
  REQUIRE_FALSE(quest.last_resolved_at.empty());
  REQUIRE(quest.latest_result);
  CHECK(quest.latest_result->kind == memix::ViewObjectKind::Derivation);
  CHECK(quest.latest_result->id == third->id);
  REQUIRE(quest.result_kind == memix::ViewObjectKind::Derivation);
}

TEST_CASE("quest resolution and freshness follow refs but preserve pins",
          "[quest]") {
  TemporaryDirectory directory("freshness");
  const fs::path old_source = directory.path() / "old.md";
  const fs::path new_source = directory.path() / "new.md";
  const fs::path newest_source = directory.path() / "newest.md";
  write_file(old_source, "old\n");
  write_file(new_source, "new\n");
  write_file(newest_source, "newest\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, old_source, "repo/AGENTS.md", "old");
  const memix::ViewService views(store);

  const auto latest_expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(latest_expression);
  const auto first = views.select(*latest_expression);
  REQUIRE(first);
  const auto initially_fresh = views.quest_freshness(first->ir_hash);
  REQUIRE(initially_fresh);
  REQUIRE_FALSE(initially_fresh->stale);
  REQUIRE(initially_fresh->inputs.size() == 1);
  REQUIRE(initially_fresh->inputs[0].fresh);
  REQUIRE_FALSE(initially_fresh->inputs[0].pinned);

  ingest_context_file(store, new_source, "repo/AGENTS.md", "new");
  const auto stale = views.quest_freshness(first->ir_hash);
  REQUIRE(stale);
  REQUIRE(stale->stale);
  REQUIRE_FALSE(stale->inputs[0].fresh);

  const auto resolved = views.resolve_quest(first->ir_hash);
  REQUIRE(resolved);
  REQUIRE(resolved->result.kind == memix::ViewObjectKind::Derivation);
  REQUIRE(resolved->result.id != first->id);
  REQUIRE(history_line_count(store_root, first->ir_hash) == 2);

  const memix::FsResult<bool> ref_removed =
      fs::remove(store_root / "refs" / "rules" / "latest");
  REQUIRE(ref_removed);
  REQUIRE(*ref_removed);
  const auto missing_ref = views.quest_freshness(first->ir_hash);
  REQUIRE(missing_ref);
  REQUIRE(missing_ref->stale);
  REQUIRE(missing_ref->inputs.size() == 1);
  REQUIRE_FALSE(missing_ref->inputs[0].pinned);
  REQUIRE_FALSE(missing_ref->inputs[0].fresh);
  REQUIRE_FALSE(missing_ref->inputs[0].current_generation.has_value());

  const auto pinned_expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@generation/000001"});
  REQUIRE(pinned_expression);
  const auto pinned = views.select(*pinned_expression);
  REQUIRE(pinned);
  ingest_context_file(store, newest_source, "repo/AGENTS.md", "newest");
  const auto pinned_freshness = views.quest_freshness(pinned->ir_hash);
  REQUIRE(pinned_freshness);
  REQUIRE_FALSE(pinned_freshness->stale);
  REQUIRE(pinned_freshness->inputs[0].pinned);
  REQUIRE_FALSE(pinned_freshness->inputs[0].current_generation.has_value());

  const auto unresolved_expression =
      views.expression_from_sources({"rules/repo/OTHER.md@latest"});
  REQUIRE(unresolved_expression);
  const auto unresolved =
      views.define_quest(*unresolved_expression, "not built", "");
  REQUIRE(unresolved);
  const auto no_resolution = views.quest_freshness(unresolved->ir_hash);
  REQUIRE_FALSE(no_resolution);
  REQUIRE(no_resolution.error().kind == memix::ErrorKind::NotFound);
}

TEST_CASE("freshness retains overlapping pinned and ref inputs", "[quest]") {
  TemporaryDirectory directory("freshness-overlap");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "first\n");
  write_file(second_source, "second\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, first_source, "repo/AGENTS.md", "first");
  const memix::ViewService views(store);
  const auto expression = views.expression_from_sources({
      "rules/repo/AGENTS.md@generation/000001",
      "rules/repo/AGENTS.md@latest",
  });
  REQUIRE(expression);

  const auto environment = views.select(*expression);
  REQUIRE(environment);
  REQUIRE(environment->resolved_files.size() == 1);
  REQUIRE(environment->input_resolutions.has_value());
  REQUIRE(environment->input_resolutions->size() == 2);
  const std::string manifest = read_file(environment->manifest_path);
  REQUIRE(manifest.contains(R"("input_resolutions":[)"));

  ingest_context_file(store, second_source, "repo/AGENTS.md", "second");
  const auto freshness = views.quest_freshness(environment->ir_hash);
  REQUIRE(freshness);
  REQUIRE(freshness->inputs.size() == 2);
  const auto pinned = std::ranges::find(freshness->inputs, true,
                                        &memix::QuestFreshnessInput::pinned);
  const auto followed = std::ranges::find(freshness->inputs, false,
                                          &memix::QuestFreshnessInput::pinned);
  REQUIRE(pinned != freshness->inputs.end());
  REQUIRE(followed != freshness->inputs.end());
  REQUIRE(pinned->fresh);
  REQUIRE_FALSE(followed->fresh);
  REQUIRE(freshness->stale);
}

TEST_CASE("freshness records optional inputs that resolve no files",
          "[quest]") {
  TemporaryDirectory directory("freshness-optional");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "first\n");
  write_file(second_source, "second\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, first_source, "repo/present.md", "first");
  const memix::ViewService views(store);
  const memix::ViewIr ir{
      .schema = "memix.view.select.v1",
      .inputs =
          {
              memix::ViewIrInput{
                  .locator = "rules/repo/missing.md@latest",
                  .type = memix::MemoryObjectType::Rules,
                  .path = "repo/missing.md",
                  .version =
                      memix::ViewVersionSelector{
                          .kind = memix::ViewVersionKind::Ref,
                          .value = "latest",
                      },
                  .recursive = false,
                  .required = false,
              },
          },
      .target = {},
      .bindings = {},
  };

  const auto derivation = views.build_ir(ir);
  REQUIRE(derivation);
  REQUIRE(derivation->resolved_files.empty());
  REQUIRE(derivation->input_resolutions.has_value());
  REQUIRE(derivation->input_resolutions->size() == 1);
  REQUIRE(derivation->input_resolutions->front().locator
          == "rules/repo/missing.md@latest");
  const std::string manifest = read_file(derivation->manifest_path);
  REQUIRE(manifest.contains(R"("resolved_inputs":[])"));
  REQUIRE(manifest.contains(
      R"("input_resolutions":[{"locator":"rules/repo/missing.md@latest")"));

  const auto environment = views.resolve_quest(derivation->ir_hash);
  REQUIRE(environment);
  REQUIRE(environment->result.id == derivation->id);
  const auto initially_fresh = views.quest_freshness(derivation->ir_hash);
  REQUIRE(initially_fresh);
  REQUIRE_FALSE(initially_fresh->stale);
  REQUIRE(initially_fresh->inputs.size() == 1);
  REQUIRE(initially_fresh->inputs.front().fresh);

  ingest_context_file(store, second_source, "repo/other.md", "second");
  const auto stale = views.quest_freshness(derivation->ir_hash);
  REQUIRE(stale);
  REQUIRE(stale->stale);
  REQUIRE_FALSE(stale->inputs.front().fresh);
}

TEST_CASE("freshness rejects manifests missing input resolutions",
          "[quest][d2]") {
  QuestFixture f;
  const auto d = f.select();
  auto text = read_file(d.manifest_path);
  erase_json_array_field(text, "input_resolutions");
  REQUIRE(fs::permissions(d.manifest_path, fs::perms::owner_all));
  write_file(d.manifest_path, text);
  CHECK_FALSE(f.views.resolve_derivation(d.id));
  CHECK_FALSE(f.views.quest_freshness(d.ir_hash));
  CHECK(read_file(d.manifest_path) == text);
}

TEST_CASE("quest finding combines semantic and structural lookup", "[quest]") {
  TemporaryDirectory directory("find");
  const memix::ViewService views{memix::Store(directory.path() / "store")};
  const auto invalid = views.find_quests({}, std::nullopt);
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().kind == memix::ErrorKind::InvalidArgument);

  const auto file_expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  const auto directory_expression =
      views.expression_from_sources({"chat/task/session/@latest"});
  const auto unicode_expression =
      views.expression_from_sources({"documents/notes/memory.md@latest"});
  const auto rules_shared_expression =
      views.expression_from_sources({"rules/shared/item.md@latest"});
  const auto documents_shared_expression =
      views.expression_from_sources({"documents/shared/item.md@latest"});
  REQUIRE(file_expression);
  REQUIRE(directory_expression);
  REQUIRE(unicode_expression);
  REQUIRE(rules_shared_expression);
  REQUIRE(documents_shared_expression);
  const auto file =
      views.define_quest(*file_expression, "Repository Release Rules", "");
  const auto recursive =
      views.define_quest(*directory_expression, "Task conversation", "");
  const auto unicode = views.define_quest(*unicode_expression,
                                          "记忆 WorkFlow context", "unicode");
  const auto rules_shared =
      views.define_quest(*rules_shared_expression, "rules shared", "");
  const auto documents_shared =
      views.define_quest(*documents_shared_expression, "documents shared", "");
  REQUIRE(file);
  REQUIRE(recursive);
  REQUIRE(unicode);
  REQUIRE(rules_shared);
  REQUIRE(documents_shared);

  const auto semantic = views.find_quests({"release", "RULES"}, std::nullopt);
  REQUIRE(semantic);
  REQUIRE(semantic->size() == 1);
  REQUIRE(semantic->front().ir_hash == file->ir_hash);

  const auto unicode_match =
      views.find_quests({"记忆", "workflow"}, std::nullopt);
  REQUIRE(unicode_match);
  REQUIRE(unicode_match->size() == 1);
  REQUIRE(unicode_match->front().ir_hash == unicode->ir_hash);

  const auto exact = views.find_quests({}, fs::path("rules/repo/AGENTS.md"));
  REQUIRE(exact);
  REQUIRE(exact->size() == 1);
  REQUIRE(exact->front().ir_hash == file->ir_hash);

  const auto below =
      views.find_quests({}, fs::path("chat/task/session/messages/a.md"));
  REQUIRE(below);
  REQUIRE(below->size() == 1);
  REQUIRE(below->front().ir_hash == recursive->ir_hash);

  const auto above = views.find_quests({}, fs::path("rules/repo"));
  REQUIRE(above);
  REQUIRE(above->size() == 1);
  REQUIRE(above->front().ir_hash == file->ir_hash);

  const auto excluded =
      views.find_quests({"conversation"}, fs::path("rules/repo"));
  REQUIRE(excluded);
  REQUIRE(excluded->empty());

  const auto typed_rules =
      views.find_quests({}, fs::path("rules/shared/item.md"));
  REQUIRE(typed_rules);
  REQUIRE(typed_rules->size() == 1);
  REQUIRE(typed_rules->front().ir_hash == rules_shared->ir_hash);

  const auto typed_documents =
      views.find_quests({}, fs::path("documents/shared/item.md"));
  REQUIRE(typed_documents);
  REQUIRE(typed_documents->size() == 1);
  REQUIRE(typed_documents->front().ir_hash == documents_shared->ir_hash);

  const auto untyped = views.find_quests({}, fs::path("shared/item.md"));
  REQUIRE(untyped);
  REQUIRE(untyped->size() == 2);
}

TEST_CASE("quest finding enriches only metadata matches", "[quest]") {
  constexpr std::size_t excluded_receipt_count = 10000;
  TemporaryDirectory directory("find-two-pass");
  const fs::path source = directory.path() / "matching.md";
  write_file(source, "matching\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/matching.md", "matching");
  const memix::ViewService views(store);

  const auto matching_expression =
      views.expression_from_sources({"rules/repo/matching.md@latest"});
  const auto excluded_expression =
      views.expression_from_sources({"rules/repo/excluded.md@latest"});
  REQUIRE(matching_expression);
  REQUIRE(excluded_expression);
  const auto matching =
      views.select(*matching_expression, memix::BuildOptions{
                                             .intent = "needle metadata",
                                             .name = "matching",
                                         });
  const auto excluded =
      views.define_quest(*excluded_expression, "unrelated metadata", "");
  REQUIRE(matching);
  REQUIRE(excluded);
  const auto read =
      read_derivation_for_test(views, matching->id, "rules/repo/matching.md");
  REQUIRE(read);

  const std::string fake_id(64, 'a');
  const std::string receipt =
      std::format(R"({{"at":"2026-07-29T00:00:00Z","operation":"cat",)"
                  R"("environment_id":"{}","derivation_id":"{}",)"
                  R"("path":"rules/repo/excluded.md"}})"
                  "\n",
                  fake_id, fake_id);
  std::string large_receipts;
  large_receipts.reserve(receipt.size() * excluded_receipt_count);
  for (std::size_t index = 0; index < excluded_receipt_count; ++index) {
    large_receipts += receipt;
  }
  write_file(receipts_path(store_root, excluded->ir_hash), large_receipts);

  const auto matches = views.find_quests({"needle"}, std::nullopt);
  REQUIRE(matches);
  REQUIRE(matches->size() == 1);
  REQUIRE(matches->front().ir_hash == matching->ir_hash);
  REQUIRE(matches->front().resolution_count == 1);
  REQUIRE(matches->front().read_count == 1);
}

TEST_CASE(
    "derivation creation time is metadata but old schemas are unsupported",
    "[quest][d2]") {
  QuestFixture f;
  const auto d = f.select();
  const auto original = read_file(d.manifest_path);
  const auto created = json_string_field(original, "created_at");
  REQUIRE_FALSE(created.empty());
  REQUIRE(f.views.resolve_derivation(d.id));
  auto changed = original;
  const auto at = changed.find(created);
  REQUIRE(at != std::string::npos);
  changed.replace(at, created.size(), "2000-01-01T00:00:00Z");
  REQUIRE(fs::permissions(d.manifest_path, fs::perms::owner_all));
  write_file(d.manifest_path, changed);
  REQUIRE(f.views.resolve_derivation(d.id));
  changed.replace(changed.find("memix.view.derivation.v4"),
                  std::string_view("memix.view.derivation.v4").size(),
                  "memix.view.derivation.v3");
  write_file(d.manifest_path, changed);
  CHECK_FALSE(f.views.resolve_derivation(d.id));
  CHECK_FALSE(f.views.select(f.expression));
  CHECK(read_file(d.manifest_path) == changed);
}

TEST_CASE("quest prefixes reject invalid and ambiguous values", "[quest]") {
  TemporaryDirectory directory("prefixes");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto build_expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(build_expression);
  const auto built = views.select(*build_expression);
  REQUIRE(built);

  const auto resolved_full = views.resolve_quest(built->ir_hash);
  REQUIRE(resolved_full);
  REQUIRE(resolved_full->result.id == built->id);
  REQUIRE(history_line_count(store_root, built->ir_hash) == 2);

  const auto empty = views.resolve_quest("");
  REQUIRE_FALSE(empty);
  REQUIRE(empty.error().kind == memix::ErrorKind::InvalidArgument);
  const auto non_hex = views.resolve_quest("not-hex");
  REQUIRE_FALSE(non_hex);
  REQUIRE(non_hex.error().kind == memix::ErrorKind::InvalidArgument);

  std::map<char, std::vector<std::string>> ids_by_first_digit;
  ids_by_first_digit[built->ir_hash.front()].push_back(built->ir_hash);
  constexpr int quest_count = 17;
  for (int index = 0; index < quest_count; ++index) {
    const auto expression = views.expression_from_sources(
        {std::format("rules/repo/file-{}.md@latest", index)});
    REQUIRE(expression);
    const auto definition = views.define_quest(*expression, "", "");
    REQUIRE(definition);
    ids_by_first_digit[definition->ir_hash.front()].push_back(
        definition->ir_hash);
  }
  const auto ambiguous_group =
      std::ranges::find_if(ids_by_first_digit, [](const auto &entry) {
        return entry.second.size() > 1;
      });
  REQUIRE(ambiguous_group != ids_by_first_digit.end());
  const std::string ambiguous_prefix(1, ambiguous_group->first);
  const auto ambiguous = views.resolve_quest(ambiguous_prefix);
  REQUIRE_FALSE(ambiguous);
  REQUIRE(ambiguous.error().kind == memix::ErrorKind::Conflict);
}

TEST_CASE("quest listing tolerates absent history but rejects bad metadata",
          "[quest]") {
  TemporaryDirectory directory("listing-errors");
  const fs::path store_root = directory.path() / "store";
  const memix::ViewService views{memix::Store(store_root)};

  const auto initially_empty = views.list_quests();
  REQUIRE(initially_empty);
  REQUIRE(initially_empty->empty());

  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto definition = views.define_quest(*expression, "metadata", "");
  REQUIRE(definition);
  const auto without_history = views.list_quests();
  REQUIRE(without_history);
  REQUIRE(without_history->size() == 1);
  REQUIRE(without_history->front().resolution_count == 0);

  write_file(definition->quest_dir / "history.jsonl", "not json\n");
  const auto corrupt_history = views.list_quests();
  REQUIRE(corrupt_history);
  REQUIRE(corrupt_history->size() == 1);
  REQUIRE(corrupt_history->front().resolution_count == 0);
  const auto corrupt_history_stats = views.quest_stats(definition->ir_hash);
  REQUIRE_FALSE(corrupt_history_stats);
  REQUIRE(corrupt_history_stats.error().kind == memix::ErrorKind::InvalidData);
  REQUIRE(read_file(definition->quest_dir / "history.jsonl") == "not json\n");

  write_file(definition->quest_dir / "quest.json", "not json");
  const auto corrupt_quest = views.list_quests();
  REQUIRE_FALSE(corrupt_quest);
  REQUIRE(corrupt_quest.error().kind == memix::ErrorKind::InvalidData);
}

TEST_CASE("quest resolution rejects tampered persisted IR", "[quest]") {
  TemporaryDirectory directory("tampered-ir");
  const fs::path store_root = directory.path() / "store";
  const memix::ViewService views{memix::Store(store_root)};
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto definition = views.define_quest(*expression, "metadata", "");
  REQUIRE(definition);

  const fs::path ir_path = definition->quest_dir / "ir.json";
  const memix::FsVoid made_writable =
      fs::permissions(ir_path, fs::perms::owner_all);
  REQUIRE(made_writable);
  write_file(ir_path, R"({"schema":"memix.view.select.v1"})");
  const auto resolved = views.resolve_quest(definition->ir_hash);
  REQUIRE_FALSE(resolved);
  REQUIRE(resolved.error().kind == memix::ErrorKind::InvalidData);
}

TEST_CASE(
    "composition quests have typed pinned results and independent history",
    "[quest][d2]") {
  QuestFixture f;
  const auto d = f.select("selection");
  const std::vector<memix::ViewEnvironmentBinding> bindings{
      {.mount = "a", .derivation_id = d.id, .output_role = "file_tree"},
      {.mount = "b", .derivation_id = d.id, .output_role = "file_tree"}};
  const auto definition = f.views.define_composition(
      bindings, {.intent = "assembly", .name = "pinned"});
  REQUIRE(definition);
  auto listing = f.views.list_quests();
  REQUIRE(listing);
  const auto &unresolved = quest_with_hash(*listing, definition->ir_hash);
  CHECK(unresolved.result_kind == memix::ViewObjectKind::Environment);
  CHECK_FALSE(unresolved.latest_result);
  const auto result = f.views.resolve_quest(definition->ir_hash);
  REQUIRE(result);
  CHECK(result->result.kind == memix::ViewObjectKind::Environment);
  REQUIRE(f.views.resolve_environment(result->result.id));
  CHECK(f.views.list_derivations()->size() == 1);
  CHECK(history_line_count(f.root, d.ir_hash) == 1);
  CHECK(history_line_count(f.root, definition->ir_hash) == 1);
  f.update("changed\n");
  const auto freshness = f.views.quest_freshness(definition->ir_hash);
  REQUIRE(freshness);
  CHECK_FALSE(freshness->stale);
  CHECK(freshness->inputs.empty());
  const auto repeated = f.views.resolve_quest(definition->ir_hash);
  REQUIRE(repeated);
  CHECK(repeated->result == result->result);
  CHECK_FALSE(repeated->inserted);
  const auto lines = jsonl_lines(definition->quest_dir / "history.jsonl");
  REQUIRE(lines.size() == 2);
  for (const auto &line : lines) {
    CHECK(json_string_field(line, "schema") == "memix.quest.resolution.v2");
    CHECK(json_string_field(line, "kind") == "environment");
    CHECK(json_string_field(line, "id") == result->result.id);
    CHECK_FALSE(line.contains("derivation_id"));
  }
}

TEST_CASE("cat ls and tree append typed derivation receipts only after success",
          "[quest][d2]") {
  QuestFixture f;
  const auto d = f.select();
  const auto inspected =
      f.views.read_output(reference(d), "rules/repo/AGENTS.md", false);
  REQUIRE(inspected);
  CHECK(f.views.quest_stats(d.ir_hash)->read_count == 0);
  REQUIRE(f.views.read_output(reference(d), "rules/repo/AGENTS.md"));
  REQUIRE(f.views.derivation_tree(d.id, {.path = "rules", .depth = 1}));
  REQUIRE(f.views.view_ls(reference(d), "rules"));
  CHECK_FALSE(f.views.read_output(reference(d), "../escape"));
  CHECK_FALSE(f.views.read_output(reference(d), "rules/missing"));
  CHECK_FALSE(f.views.view_ls(reference(d), "rules/repo/AGENTS.md"));
  CHECK_FALSE(f.views.derivation_tree(d.id, {.path = "missing", .depth = {}}));
  const auto lines = jsonl_lines(receipts_path(f.root, d.ir_hash));
  REQUIRE(lines.size() == 3);
  for (const auto &line : lines) {
    CHECK(json_string_field(line, "schema") == "memix.quest.receipt.v2");
    CHECK(json_string_field(line, "kind") == "derivation");
    CHECK(json_string_field(line, "id") == d.id);
    CHECK_FALSE(line.contains("environment_id"));
  }
  const auto stats = f.views.quest_stats(d.ir_hash);
  REQUIRE(stats);
  CHECK(stats->read_count == 3);
  CHECK(stats->cat_count == 1);
  CHECK(stats->tree_count == 1);
  CHECK(stats->ls_count == 1);
  REQUIRE(stats->reads_by_derivation.size() == 1);
  CHECK(stats->reads_by_derivation.front().derivation_id == d.id);
  CHECK(stats->reads_by_environment.empty());
  CHECK(f.views.list_environments()->empty());
}

TEST_CASE("multi-binding environment consumption belongs only to its "
          "composition quest",
          "[quest][d2]") {
  QuestFixture f;
  const auto d = f.select("first");
  const auto second = f.select("second");
  const auto e = f.views.compose(
      {{.mount = "a", .derivation_id = d.id, .output_role = "file_tree"},
       {.mount = "b", .derivation_id = second.id, .output_role = "file_tree"}});
  REQUIRE(e);
  REQUIRE(f.views.read_output(reference(*e), "b/rules/repo/AGENTS.md"));
  REQUIRE(f.views.environment_tree(e->id));
  REQUIRE(f.views.view_ls(reference(*e)));
  REQUIRE(f.views.activate(
      e->id, {.profile = "owner", .into = f.directory.path() / "memory"}));
  const auto stats = f.views.quest_stats(e->ir_hash);
  REQUIRE(stats);
  CHECK(stats->read_count == 3);
  CHECK(stats->activation_count == 1);
  CHECK(stats->reads_by_derivation.empty());
  REQUIRE(stats->reads_by_environment.size() == 1);
  CHECK(stats->reads_by_environment.front().environment_id == e->id);
  const auto selection_stats = f.views.quest_stats(d.ir_hash);
  REQUIRE(selection_stats);
  CHECK(selection_stats->read_count == 0);
  CHECK(selection_stats->activation_count == 0);
  CHECK_FALSE(f.views.activate(
      d.id, {.profile = "invalid", .into = f.directory.path() / "invalid"}));
}

TEST_CASE("activation receipts record intent events and skip deactivation",
          "[quest]") {
  QuestFixture f;
  const auto d = f.select();
  const auto e = f.compose(d);
  const auto into = f.directory.path() / "memory";
  const memix::ActivationOptions options{.profile = "profile", .into = into};
  REQUIRE(f.views.activate(e.id, options));
  REQUIRE(f.views.activate(e.id, options));
  CHECK(f.views.quest_stats(e.ir_hash)->activation_count == 2);
  REQUIRE(f.views.deactivate(into));
  CHECK(f.views.quest_stats(e.ir_hash)->activation_count == 2);
  const auto lines = jsonl_lines(receipts_path(f.root, e.ir_hash));
  REQUIRE(lines.size() == 2);
  for (const auto &line : lines) {
    CHECK(json_string_field(line, "kind") == "environment");
    CHECK(json_string_field(line, "id") == e.id);
    CHECK(json_string_field(line, "operation") == "activate");
    CHECK(json_string_field(line, "profile") == "profile");
    CHECK(json_string_field(line, "via") == "activate");
    CHECK(json_string_field(line, "mode") == "link");
  }
}

TEST_CASE("rollback receipts identify the recommitted environment", "[quest]") {
  QuestFixture f;
  const auto first = f.compose(f.select());
  f.update("two\n");
  const auto second = f.compose(f.select());
  const auto into = f.directory.path() / "memory";
  const memix::ActivationOptions options{.profile = "profile", .into = into};
  REQUIRE(f.views.activate(first.id, options));
  REQUIRE(f.views.activate(second.id, options));
  const auto rollback = f.views.rollback("profile");
  REQUIRE(rollback);
  CHECK(rollback->environment_id == first.id);
  const auto lines = jsonl_lines(receipts_path(f.root, first.ir_hash));
  REQUIRE(lines.size() == 2);
  CHECK(json_string_field(lines.back(), "via") == "rollback");
  CHECK(json_string_field(lines.back(), "id") == first.id);
  CHECK(f.views.quest_stats(first.ir_hash)->activation_count == 2);
  CHECK(f.views.quest_stats(second.ir_hash)->activation_count == 1);
}

TEST_CASE("activation receipt failures and missing catalogs remain best effort",
          "[quest]") {
  QuestFixture f;
  const auto d = f.select();
  const auto e = f.compose(d);
  const auto into = f.directory.path() / "memory";
  const auto receipt = receipts_path(f.root, e.ir_hash);
  REQUIRE(fs::create_directories(receipt));
  REQUIRE(f.views.activate(e.id, {.profile = "profile", .into = into}));
  REQUIRE_FALSE(f.views.quest_stats(e.ir_hash));
  REQUIRE(fs::remove_all(f.root / "views" / "quests" / e.ir_hash));
  REQUIRE(fs::remove_all(f.root / "views" / "quests" / d.ir_hash));
  REQUIRE(f.views.activate(e.id, {.profile = "profile", .into = into}));
  REQUIRE(f.views.rollback("profile", 1));
  CHECK(read_file(into / "rules/repo/AGENTS.md") == "one\n");
}

TEST_CASE("direct reads survive missing catalogs and receipt append failures",
          "[quest]") {
  QuestFixture f;
  const auto d = f.select();
  REQUIRE(fs::remove_all(f.root / "views" / "quests" / d.ir_hash));
  REQUIRE(f.views.read_output(reference(d), "rules/repo/AGENTS.md"));
  REQUIRE(f.views.list_quests()->empty());
  REQUIRE(f.views.select(f.expression));
  REQUIRE(fs::create_directories(receipts_path(f.root, d.ir_hash)));
  const auto contents =
      f.views.read_output(reference(d), "rules/repo/AGENTS.md");
  REQUIRE(contents);
  CHECK(contents->contents == "one\n");
  CHECK_FALSE(f.views.quest_stats(d.ir_hash));
  CHECK_FALSE(
      fs::exists(f.root / "views" / "quests" / d.ir_hash / "receipts.v2.jsonl")
          .value_or(true));
}

TEST_CASE("statistics reject malformed receipt logs without repairing them",
          "[quest]") {
  QuestFixture f;
  const auto d = f.select();
  REQUIRE(f.views.read_output(reference(d), "rules/repo/AGENTS.md"));
  const auto path = receipts_path(f.root, d.ir_hash);
  append_file(path, "{truncated");
  const auto damaged = read_file(path);
  CHECK_FALSE(f.views.quest_stats(d.ir_hash));
  REQUIRE(f.views.read_output(reference(d), "rules/repo/AGENTS.md"));
  CHECK(read_file(path) == damaged);
}

TEST_CASE("quest stats aggregate reads across derivation resolutions",
          "[quest]") {
  QuestFixture f;
  const auto a = f.select();
  REQUIRE(f.views.read_output(reference(a), "rules/repo/AGENTS.md"));
  REQUIRE(f.views.derivation_tree(a.id));
  f.update("two\n");
  const auto next = f.views.resolve_quest(a.ir_hash);
  REQUIRE(next);
  REQUIRE(next->result.kind == memix::ViewObjectKind::Derivation);
  REQUIRE(next->result.id != a.id);
  REQUIRE(f.views.read_output(next->result, "rules/repo/AGENTS.md"));
  REQUIRE(f.views.read_output(reference(a), "rules/repo/AGENTS.md"));
  const auto repeated = f.views.resolve_quest(a.ir_hash);
  REQUIRE(repeated);
  CHECK(repeated->result == next->result);
  REQUIRE(f.views.view_ls(repeated->result));
  const auto stats = f.views.quest_stats(a.ir_hash);
  REQUIRE(stats);
  CHECK(stats->resolution_count == 3);
  CHECK(stats->read_count == 5);
  CHECK(stats->cat_count == 3);
  CHECK(stats->tree_count == 1);
  CHECK(stats->ls_count == 1);
  CHECK(stats->reads_by_derivation.size() == 2);
  CHECK(stats->reads_by_environment.empty());
  CHECK_FALSE(stats->first_read_at.empty());
  CHECK_FALSE(stats->last_read_at.empty());
  CHECK(stats->activation_count == 0);
}

TEST_CASE("quest stats aggregate activations by profile", "[quest]") {
  QuestFixture f;
  const auto e = f.compose(f.select());
  for (const auto *profile : {"first", "second", "first"}) {
    REQUIRE(f.views.activate(
        e.id, {.profile = profile, .into = f.directory.path() / profile}));
  }
  const auto stats = f.views.quest_stats(e.ir_hash);
  REQUIRE(stats);
  CHECK(stats->activation_count == 3);
  REQUIRE(stats->activations_by_profile.size() == 2);
  CHECK(stats->activations_by_profile[0].profile == "first");
  CHECK(stats->activations_by_profile[0].activation_count == 2);
  CHECK(stats->activations_by_profile[1].activation_count == 1);
  CHECK_FALSE(stats->first_activated_at.empty());
  CHECK_FALSE(stats->last_activated_at.empty());
  CHECK(stats->read_count == 0);
}

TEST_CASE(
    "receipt scans reject obsolete formats and inconsistent typed payloads",
    "[quest][d2]") {
  QuestFixture f;
  const auto d = f.select();
  const memix::ViewReference ref{.kind = memix::ViewObjectKind::Derivation,
                                 .id = d.id};
  REQUIRE(f.views.read_output(ref, "rules/repo/AGENTS.md"));
  REQUIRE(f.views.quest_stats(d.ir_hash)->read_count == 1);
  const auto path = receipts_path(f.root, d.ir_hash);
  const std::string prefix = std::format(
      R"({{"schema":"memix.quest.receipt.v2","at":"2000-01-01T00:00:00Z","result":{{"kind":"derivation","id":"{}"}})",
      d.id);
  const std::vector<std::string> invalid{
      R"({"at":"2000-01-01T00:00:00Z","operation":"cat"})"
      "\n",
      prefix
          + R"(,"operation":"future","path":"rules/repo/AGENTS.md"})"
            "\n",
      prefix
          + R"(,"operation":"cat"})"
            "\n",
      prefix
          + R"(,"operation":"cat","path":42})"
            "\n",
      prefix
          + R"(,"operation":"cat","path":"rules/repo/AGENTS.md","profile":"wrong"})"
            "\n",
      prefix
          + R"(,"operation":"activate","profile":"p","generation":1,"mode":"link","via":"activate"})"
            "\n",
      std::format(
          R"({{"schema":"memix.quest.receipt.v2","at":"2000-01-01T00:00:00Z","operation":"cat","result":{{"kind":"environment","id":"{}"}},"path":"rules/repo/AGENTS.md"}})"
          "\n",
          d.id),
  };
  for (const auto &text : invalid) {
    INFO(text);
    write_file(path, text);
    const auto stats = f.views.quest_stats(d.ir_hash);
    REQUIRE_FALSE(stats);
    CHECK(stats.error().kind == memix::ErrorKind::InvalidData);
    CHECK(read_file(path) == text);
    // Verified reads still succeed, but must not append through corrupt logs.
    REQUIRE(f.views.read_output(ref, "rules/repo/AGENTS.md"));
    CHECK(read_file(path) == text);
  }
}

TEST_CASE("file locks serialize local threads and remain reusable",
          "[quest][lock]") {
  constexpr int thread_count = 16;
  constexpr int acquisitions_per_thread = 100;
  TemporaryDirectory directory("file-lock-stress");
  const fs::path lock_path = directory.path() / ".quest-store.lock";

  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  std::atomic<int> inside = 0;
  std::atomic<int> acquisitions = 0;
  std::atomic<bool> overlapped = false;
  std::mutex error_mutex;
  std::string first_error;
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
    threads.emplace_back([&]() {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int acquisition = 0; acquisition < acquisitions_per_thread;
           ++acquisition) {
        memix::FsResult<memix::detail::FileLockGuard> lock =
            memix::detail::FileLockGuard::acquire(
                lock_path, memix::detail::FileLockGuard::Clock::now()
                               + std::chrono::seconds(10));
        if (!lock) {
          const std::lock_guard error_lock(error_mutex);
          if (first_error.empty()) {
            first_error = lock.error().message;
          }
          return;
        }
        if (inside.fetch_add(1, std::memory_order_acq_rel) != 0) {
          overlapped.store(true, std::memory_order_release);
        }
        std::this_thread::yield();
        inside.fetch_sub(1, std::memory_order_acq_rel);
        acquisitions.fetch_add(1, std::memory_order_acq_rel);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != thread_count) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread &thread : threads) {
    thread.join();
  }

  INFO(first_error);
  REQUIRE(first_error.empty());
  REQUIRE_FALSE(overlapped.load(std::memory_order_acquire));
  REQUIRE(inside.load(std::memory_order_acquire) == 0);
  REQUIRE(acquisitions.load(std::memory_order_acquire)
          == thread_count * acquisitions_per_thread);
  const memix::FsResult<bool> lock_is_file = fs::is_regular_file(lock_path);
  REQUIRE(lock_is_file);
  REQUIRE(*lock_is_file);
  const auto reused = memix::detail::FileLockGuard::acquire(
      lock_path,
      memix::detail::FileLockGuard::Clock::now() + std::chrono::seconds(1));
  REQUIRE(reused);
}

TEST_CASE("file lock contention times out and releases with its guard",
          "[quest][lock]") {
  TemporaryDirectory directory("file-lock-timeout");
  const fs::path lock_path = directory.path() / ".quest-store.lock";

  {
    const auto held = memix::detail::FileLockGuard::acquire(
        lock_path,
        memix::detail::FileLockGuard::Clock::now() + std::chrono::seconds(1));
    REQUIRE(held);
    bool unexpectedly_acquired = false;
    std::optional<memix::FsError> contention_error;
    std::thread contender([&]() {
      auto contended = memix::detail::FileLockGuard::acquire(
          lock_path, memix::detail::FileLockGuard::Clock::now()
                         + std::chrono::milliseconds(50));
      unexpectedly_acquired = contended.has_value();
      if (!contended) {
        contention_error = contended.error();
      }
    });
    contender.join();
    REQUIRE_FALSE(unexpectedly_acquired);
    REQUIRE(contention_error.has_value());
    REQUIRE(contention_error->code == std::errc::timed_out);
  }

  const auto reacquired = memix::detail::FileLockGuard::acquire(
      lock_path,
      memix::detail::FileLockGuard::Clock::now() + std::chrono::seconds(1));
  REQUIRE(reacquired);
}

TEST_CASE("concurrent quest definitions atomically publish complete metadata",
          "[quest]") {
  constexpr int writer_count = 16;
  TemporaryDirectory directory("concurrent-metadata");
  const fs::path store_root = directory.path() / "store";
  const memix::ViewService views{memix::Store(store_root)};
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto initial = views.define_quest(*expression, "initial", "initial");
  REQUIRE(initial);

  std::atomic<int> ready = 0;
  std::atomic<int> completed = 0;
  std::atomic<bool> start = false;
  std::atomic<bool> parse_failed = false;
  std::array<memix::ViewResult<memix::QuestDefinition>, writer_count> results{};
  std::vector<std::thread> writers;
  writers.reserve(writer_count);
  for (int index = 0; index < writer_count; ++index) {
    writers.emplace_back([&, index]() {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const memix::ViewService thread_views{memix::Store(store_root)};
      results.at(static_cast<std::size_t>(index)) = thread_views.define_quest(
          *expression, std::format("intent-{}", index),
          std::format("name-{}", index));
      completed.fetch_add(1, std::memory_order_acq_rel);
    });
  }
  std::thread reader([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    const memix::ViewService thread_views{memix::Store(store_root)};
    while (completed.load(std::memory_order_acquire) != writer_count) {
      const auto listed = thread_views.list_quests();
      if (!listed) {
        parse_failed.store(true, std::memory_order_release);
        return;
      }
    }
  });
  while (ready.load(std::memory_order_acquire) != writer_count) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread &writer : writers) {
    writer.join();
  }
  reader.join();

  REQUIRE_FALSE(parse_failed.load(std::memory_order_acquire));
  for (const auto &result : results) {
    const std::string error = result ? "" : result.error().message;
    INFO(error);
    REQUIRE(result);
  }
  const std::string quest_json = read_file(initial->quest_dir / "quest.json");
  const std::string final_intent = json_string_field(quest_json, "intent");
  const std::string final_name = json_string_field(quest_json, "name");
  bool matches_writer = false;
  for (int index = 0; index < writer_count; ++index) {
    if (final_intent == std::format("intent-{}", index)
        && final_name == std::format("name-{}", index)) {
      matches_writer = true;
      break;
    }
  }
  REQUIRE(matches_writer);
}

TEST_CASE("concurrent define and annotated build preserve quest metadata and "
          "history",
          "[quest]") {
  TemporaryDirectory directory("concurrent-define-build");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto initial = views.define_quest(*expression, "initial", "initial");
  REQUIRE(initial);

  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  memix::ViewResult<memix::QuestDefinition> defined;
  memix::ViewResult<memix::ViewDerivation> built;
  std::thread definer([&]() {
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    const memix::ViewService thread_views{memix::Store(store_root)};
    defined =
        thread_views.define_quest(*expression, "define-intent", "define-name");
  });
  std::thread builder([&]() {
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    const memix::ViewService thread_views{memix::Store(store_root)};
    built = thread_views.select(*expression, memix::BuildOptions{
                                                 .intent = "build-intent",
                                                 .name = "build-name",
                                             });
  });
  while (ready.load(std::memory_order_acquire) != 2) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  definer.join();
  builder.join();

  const std::string define_error =
      defined ? std::string{} : defined.error().message;
  const std::string build_error = built ? std::string{} : built.error().message;
  INFO(define_error);
  INFO(build_error);
  REQUIRE(defined);
  REQUIRE(built);
  REQUIRE(history_line_count(store_root, initial->ir_hash) == 1);
  const std::string quest_json = read_file(initial->quest_dir / "quest.json");
  const std::string final_intent = json_string_field(quest_json, "intent");
  const std::string final_name = json_string_field(quest_json, "name");
  const bool define_won =
      final_intent == "define-intent" && final_name == "define-name";
  const bool build_won =
      final_intent == "build-intent" && final_name == "build-name";
  REQUIRE((define_won || build_won));
  const auto listed = views.list_quests();
  REQUIRE(listed);
  REQUIRE(listed->size() == 1);
  REQUIRE(listed->front().resolution_count == 1);
}

TEST_CASE("concurrent quest reads append complete receipt lines",
          "[quest][receipts]") {
  constexpr int reader_count = 16;
  TemporaryDirectory directory("concurrent-receipts");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto environment = views.select(*expression);
  REQUIRE(environment);

  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  std::array<memix::ViewResult<memix::ResolvedViewFile>, reader_count>
      results{};
  std::vector<std::thread> readers;
  readers.reserve(reader_count);
  for (int index = 0; index < reader_count; ++index) {
    readers.emplace_back([&, index]() {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const memix::ViewService thread_views{memix::Store(store_root)};
      results.at(static_cast<std::size_t>(index)) = read_derivation_for_test(
          thread_views, environment->id, "rules/repo/AGENTS.md");
    });
  }
  while (ready.load(std::memory_order_acquire) != reader_count) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread &reader : readers) {
    reader.join();
  }
  for (const auto &result : results) {
    const std::string error = result ? "" : result.error().message;
    INFO(error);
    REQUIRE(result);
  }

  const std::vector<std::string> receipts =
      jsonl_lines(receipts_path(store_root, environment->ir_hash));
  REQUIRE(receipts.size() == static_cast<std::size_t>(reader_count));
  for (const std::string &receipt : receipts) {
    REQUIRE(json_string_field(receipt, "schema") == "memix.quest.receipt.v2");
    REQUIRE(json_string_field(receipt, "operation") == "cat");
    REQUIRE(json_string_field(receipt, "kind") == "derivation");
    REQUIRE(json_string_field(receipt, "id") == environment->id);
    REQUIRE(json_string_field(receipt, "path") == "rules/repo/AGENTS.md");
  }
}

TEST_CASE("concurrent quest builds append complete history lines", "[quest]") {
  constexpr int builder_count = 16;
  TemporaryDirectory directory("concurrent-history");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);

  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  std::array<memix::ViewResult<memix::ViewDerivation>, builder_count> results{};
  std::vector<std::thread> builders;
  builders.reserve(builder_count);
  for (int index = 0; index < builder_count; ++index) {
    builders.emplace_back([&, index]() {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const memix::ViewService thread_views{memix::Store(store_root)};
      results.at(static_cast<std::size_t>(index)) =
          thread_views.select(*expression);
    });
  }
  while (ready.load(std::memory_order_acquire) != builder_count) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread &builder : builders) {
    builder.join();
  }

  std::string ir_hash;
  std::string derivation_id;
  std::string environment_id;
  for (const auto &result : results) {
    const std::string error = result ? "" : result.error().message;
    INFO(error);
    REQUIRE(result);
    if (ir_hash.empty()) {
      ir_hash = result->ir_hash;
      derivation_id = result->id;
      environment_id = result->id;
    }
    REQUIRE(result->ir_hash == ir_hash);
    REQUIRE(result->id == derivation_id);
    REQUIRE(result->id == environment_id);
  }

  const std::string history =
      read_file(store_root / "views" / "quests" / ir_hash / "history.jsonl");
  REQUIRE(history.ends_with('\n'));
  REQUIRE(static_cast<std::size_t>(std::ranges::count(history, '\n'))
          == static_cast<std::size_t>(builder_count));
  std::string_view remaining = history;
  while (!remaining.empty()) {
    const std::size_t end = remaining.find('\n');
    REQUIRE(end != std::string_view::npos);
    const std::string_view line = remaining.substr(0, end);
    REQUIRE(json_string_field(line, "schema") == "memix.quest.resolution.v2");
    REQUIRE(json_string_field(line, "kind") == "derivation");
    REQUIRE(json_string_field(line, "id") == derivation_id);
    REQUIRE_FALSE(line.contains("environment_id"));
    remaining.remove_prefix(end + 1);
  }
}

TEST_CASE("quest CLI validates find and truncates only plain tables",
          "[quest][cli]") {
  TemporaryDirectory directory("cli");
  const fs::path store_root = directory.path() / "store";
  const CliResult invalid = run_cli(directory.path(), store_root, {"find"});
  REQUIRE(invalid.status != 0);
  REQUIRE(
      invalid.error.contains("find requires at least one term or --covers"));

  const std::string intent = "intent-abcdefghijklmnopqrstuvwxyz-0123456789";
  const std::string name = "name-abcdefghijklmnopqrstuvwxyz-0123456789";
  const CliResult defined =
      run_cli(directory.path(), store_root,
              {"define", "--source", "rules/repo/AGENTS.md@latest", "--intent",
               intent, "--name", name});
  INFO(defined.error);
  REQUIRE(defined.status == 0);

  const CliResult preserved =
      run_cli(directory.path(), store_root,
              {"define", "--source", "rules/repo/AGENTS.md@latest", "--json"});
  INFO(preserved.error);
  REQUIRE(preserved.status == 0);
  REQUIRE(preserved.output.contains(std::format(R"("intent":"{}")", intent)));
  REQUIRE(preserved.output.contains(std::format(R"("name":"{}")", name)));

  const std::string truncated_intent = intent.substr(0, 25) + "...";
  const std::string truncated_name = name.substr(0, 21) + "...";
  const CliResult quests = run_cli(directory.path(), store_root, {"quests"});
  INFO(quests.error);
  REQUIRE(quests.status == 0);
  REQUIRE(quests.output.contains(
      std::format("{}  {}", truncated_intent, truncated_name)));
  REQUIRE_FALSE(quests.output.contains(intent));
  REQUIRE_FALSE(quests.output.contains(name));

  const CliResult found =
      run_cli(directory.path(), store_root, {"find", "intent"});
  INFO(found.error);
  REQUIRE(found.status == 0);
  REQUIRE(found.output.contains(
      std::format("{}  {}", truncated_intent, truncated_name)));
  REQUIRE_FALSE(found.output.contains(intent));
  REQUIRE_FALSE(found.output.contains(name));

  const CliResult quests_json =
      run_cli(directory.path(), store_root, {"quests", "--json"});
  INFO(quests_json.error);
  REQUIRE(quests_json.status == 0);
  REQUIRE(quests_json.output.contains(std::format(R"("intent":"{}")", intent)));
  REQUIRE(quests_json.output.contains(std::format(R"("name":"{}")", name)));

  const CliResult found_json =
      run_cli(directory.path(), store_root, {"find", "intent", "--json"});
  INFO(found_json.error);
  REQUIRE(found_json.status == 0);
  REQUIRE(found_json.output.contains(std::format(R"("intent":"{}")", intent)));
  REQUIRE(found_json.output.contains(std::format(R"("name":"{}")", name)));

  const CliResult cleared =
      run_cli(directory.path(), store_root,
              {"define", "--source", "rules/repo/AGENTS.md@latest", "--intent",
               "", "--name", "", "--json"});
  INFO(cleared.error);
  REQUIRE(cleared.status == 0);
  REQUIRE(cleared.output.contains(R"("intent":"")"));
  REQUIRE(cleared.output.contains(R"("name":"")"));
}

TEST_CASE("quest stats CLI reports consumption evidence",
          "[quest][receipts][cli]") {
  TemporaryDirectory directory("stats-cli");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto environment = compose_for_test(views, *expression,
                                            memix::BuildOptions{
                                                .intent = "CLI reuse evidence",
                                                .name = "cli stats",
                                            });
  REQUIRE(environment);
  REQUIRE(
      views.activate(environment->id, {
                                          .profile = "cli",
                                          .into = directory.path() / ".memory",
                                          .mode = memix::ActivationMode::Link,
                                      }));

  const CliResult cat =
      run_cli(directory.path(), store_root,
              {"cat", environment->id, "rules/repo/AGENTS.md"});
  INFO(cat.error);
  REQUIRE(cat.status == 0);
  REQUIRE(cat.output == "rules\n");
  const CliResult tree =
      run_cli(directory.path(), store_root,
              {"tree", environment->id, "--path", "rules/repo", "--json"});
  INFO(tree.error);
  REQUIRE(tree.status == 0);
  REQUIRE(tree.output.contains(R"("path":"rules/repo/AGENTS.md")"));

  const std::string quest_prefix = environment->ir_hash.substr(0, 12);
  const CliResult plain =
      run_cli(directory.path(), store_root, {"stats", quest_prefix});
  INFO(plain.error);
  REQUIRE(plain.status == 0);
  REQUIRE(
      plain.output.contains(std::format("quest: {}", environment->ir_hash)));
  REQUIRE(plain.output.contains("resolutions: 1"));
  REQUIRE(plain.output.contains("activations: 1"));
  REQUIRE(plain.output.contains("first_activated: "));
  REQUIRE(plain.output.contains("last_activated: "));
  REQUIRE(plain.output.contains("activations_by_profile:\n  cli 1"));
  REQUIRE(plain.output.contains("reads: 2"));
  REQUIRE(plain.output.contains("cat_reads: 1"));
  REQUIRE(plain.output.contains("tree_reads: 1"));
  REQUIRE(plain.output.contains("reads_by_environment:"));
  REQUIRE(plain.output.contains(environment->id.substr(0, 12)));
  REQUIRE(plain.output.find("\nactivations:") < plain.output.find("\nreads:"));

  const CliResult json =
      run_cli(directory.path(), store_root, {"stats", quest_prefix, "--json"});
  INFO(json.error);
  REQUIRE(json.status == 0);
  REQUIRE(json.output.contains(
      std::format(R"("ir_hash":"{}")", environment->ir_hash)));
  REQUIRE(json.output.contains(R"("resolution_count":1)"));
  REQUIRE(json.output.contains(R"("activation_count":1)"));
  REQUIRE(json.output.contains(R"("first_activated_at":")"));
  REQUIRE(json.output.contains(R"("last_activated_at":")"));
  REQUIRE(json.output.contains(
      R"("activations_by_profile":[{"profile":"cli","activation_count":1}])"));
  REQUIRE(json.output.contains(R"("read_count":2)"));
  REQUIRE(json.output.contains(R"("cat_count":1)"));
  REQUIRE(json.output.contains(R"("tree_count":1)"));
  REQUIRE(json.output.contains(
      std::format(R"("environment_id":"{}","read_count":2)", environment->id)));
  REQUIRE(json.output.contains(R"("first_read_at":")"));
  REQUIRE(json.output.contains(R"("last_read_at":")"));

  const CliResult quests = run_cli(directory.path(), store_root, {"quests"});
  INFO(quests.error);
  REQUIRE(quests.status == 0);
  REQUIRE(quests.output.contains("READS"));
  REQUIRE(quests.output.contains("ACTS"));
  REQUIRE(quests.output.find("READS") < quests.output.find("ACTS"));
  REQUIRE(quests.output.contains("LAST_READ"));
  REQUIRE_FALSE(quests.output.contains("LAST_ACTIVE"));
  const CliResult found =
      run_cli(directory.path(), store_root, {"find", "evidence"});
  INFO(found.error);
  REQUIRE(found.status == 0);
  REQUIRE(found.output.contains("READS"));
  REQUIRE(found.output.contains("ACTS"));
  REQUIRE(found.output.find("READS") < found.output.find("ACTS"));
  REQUIRE(found.output.contains("LAST_READ"));
  REQUIRE_FALSE(found.output.contains("LAST_ACTIVE"));
  const CliResult found_json =
      run_cli(directory.path(), store_root, {"find", "evidence", "--json"});
  INFO(found_json.error);
  REQUIRE(found_json.status == 0);
  REQUIRE(found_json.output.contains(R"("read_count":2)"));
  REQUIRE(found_json.output.contains(R"("last_read_at":")"));
  REQUIRE(found_json.output.contains(R"("activation_count":1)"));
  REQUIRE(found_json.output.contains(R"("last_activated_at":")"));

  const CliResult quests_json =
      run_cli(directory.path(), store_root, {"quests", "--json"});
  INFO(quests_json.error);
  REQUIRE(quests_json.status == 0);
  REQUIRE(quests_json.output.contains(R"("read_count":2)"));
  REQUIRE(quests_json.output.contains(R"("last_read_at":")"));
  REQUIRE(quests_json.output.contains(R"("activation_count":1)"));
  REQUIRE(quests_json.output.contains(R"("last_activated_at":")"));
}

TEST_CASE(
    "typed history rejects torn tails and never appends through corruption",
    "[quest][d2]") {
  QuestFixture f;
  const auto d = f.select();
  const auto path = f.root / "views" / "quests" / d.ir_hash / "history.jsonl";
  auto contents = read_file(path);
  SECTION("partial JSON") { contents += "{\"schema\":"; }
  SECTION("complete record without its delimiter") { contents.pop_back(); }
  write_file(path, contents);
  const auto resolved = f.views.resolve_quest(d.ir_hash);
  REQUIRE_FALSE(resolved);
  CHECK(read_file(path) == contents);
  REQUIRE(f.views.resolve_derivation(d.id));
  CHECK(f.views.list_environments()->empty());
}

TEST_CASE("quest kind and history tags must agree with the canonical recipe",
          "[quest][d2]") {
  QuestFixture f;
  const auto d = f.select();
  const auto dir = f.root / "views" / "quests" / d.ir_hash;
  SECTION("metadata cannot relabel a selection as composition") {
    const auto path = dir / "quest.json";
    auto text = read_file(path);
    const auto at = text.find("\"result_kind\":\"derivation\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("\"result_kind\":\"derivation\"").size(),
                 "\"result_kind\":\"environment\"");
    write_file(path, text);
    CHECK_FALSE(f.views.resolve_quest(d.ir_hash));
    CHECK_FALSE(f.views.quest_stats(d.ir_hash));
  }
  SECTION("history cannot attach an Environment result to a selection") {
    const auto path = dir / "history.jsonl";
    auto text = read_file(path);
    const auto at = text.find("\"kind\":\"derivation\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("\"kind\":\"derivation\"").size(),
                 "\"kind\":\"environment\"");
    write_file(path, text);
    const auto resolved = f.views.resolve_quest(d.ir_hash);
    REQUIRE_FALSE(resolved);
    CHECK(resolved.error().message.contains("published derivation:" + d.id));
    CHECK(read_file(path) == text);
    CHECK_FALSE(f.views.quest_stats(d.ir_hash));
  }
}
