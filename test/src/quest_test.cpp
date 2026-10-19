#include "memix/error.hpp"
#include "memix/view.hpp"

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

void erase_first_json_object_from_array(std::string &json,
                                        std::string_view key) {
  const std::string prefix = std::format(R"("{}":[)", key);
  const std::size_t field_begin = json.find(prefix);
  REQUIRE(field_begin != std::string::npos);
  const std::size_t object_begin = field_begin + prefix.size();
  REQUIRE(object_begin < json.size());
  REQUIRE(json[object_begin] == '{');
  std::size_t depth = 0;
  for (std::size_t index = object_begin; index < json.size(); ++index) {
    if (json[index] == '{') {
      ++depth;
    } else if (json[index] == '}') {
      REQUIRE(depth > 0);
      --depth;
      if (depth == 0) {
        const bool followed_by_comma =
            index + 1 < json.size() && json[index + 1] == ',';
        json.erase(object_begin,
                   index - object_begin + (followed_by_comma ? 2 : 1));
        return;
      }
    }
  }
  FAIL("unterminated JSON object");
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
  if (const char *override_binary =
          std::getenv("MEMIX_TEST_BINARY_OVERRIDE");
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

  const auto first = views.build(*expression);
  REQUIRE(first);
  REQUIRE(first->inserted);
  REQUIRE(history_line_count(store_root, first->derivation.ir_hash) == 1);
  const auto initial_quests = views.list_quests();
  REQUIRE(initial_quests);
  const memix::QuestInfo &initial =
      quest_with_hash(*initial_quests, first->derivation.ir_hash);
  REQUIRE(initial.intent.empty());
  REQUIRE(initial.name == "rules/repo/AGENTS.md@latest");
  const std::string created_at = initial.created_at;

  const auto second = views.build(*expression, memix::BuildOptions{
                                                   .intent = "Repository build",
                                                   .name = {},
                                               });
  REQUIRE(second);
  REQUIRE(second->inserted);
  REQUIRE(second->derivation.id != first->derivation.id);
  REQUIRE(second->id != first->id);
  REQUIRE(second->derivation.ir_hash == first->derivation.ir_hash);
  REQUIRE(history_line_count(store_root, first->derivation.ir_hash) == 2);

  const auto third = views.build(*expression);
  REQUIRE(third);
  REQUIRE_FALSE(third->inserted);
  REQUIRE(third->derivation.id == second->derivation.id);
  REQUIRE(history_line_count(store_root, first->derivation.ir_hash) == 3);

  const auto quests = views.list_quests();
  REQUIRE(quests);
  const memix::QuestInfo &quest =
      quest_with_hash(*quests, first->derivation.ir_hash);
  REQUIRE(quest.resolution_count == 3);
  REQUIRE(quest.intent == "Repository build");
  REQUIRE(quest.name == "rules/repo/AGENTS.md@latest");
  REQUIRE(quest.created_at == created_at);
  REQUIRE_FALSE(quest.last_resolved_at.empty());
  REQUIRE(quest.latest_derivation_id == third->derivation.id);
  REQUIRE(quest.latest_environment_id == third->id);
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
  const auto first = views.build(*latest_expression);
  REQUIRE(first);
  const auto initially_fresh = views.quest_freshness(first->derivation.ir_hash);
  REQUIRE(initially_fresh);
  REQUIRE_FALSE(initially_fresh->stale);
  REQUIRE(initially_fresh->inputs.size() == 1);
  REQUIRE(initially_fresh->inputs[0].fresh);
  REQUIRE_FALSE(initially_fresh->inputs[0].pinned);

  ingest_context_file(store, new_source, "repo/AGENTS.md", "new");
  const auto stale = views.quest_freshness(first->derivation.ir_hash);
  REQUIRE(stale);
  REQUIRE(stale->stale);
  REQUIRE_FALSE(stale->inputs[0].fresh);

  const auto resolved = views.resolve_quest(first->derivation.ir_hash);
  REQUIRE(resolved);
  REQUIRE(resolved->derivation.id != first->derivation.id);
  REQUIRE(resolved->id != first->id);
  REQUIRE(history_line_count(store_root, first->derivation.ir_hash) == 2);

  const memix::FsResult<bool> ref_removed =
      fs::remove(store_root / "refs" / "rules" / "latest");
  REQUIRE(ref_removed);
  REQUIRE(*ref_removed);
  const auto missing_ref = views.quest_freshness(first->derivation.ir_hash);
  REQUIRE(missing_ref);
  REQUIRE(missing_ref->stale);
  REQUIRE(missing_ref->inputs.size() == 1);
  REQUIRE_FALSE(missing_ref->inputs[0].pinned);
  REQUIRE_FALSE(missing_ref->inputs[0].fresh);
  REQUIRE_FALSE(missing_ref->inputs[0].current_generation.has_value());

  const auto pinned_expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@generation/000001"});
  REQUIRE(pinned_expression);
  const auto pinned = views.build(*pinned_expression);
  REQUIRE(pinned);
  ingest_context_file(store, newest_source, "repo/AGENTS.md", "newest");
  const auto pinned_freshness =
      views.quest_freshness(pinned->derivation.ir_hash);
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

  const auto environment = views.build(*expression);
  REQUIRE(environment);
  REQUIRE(environment->derivation.resolved_files.size() == 1);
  REQUIRE(environment->derivation.input_resolutions.has_value());
  REQUIRE(environment->derivation.input_resolutions->size() == 2);
  const std::string manifest =
      read_file(environment->derivation.manifest_path);
  REQUIRE(manifest.contains(R"("input_resolutions":[)"));

  ingest_context_file(store, second_source, "repo/AGENTS.md", "second");
  const auto freshness =
      views.quest_freshness(environment->derivation.ir_hash);
  REQUIRE(freshness);
  REQUIRE(freshness->inputs.size() == 2);
  const auto pinned = std::ranges::find(
      freshness->inputs, true, &memix::QuestFreshnessInput::pinned);
  const auto followed = std::ranges::find(
      freshness->inputs, false, &memix::QuestFreshnessInput::pinned);
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
      .schema = "memix.view.ir.v1",
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
                  .environment_id = {},
                  .mount = {},
              },
          },
      .target = {},
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
  REQUIRE(environment->derivation.id == derivation->id);
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

