#include "memix/view.hpp"

#include "detail/hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <string_view>
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

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
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

const memix::QuestInfo &
quest_with_hash(const std::vector<memix::QuestInfo> &quests,
                std::string_view ir_hash) {
  const auto found =
      std::ranges::find(quests, ir_hash, &memix::QuestInfo::ir_hash);
  REQUIRE(found != quests.end());
  return *found;
}

std::string shell_quote(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
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
  std::string command = shell_quote(MEMIX_VIEW_TEST_BINARY);
  command += " --store-root ";
  command += shell_quote(store_root.string());
  for (const std::string &argument : arguments) {
    command.push_back(' ');
    command += shell_quote(argument);
  }
  command += " >";
  command += shell_quote(output_path.string());
  command += " 2>";
  command += shell_quote(error_path.string());
  // Arguments and redirect paths are individually single-quoted above.
  const int status = std::system(command.c_str()); // NOLINT(cert-env33-c)
  return CliResult{
      .status = status,
      .output = read_file(output_path),
      .error = read_file(error_path),
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
  REQUIRE_FALSE(second->inserted);
  REQUIRE(second->derivation.id == first->derivation.id);
  REQUIRE(history_line_count(store_root, first->derivation.ir_hash) == 2);

  const auto third = views.build(*expression);
  REQUIRE(third);
  REQUIRE_FALSE(third->inserted);
  REQUIRE(third->derivation.id == first->derivation.id);
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
          .environment_id = base->id,
          .mount = "prior",
      },
      memix::AuthoredViewInput{
          .kind = memix::ViewInputKind::Context,
          .locator = "documents/notes/design.md@latest",
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
  REQUIRE(manifest.contains(R"("schema":"memix.view.derivation.v2")"));
  REQUIRE(manifest.contains(R"("created_at":")"));
  REQUIRE(manifest.contains(R"("resolved_view_inputs":[)"));

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
          .environment_id = base->id,
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