TEST_CASE("legacy freshness fallback never skips an absent locator",
          "[quest]") {
  TemporaryDirectory directory("freshness-legacy");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "first\n");
  write_file(second_source, "second\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, first_source, "repo/a.md", "first");
  ingest_context_file(store, second_source, "repo/b.md", "second");
  const memix::ViewService views(store);
  const auto expression = views.expression_from_sources({
      "rules/repo/a.md@latest",
      "rules/repo/b.md@latest",
  });
  REQUIRE(expression);
  const auto environment = views.build(*expression);
  REQUIRE(environment);

  const fs::path manifest_path = environment->derivation.manifest_path;
  memix::ViewDerivation legacy = environment->derivation;
  legacy.runtime_quest.reset();
  const auto legacy_json = memix::view_derivation_json(legacy);
  REQUIRE(legacy_json);
  std::string legacy_manifest = *legacy_json;
  erase_json_array_field(legacy_manifest, "input_resolutions");
  REQUIRE(fs::permissions(manifest_path.parent_path(), fs::perms::owner_all));
  REQUIRE(fs::permissions(manifest_path, fs::perms::owner_all));
  write_file(manifest_path, legacy_manifest);

  const auto complete_legacy =
      views.quest_freshness(environment->derivation.ir_hash);
  REQUIRE(complete_legacy);
  REQUIRE_FALSE(complete_legacy->stale);
  REQUIRE(complete_legacy->inputs.size() == 2);
  REQUIRE(std::ranges::all_of(
      complete_legacy->inputs, &memix::QuestFreshnessInput::fresh));

  erase_first_json_object_from_array(legacy_manifest, "resolved_inputs");
  write_file(manifest_path, legacy_manifest);
  const auto incomplete_legacy =
      views.quest_freshness(environment->derivation.ir_hash);
  REQUIRE(incomplete_legacy);
  REQUIRE(incomplete_legacy->stale);
  REQUIRE(incomplete_legacy->inputs.size() == 2);
  REQUIRE(std::ranges::count(
              incomplete_legacy->inputs, false,
              &memix::QuestFreshnessInput::fresh)
          == 1);
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
      views.build(*matching_expression, memix::BuildOptions{
                                            .intent = "needle metadata",
                                            .name = "matching",
                                        });
  const auto excluded =
      views.define_quest(*excluded_expression, "unrelated metadata", "");
  REQUIRE(matching);
  REQUIRE(excluded);
  const auto read =
      views.read_environment_file(matching->id, "rules/repo/matching.md");
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
  REQUIRE(matches->front().ir_hash == matching->derivation.ir_hash);
  REQUIRE(matches->front().resolution_count == 1);
  REQUIRE(matches->front().read_count == 1);
}

TEST_CASE("derivation manifests add created_at and legacy manifests parse",
          "[quest]") {
  TemporaryDirectory directory("legacy-manifest");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const memix::Store store(directory.path() / "store");
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto environment = views.build(*expression);
  REQUIRE(environment);

  std::string manifest = read_file(environment->derivation.manifest_path);
  const std::string original_manifest = manifest;
  const auto rebuilt = views.build(*expression);
  REQUIRE(rebuilt);
  REQUIRE(read_file(environment->derivation.manifest_path)
          == original_manifest);

  const std::string key = R"(,"created_at":")";
  const std::size_t begin = manifest.find(key);
  REQUIRE(begin != std::string::npos);
  const std::size_t end = manifest.find('"', begin + key.size());
  REQUIRE(end != std::string::npos);
  manifest.erase(begin, end - begin + 1);

  REQUIRE(fs::permissions(environment->derivation.manifest_path.parent_path(),
                          fs::perms::owner_all));
  REQUIRE(fs::permissions(environment->derivation.manifest_path,
                          fs::perms::owner_all));
  write_file(environment->derivation.manifest_path, manifest);
  const auto resolved = views.resolve_environment(environment->id);
  REQUIRE(resolved);
  REQUIRE(resolved->derivation.id == environment->derivation.id);
  const auto rebuilt_legacy = views.build(*expression);
  REQUIRE(rebuilt_legacy);
  REQUIRE(read_file(environment->derivation.manifest_path) == manifest);
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
  const auto built = views.build(*build_expression);
  REQUIRE(built);

  const auto resolved_full = views.resolve_quest(built->derivation.ir_hash);
  REQUIRE(resolved_full);
  REQUIRE(resolved_full->derivation.id == built->derivation.id);
  REQUIRE(history_line_count(store_root, built->derivation.ir_hash) == 2);

  const auto empty = views.resolve_quest("");
  REQUIRE_FALSE(empty);
  REQUIRE(empty.error().kind == memix::ErrorKind::InvalidArgument);
  const auto non_hex = views.resolve_quest("not-hex");
  REQUIRE_FALSE(non_hex);
  REQUIRE(non_hex.error().kind == memix::ErrorKind::InvalidArgument);

  std::map<char, std::vector<std::string>> ids_by_first_digit;
  ids_by_first_digit[built->derivation.ir_hash.front()].push_back(
      built->derivation.ir_hash);
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
  REQUIRE(corrupt_history_stats);
  REQUIRE(corrupt_history_stats->resolution_count == 0);
  REQUIRE(corrupt_history_stats->read_count == 0);

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
  write_file(ir_path, R"({"schema":"memix.view.ir.v1"})");
  const auto resolved = views.resolve_quest(definition->ir_hash);
  REQUIRE_FALSE(resolved);
  REQUIRE(resolved.error().kind == memix::ErrorKind::InvalidData);
}

TEST_CASE("composed quest builds persist timestamps history and freshness",
          "[quest]") {
  TemporaryDirectory directory("composed");
  const fs::path rules_source = directory.path() / "AGENTS.md";
  const fs::path document_source = directory.path() / "design.md";
  write_file(rules_source, "rules\n");
  write_file(document_source, "design\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, rules_source, "repo/AGENTS.md", "rules");
  ingest_context_file(store, memix::MemoryObjectType::Documents,
                      document_source, "notes/design.md", "document");
  const memix::ViewService views(store);
  const auto base_expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(base_expression);
  const auto base = views.build(*base_expression);
  REQUIRE(base);

  const auto composed_expression = views.expression_from_inputs({
      memix::AuthoredViewInput{
          .kind = memix::ViewInputKind::Environment,
          .locator = {},
          .environment_id = base->id,
          .mount = "prior",
      },
      memix::AuthoredViewInput{
          .kind = memix::ViewInputKind::Context,
          .locator = "documents/notes/design.md@latest",
          .environment_id = {},
          .mount = {},
      },
  });
  REQUIRE(composed_expression);
  const auto composed =
      views.build(*composed_expression, memix::BuildOptions{
                                            .intent = "Composed context",
                                            .name = "composed",
                                        });
  REQUIRE(composed);
  REQUIRE(composed->derivation.environment_inputs.size() == 1);
  REQUIRE(history_line_count(store_root, composed->derivation.ir_hash) == 1);
  const std::string manifest = read_file(composed->derivation.manifest_path);
  REQUIRE(manifest.contains(R"("schema":"memix.view.derivation.v3")"));
  REQUIRE(manifest.contains(R"("created_at":")"));
  REQUIRE(manifest.contains(R"("input_resolutions":[)"));
  REQUIRE(manifest.contains(R"("resolved_view_inputs":[)"));
  REQUIRE(composed->derivation.input_resolutions.has_value());
  REQUIRE(composed->derivation.input_resolutions->size() == 1);

  const auto freshness = views.quest_freshness(composed->derivation.ir_hash);
  REQUIRE(freshness);
  REQUIRE_FALSE(freshness->stale);
  REQUIRE(freshness->inputs.size() == 1);
  REQUIRE(freshness->inputs.front().locator
          == "documents/notes/design.md@latest");
  REQUIRE_FALSE(freshness->inputs.front().pinned);

  const auto rebuilt = views.build(*composed_expression);
  const std::string rebuild_error =
      rebuilt.has_value() ? "" : rebuilt.error().message;
  INFO(rebuild_error);
  REQUIRE(rebuilt);
  REQUIRE(rebuilt->derivation.id == composed->derivation.id);
  REQUIRE(rebuilt->id == composed->id);
  REQUIRE(read_file(composed->derivation.manifest_path) == manifest);
  REQUIRE(history_line_count(store_root, composed->derivation.ir_hash) == 2);

  const auto quests = views.list_quests();
  REQUIRE(quests);
  const memix::QuestInfo &quest =
      quest_with_hash(*quests, composed->derivation.ir_hash);
  REQUIRE(quest.intent == "Composed context");
  REQUIRE(quest.name == "composed");
  REQUIRE(quest.resolution_count == 2);

  const auto view_only_expression = views.expression_from_inputs({
      memix::AuthoredViewInput{
          .kind = memix::ViewInputKind::Environment,
          .locator = {},
          .environment_id = base->id,
          .mount = {},
      },
  });
  REQUIRE(view_only_expression);
  const auto view_only = views.build(*view_only_expression);
  REQUIRE(view_only);
  REQUIRE(history_line_count(store_root, view_only->derivation.ir_hash) == 1);
  const auto view_only_freshness =
      views.quest_freshness(view_only->derivation.ir_hash);
  REQUIRE(view_only_freshness);
  REQUIRE(view_only_freshness->inputs.empty());
  REQUIRE_FALSE(view_only_freshness->stale);
}

TEST_CASE("cat and tree append complete attributed quest receipts",
          "[quest][receipts]") {
  TemporaryDirectory directory("receipts-basic");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto environment = views.build(*expression);
  REQUIRE(environment);

  const auto file = views.read_environment_file(
      environment->id, fs::path("rules/repo/AGENTS.md"));
  REQUIRE(file);
  std::vector<std::string> receipts =
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash));
  REQUIRE(receipts.size() == 1);
  REQUIRE(json_string_field(receipts[0], "operation") == "cat");
  REQUIRE(json_string_field(receipts[0], "path") == "rules/repo/AGENTS.md");
  REQUIRE(json_string_field(receipts[0], "environment_id") == environment->id);
  REQUIRE(json_string_field(receipts[0], "derivation_id")
          == environment->derivation.id);
  REQUIRE_FALSE(json_string_field(receipts[0], "at").empty());
  REQUIRE_FALSE(receipts[0].contains("read_at"));

  const auto whole_tree = views.environment_tree(environment->id);
  REQUIRE(whole_tree);
  const auto subtree =
      views.environment_tree(environment->id, memix::ViewCatalogOptions{
                                                  .path = "rules/repo",
                                                  .depth = std::nullopt,
                                              });
  REQUIRE(subtree);
  receipts =
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash));
  REQUIRE(receipts.size() == 3);
  REQUIRE(json_string_field(receipts[1], "operation") == "tree");
  REQUIRE(json_string_field(receipts[1], "path") == ".");
  REQUIRE(json_string_field(receipts[2], "operation") == "tree");
  REQUIRE(json_string_field(receipts[2], "path") == "rules/repo");
  REQUIRE(json_string_field(receipts[2], "environment_id") == environment->id);
  REQUIRE(json_string_field(receipts[2], "derivation_id")
          == environment->derivation.id);

  const auto missing_file =
      views.read_environment_file(environment->id, "rules/missing.md");
  REQUIRE_FALSE(missing_file);
  const auto missing_tree =
      views.environment_tree(environment->id, memix::ViewCatalogOptions{
                                                  .path = "rules/missing",
                                                  .depth = std::nullopt,
                                              });
  REQUIRE_FALSE(missing_tree);
  REQUIRE(
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash))
          .size()
      == 3);
}

TEST_CASE("receipts are attributed to the quest that produced the environment",
          "[quest][receipts]") {
  TemporaryDirectory directory("receipt-attribution");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "first\n");
  write_file(second_source, "second\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, first_source, "repo/first.md", "first");
  ingest_context_file(store, second_source, "repo/second.md", "second");
  const memix::ViewService views(store);
  const auto first_expression =
      views.expression_from_sources({"rules/repo/first.md@latest"});
  const auto second_expression =
      views.expression_from_sources({"rules/repo/second.md@latest"});
  REQUIRE(first_expression);
  REQUIRE(second_expression);
  const auto first = views.build(*first_expression);
  const auto second = views.build(*second_expression);
  REQUIRE(first);
  REQUIRE(second);

  REQUIRE(views.read_environment_file(first->id, "rules/repo/first.md"));
  REQUIRE(views.read_environment_file(second->id, "rules/repo/second.md"));
  const std::vector<std::string> first_receipts =
      jsonl_lines(receipts_path(store_root, first->derivation.ir_hash));
  const std::vector<std::string> second_receipts =
      jsonl_lines(receipts_path(store_root, second->derivation.ir_hash));
  REQUIRE(first_receipts.size() == 1);
  REQUIRE(second_receipts.size() == 1);
  REQUIRE(json_string_field(first_receipts.front(), "environment_id")
          == first->id);
  REQUIRE(json_string_field(second_receipts.front(), "environment_id")
          == second->id);
}

TEST_CASE("activation receipts record intent events and skip deactivation",
          "[quest][receipts][activation]") {
  TemporaryDirectory directory("activation-receipt");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto environment = views.build(*expression);
  REQUIRE(environment);
  const fs::path attachment = directory.path() / ".memory";
  const memix::ActivationOptions options{
      .profile = "workspace",
      .into = attachment,
      .mode = memix::ActivationMode::Link,
      .force = false,
  };

  const auto first = views.activate(environment->id, options);
  REQUIRE(first);
  REQUIRE(first->generation == 1);
  std::vector<std::string> receipts =
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash));
  REQUIRE(receipts.size() == 1);
  REQUIRE(receipts.front().starts_with(R"({"at":")"));
  REQUIRE_FALSE(receipts.front().contains("read_at"));
  REQUIRE(json_string_field(receipts.front(), "operation") == "activate");
  REQUIRE(json_string_field(receipts.front(), "environment_id")
          == environment->id);
  REQUIRE(json_string_field(receipts.front(), "derivation_id")
          == environment->derivation.id);
  REQUIRE(json_string_field(receipts.front(), "profile") == "workspace");
  REQUIRE(json_string_field(receipts.front(), "mode") == "link");
  REQUIRE(receipts.front().contains(R"("generation":1)"));
  REQUIRE_FALSE(receipts.front().contains(R"("via":)"));
  REQUIRE_FALSE(receipts.front().contains(R"("path":)"));

  const auto no_op = views.activate(environment->id, options);
  REQUIRE(no_op);
  REQUIRE_FALSE(no_op->switched);
  receipts =
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash));
  REQUIRE(receipts.size() == 2);
  REQUIRE(json_string_field(receipts.back(), "operation") == "activate");
  REQUIRE(receipts.back().contains(R"("generation":1)"));

  const auto deactivated = views.deactivate(attachment);
  REQUIRE(deactivated);
  REQUIRE(
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash))
          .size()
      == 2);
}

TEST_CASE("rollback receipts identify the recommitted environment",
          "[quest][receipts][activation]") {
  TemporaryDirectory directory("rollback-receipt");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "first\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "first");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto first = views.build(*expression);
  REQUIRE(first);
  const memix::ActivationOptions options{
      .profile = "workspace",
      .into = directory.path() / ".memory",
      .mode = memix::ActivationMode::Link,
      .force = false,
  };
  REQUIRE(views.activate(first->id, options));

  write_file(source, "second\n");
  ingest_context_file(store, source, "repo/AGENTS.md", "second");
  const auto second = views.resolve_quest(first->derivation.ir_hash);
  REQUIRE(second);
  REQUIRE(second->id != first->id);
  REQUIRE(views.activate(second->id, options));

  const auto rolled_back = views.rollback("workspace");
  REQUIRE(rolled_back);
  REQUIRE(rolled_back->environment_id == first->id);
  REQUIRE(rolled_back->derivation_id == first->derivation.id);
  REQUIRE(rolled_back->generation == 1);
  const std::vector<std::string> receipts =
      jsonl_lines(receipts_path(store_root, first->derivation.ir_hash));
  REQUIRE(receipts.size() == 3);
  const std::string &rollback = receipts.back();
  REQUIRE(json_string_field(rollback, "operation") == "activate");
  REQUIRE(json_string_field(rollback, "environment_id") == first->id);
  REQUIRE(json_string_field(rollback, "derivation_id")
          == first->derivation.id);
  REQUIRE(json_string_field(rollback, "profile") == "workspace");
  REQUIRE(json_string_field(rollback, "mode") == "link");
  REQUIRE(json_string_field(rollback, "via") == "rollback");
  REQUIRE(rollback.contains(R"("generation":1)"));
}

TEST_CASE("activation receipt failures and legacy quests are best effort",
          "[quest][receipts][activation]") {
  TemporaryDirectory directory("activation-best-effort");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");

  const fs::path legacy_store_root = directory.path() / "legacy-store";
  const memix::Store legacy_store(legacy_store_root);
  ingest_context_file(legacy_store, source, "repo/AGENTS.md", "legacy");
  const memix::ViewService legacy_views(legacy_store);
  const auto legacy_expression =
      legacy_views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(legacy_expression);
  const auto legacy_environment = legacy_views.build(*legacy_expression);
  REQUIRE(legacy_environment);
  const fs::path legacy_quest_dir = legacy_store_root
                                  / "views"
                                  / "quests"
                                  / legacy_environment->derivation.ir_hash;
  const memix::FsResult<std::uintmax_t> removed =
      fs::remove_all(legacy_quest_dir);
  REQUIRE(removed);
  REQUIRE(*removed > 0);
  const auto legacy_activation =
      legacy_views.activate(legacy_environment->id,
                            {
                                .profile = "legacy",
                                .into = directory.path() / ".legacy-memory",
                                .mode = memix::ActivationMode::Link,
                            });
  REQUIRE(legacy_activation);
  const memix::FsResult<bool> legacy_receipts_exist = fs::exists(
      receipts_path(legacy_store_root, legacy_environment->derivation.ir_hash));
  REQUIRE(legacy_receipts_exist);
  REQUIRE_FALSE(*legacy_receipts_exist);

  const fs::path read_only_store_root = directory.path() / "readonly-store";
  const memix::Store read_only_store(read_only_store_root);
  ingest_context_file(read_only_store, source, "repo/AGENTS.md", "read-only");
  const memix::ViewService read_only_views(read_only_store);
  const auto read_only_expression =
      read_only_views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(read_only_expression);
  const auto read_only_environment =
      read_only_views.build(*read_only_expression);
  REQUIRE(read_only_environment);
  const memix::ActivationOptions read_only_options{
      .profile = "read-only",
      .into = directory.path() / ".readonly-memory",
      .mode = memix::ActivationMode::Link,
      .force = false,
  };
  REQUIRE(read_only_views.activate(read_only_environment->id,
                                   read_only_options));
  const fs::path receipt_log = receipts_path(
      read_only_store_root, read_only_environment->derivation.ir_hash);
  REQUIRE(jsonl_lines(receipt_log).size() == 1);
  const memix::FsVoid made_read_only =
      fs::permissions(receipt_log, fs::perms::owner_read);
  REQUIRE(made_read_only);
  const auto no_op =
      read_only_views.activate(read_only_environment->id, read_only_options);
  const memix::FsVoid restored = fs::permissions(
      receipt_log, fs::perms::owner_read | fs::perms::owner_write);
  REQUIRE(restored);
  REQUIRE(no_op);
  REQUIRE(jsonl_lines(receipt_log).size() == 1);
}

TEST_CASE("legacy reads and receipt append failures remain successful",
          "[quest][receipts]") {
  TemporaryDirectory directory("receipt-best-effort");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");

  const fs::path legacy_store_root = directory.path() / "legacy-store";
  const memix::Store legacy_store(legacy_store_root);
  ingest_context_file(legacy_store, source, "repo/AGENTS.md", "legacy");
  const memix::ViewService legacy_views(legacy_store);
  const auto legacy_expression =
      legacy_views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(legacy_expression);
  const auto legacy_environment = legacy_views.build(*legacy_expression);
  REQUIRE(legacy_environment);
  const fs::path legacy_quest_dir = legacy_store_root
                                  / "views"
                                  / "quests"
                                  / legacy_environment->derivation.ir_hash;
  const memix::FsResult<std::uintmax_t> removed =
      fs::remove_all(legacy_quest_dir);
  REQUIRE(removed);
  REQUIRE(*removed > 0);
  const auto legacy_read = legacy_views.read_environment_file(
      legacy_environment->id, "rules/repo/AGENTS.md");
  REQUIRE(legacy_read);
  const memix::FsResult<bool> legacy_receipts_exist = fs::exists(
      receipts_path(legacy_store_root, legacy_environment->derivation.ir_hash));
  REQUIRE(legacy_receipts_exist);
  REQUIRE_FALSE(*legacy_receipts_exist);

  const fs::path read_only_store_root = directory.path() / "readonly-store";
  const memix::Store read_only_store(read_only_store_root);
  ingest_context_file(read_only_store, source, "repo/AGENTS.md", "read-only");
  const memix::ViewService read_only_views(read_only_store);
  const auto read_only_expression =
      read_only_views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(read_only_expression);
  const auto read_only_environment =
      read_only_views.build(*read_only_expression);
  REQUIRE(read_only_environment);
  const fs::path blocked_receipt_log = receipts_path(
      read_only_store_root, read_only_environment->derivation.ir_hash);
  const memix::FsResult<bool> blocked =
      fs::create_directory(blocked_receipt_log);
  REQUIRE(blocked);
  REQUIRE(*blocked);
  const auto read_only_result = read_only_views.read_environment_file(
      read_only_environment->id, "rules/repo/AGENTS.md");
  const memix::FsResult<bool> receipt_is_file =
      fs::is_regular_file(blocked_receipt_log);
  REQUIRE(read_only_result);
  REQUIRE(receipt_is_file);
  REQUIRE_FALSE(*receipt_is_file);
}

TEST_CASE("quest listings summarize receipts and tolerate corrupt logs",
          "[quest][receipts]") {
  TemporaryDirectory directory("receipt-listing");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto environment = views.build(*expression);
  REQUIRE(environment);
  REQUIRE(views.read_environment_file(environment->id, "rules/repo/AGENTS.md"));

  const std::vector<std::string> receipts =
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash));
  REQUIRE(receipts.size() == 1);
  const std::string read_at = json_string_field(receipts.front(), "at");
  const auto listed = views.list_quests();
  REQUIRE(listed);
  const memix::QuestInfo &quest =
      quest_with_hash(*listed, environment->derivation.ir_hash);
  REQUIRE(quest.read_count == 1);
  REQUIRE(quest.last_read_at == read_at);

  write_file(receipts_path(store_root, environment->derivation.ir_hash),
             "not json\n");
  const auto corrupt_listing = views.list_quests();
  REQUIRE(corrupt_listing);
  const memix::QuestInfo &corrupt_quest =
      quest_with_hash(*corrupt_listing, environment->derivation.ir_hash);
  REQUIRE(corrupt_quest.read_count == 0);
  REQUIRE(corrupt_quest.last_read_at.empty());
  REQUIRE(corrupt_quest.activation_count == 0);
  REQUIRE(corrupt_quest.last_activated_at.empty());
  const auto corrupt_stats = views.quest_stats(environment->derivation.ir_hash);
  REQUIRE(corrupt_stats);
  REQUIRE(corrupt_stats->resolution_count == 1);
  REQUIRE(corrupt_stats->activation_count == 0);
  REQUIRE(corrupt_stats->first_activated_at.empty());
  REQUIRE(corrupt_stats->last_activated_at.empty());
  REQUIRE(corrupt_stats->activations_by_profile.empty());
  REQUIRE(corrupt_stats->read_count == 0);
  REQUIRE(corrupt_stats->first_read_at.empty());
  REQUIRE(corrupt_stats->last_read_at.empty());
}

TEST_CASE("quest stats aggregate reads across resolutions and environments",
          "[quest][receipts]") {
  TemporaryDirectory directory("receipt-stats");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  const fs::path third_source = directory.path() / "third.md";
  write_file(first_source, "first\n");
  write_file(second_source, "second\n");
  write_file(third_source, "third\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, first_source, "repo/AGENTS.md", "first");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto first = views.build(*expression);
  REQUIRE(first);
  REQUIRE(views.read_environment_file(first->id, "rules/repo/AGENTS.md"));
  REQUIRE(views.environment_tree(first->id));

  ingest_context_file(store, second_source, "repo/AGENTS.md", "second");
  const auto second = views.resolve_quest(first->derivation.ir_hash);
  REQUIRE(second);
  REQUIRE(second->id != first->id);
  REQUIRE(views.read_environment_file(first->id, "rules/repo/AGENTS.md"));
  REQUIRE(views.read_environment_file(second->id, "rules/repo/AGENTS.md"));
  REQUIRE(views.environment_tree(second->id, memix::ViewCatalogOptions{
                                                 .path = "rules/repo",
                                                 .depth = std::nullopt,
                                             }));

  ingest_context_file(store, third_source, "repo/AGENTS.md", "third");
  const auto third = views.resolve_quest(first->derivation.ir_hash);
  REQUIRE(third);
  REQUIRE(third->id != second->id);
  REQUIRE(views.read_environment_file(third->id, "rules/repo/AGENTS.md"));
  REQUIRE(views.read_environment_file(third->id, "rules/repo/AGENTS.md"));

  const std::vector<std::string> receipts =
      jsonl_lines(receipts_path(store_root, first->derivation.ir_hash));
  REQUIRE(receipts.size() == 7);
  const auto stats = views.quest_stats(first->derivation.ir_hash);
  REQUIRE(stats);
  REQUIRE(stats->ir_hash == first->derivation.ir_hash);
  REQUIRE(stats->resolution_count == 3);
  REQUIRE_FALSE(stats->last_resolved_at.empty());
  REQUIRE(stats->read_count == 7);
  REQUIRE(stats->first_read_at
          == json_string_field(receipts.front(), "at"));
  REQUIRE(stats->last_read_at == json_string_field(receipts.back(), "at"));
  REQUIRE(stats->cat_count == 5);
  REQUIRE(stats->tree_count == 2);
  REQUIRE(stats->reads_by_environment.size() == 3);
  REQUIRE(stats->reads_by_environment[0].environment_id == first->id);
  REQUIRE(stats->reads_by_environment[0].read_count == 3);
  const std::string lower_tied_id = std::min(second->id, third->id);
  const std::string higher_tied_id = std::max(second->id, third->id);
  REQUIRE(stats->reads_by_environment[1].environment_id == lower_tied_id);
  REQUIRE(stats->reads_by_environment[1].read_count == 2);
  REQUIRE(stats->reads_by_environment[2].environment_id == higher_tied_id);
  REQUIRE(stats->reads_by_environment[2].read_count == 2);

  const auto zero_expression =
      views.expression_from_sources({"rules/repo/OTHER.md@latest"});
  REQUIRE(zero_expression);
  const auto zero_definition =
      views.define_quest(*zero_expression, "zero reads", "");
  REQUIRE(zero_definition);
  const auto zero = views.quest_stats(zero_definition->ir_hash);
  REQUIRE(zero);
  REQUIRE(zero->resolution_count == 0);
  REQUIRE(zero->last_resolved_at.empty());
  REQUIRE(zero->read_count == 0);
  REQUIRE(zero->first_read_at.empty());
  REQUIRE(zero->last_read_at.empty());
  REQUIRE(zero->cat_count == 0);
  REQUIRE(zero->tree_count == 0);
  REQUIRE(zero->reads_by_environment.empty());

  const auto unknown = views.quest_stats(std::string(64, 'f'));
  REQUIRE_FALSE(unknown);
  REQUIRE(unknown.error().kind == memix::ErrorKind::NotFound);
}

TEST_CASE("quest stats aggregate activations by profile",
          "[quest][receipts][activation]") {
  TemporaryDirectory directory("activation-stats");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto environment = views.build(*expression);
  REQUIRE(environment);

  const memix::ActivationOptions beta{
      .profile = "beta",
      .into = directory.path() / ".memory-beta",
      .mode = memix::ActivationMode::Link,
      .force = false,
  };
  const memix::ActivationOptions alpha{
      .profile = "alpha",
      .into = directory.path() / ".memory-alpha",
      .mode = memix::ActivationMode::Link,
      .force = false,
  };
  const memix::ActivationOptions gamma{
      .profile = "gamma",
      .into = directory.path() / ".memory-gamma",
      .mode = memix::ActivationMode::Link,
      .force = false,
  };
  REQUIRE(views.activate(environment->id, beta));
  REQUIRE(views.activate(environment->id, alpha));
  REQUIRE(views.activate(environment->id, alpha));
  REQUIRE(views.activate(environment->id, gamma));
  REQUIRE(
      views.read_environment_file(environment->id, "rules/repo/AGENTS.md"));

  const std::vector<std::string> receipts =
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash));
  REQUIRE(receipts.size() == 5);
  const auto stats = views.quest_stats(environment->derivation.ir_hash);
  REQUIRE(stats);
  REQUIRE(stats->activation_count == 4);
  REQUIRE(stats->first_activated_at
          == json_string_field(receipts.front(), "at"));
  REQUIRE(stats->last_activated_at
          == json_string_field(receipts.at(3), "at"));
  REQUIRE(stats->activations_by_profile.size() == 3);
  REQUIRE(stats->activations_by_profile[0].profile == "alpha");
  REQUIRE(stats->activations_by_profile[0].activation_count == 2);
  REQUIRE(stats->activations_by_profile[1].profile == "beta");
  REQUIRE(stats->activations_by_profile[1].activation_count == 1);
  REQUIRE(stats->activations_by_profile[2].profile == "gamma");
  REQUIRE(stats->activations_by_profile[2].activation_count == 1);
  REQUIRE(stats->read_count == 1);

  const auto listed = views.list_quests();
  REQUIRE(listed);
  const memix::QuestInfo &quest =
      quest_with_hash(*listed, environment->derivation.ir_hash);
  REQUIRE(quest.activation_count == 4);
  REQUIRE(quest.last_activated_at == stats->last_activated_at);
}

TEST_CASE("receipt scans tolerate unknown operations and missing optionals",
          "[quest][receipts]") {
  TemporaryDirectory directory("receipt-forward-compatibility");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  ingest_context_file(store, source, "repo/AGENTS.md", "initial");
  const memix::ViewService views(store);
  const auto expression =
      views.expression_from_sources({"rules/repo/AGENTS.md@latest"});
  REQUIRE(expression);
  const auto environment = views.build(*expression);
  REQUIRE(environment);
  REQUIRE(
      views.read_environment_file(environment->id, "rules/repo/AGENTS.md"));
  const fs::path log =
      receipts_path(store_root, environment->derivation.ir_hash);
  append_file(
      log,
      std::format(
          R"({{"at":"future-1","operation":"future","environment_id":"{}","derivation_id":"{}"}})"
          "\n",
          environment->id, environment->derivation.id));
  append_file(
      log,
      std::format(
          R"({{"at":"future-2","operation":"activate","environment_id":"{}","derivation_id":"{}"}})"
          "\n",
          environment->id, environment->derivation.id));

  const auto stats = views.quest_stats(environment->derivation.ir_hash);
  REQUIRE(stats);
  REQUIRE(stats->read_count == 1);
  REQUIRE(stats->activation_count == 1);
  REQUIRE(stats->first_activated_at == "future-2");
  REQUIRE(stats->last_activated_at == "future-2");
  REQUIRE(stats->activations_by_profile.empty());
  const auto listed = views.list_quests();
  REQUIRE(listed);
  const memix::QuestInfo &quest =
      quest_with_hash(*listed, environment->derivation.ir_hash);
  REQUIRE(quest.read_count == 1);
  REQUIRE(quest.activation_count == 1);
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
      results.at(static_cast<std::size_t>(index)) =
          thread_views.define_quest(
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
  memix::ViewResult<memix::ViewEnvironment> built;
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
    built = thread_views.build(*expression, memix::BuildOptions{
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
  const auto environment = views.build(*expression);
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
      results.at(static_cast<std::size_t>(index)) =
          thread_views.read_environment_file(environment->id,
                                             "rules/repo/AGENTS.md");
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
      jsonl_lines(receipts_path(store_root, environment->derivation.ir_hash));
  REQUIRE(receipts.size() == static_cast<std::size_t>(reader_count));
  for (const std::string &receipt : receipts) {
    REQUIRE(receipt.starts_with(R"({"at":")"));
    REQUIRE(json_string_field(receipt, "operation") == "cat");
    REQUIRE(json_string_field(receipt, "environment_id") == environment->id);
    REQUIRE(json_string_field(receipt, "derivation_id")
            == environment->derivation.id);
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
  std::array<memix::ViewResult<memix::ViewEnvironment>, builder_count>
      results{};
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
          thread_views.build(*expression);
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
      ir_hash = result->derivation.ir_hash;
      derivation_id = result->derivation.id;
      environment_id = result->id;
    }
    REQUIRE(result->derivation.ir_hash == ir_hash);
    REQUIRE(result->derivation.id == derivation_id);
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
    REQUIRE(line.starts_with(R"({"resolved_at":")"));
    REQUIRE(line.ends_with(
        std::format(R"(","derivation_id":"{}","environment_id":"{}"}})",
                    derivation_id, environment_id)));
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
  const auto environment =
      views.build(*expression, memix::BuildOptions{
                                   .intent = "CLI reuse evidence",
                                   .name = "cli stats",
                               });
  REQUIRE(environment);
  REQUIRE(views.activate(environment->id,
                         {
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

  const std::string quest_prefix =
      environment->derivation.ir_hash.substr(0, 12);
  const CliResult plain =
      run_cli(directory.path(), store_root, {"stats", quest_prefix});
  INFO(plain.error);
  REQUIRE(plain.status == 0);
  REQUIRE(plain.output.contains(
      std::format("quest: {}", environment->derivation.ir_hash)));
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
  REQUIRE(plain.output.find("\nactivations:")
          < plain.output.find("\nreads:"));

  const CliResult json =
      run_cli(directory.path(), store_root, {"stats", quest_prefix, "--json"});
  INFO(json.error);
  REQUIRE(json.status == 0);
  REQUIRE(json.output.contains(
      std::format(R"("ir_hash":"{}")", environment->derivation.ir_hash)));
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
