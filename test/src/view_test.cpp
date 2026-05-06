#include "memix/view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path unique_test_path(std::string_view label) {
  std::random_device random_device;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return fs::temp_directory_path()
       / std::format("memix-view-{}-{}-{}", label, nanos, random_device());
}

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label)
      : path_(unique_test_path(label)) {
    fs::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code remove_error;
    fs::remove_all(path_, remove_error);
  }

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

std::string fake_file_tree_manifest(std::string_view derivation_id,
                                    std::string_view expression_id,
                                    std::string_view agent_generation,
                                    std::string_view blob_id,
                                    std::string_view tree = "tree") {
  return std::format(
      "{{\"schema_version\":1,\"object_version\":1,\"kind\":"
      "\"file_tree_view\",\"derivation_id\":\"{}\","
      "\"expression_id\":\"{}\",\"name\":\"fake\","
      "\"operator_version\":\"file-tree-v1\","
      "\"agent_generation\":\"{}\",\"inputs\":[{{\"agent_path\":"
      "\"rules.md\",\"scope_label\":null,\"required\":true,"
      "\"selected\":true,\"blob_id\":\"{}\"}}],\"tree\":\"{}\"}}",
      derivation_id, expression_id, agent_generation, blob_id, tree);
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

memix::AgentIngestResult ingest_agent(const memix::Store &store,
                                      const fs::path &source,
                                      const fs::path &agent_path,
                                      std::string_view message) {
  const std::expected<memix::AgentIngestResult, memix::StoreError> result =
      store.ingest_agent_file({
          .source_path = source,
          .agent_path = agent_path,
          .message = message,
      });
  REQUIRE(result.has_value());
  return *result;
}

bool owner_can_write(const fs::path &path) {
  return (fs::status(path).permissions() & fs::perms::owner_write)
      != fs::perms::none;
}

void write_fake_file_tree(const fs::path &store_root,
                          std::string_view derivation_id) {
  const fs::path derivation_dir =
      store_root / "views" / "derivations" / std::string(derivation_id);
  fs::create_directories(derivation_dir / "tree");
  const std::string expression_id(64, 'b');
  const std::string agent_generation(64, 'c');
  const std::string blob_id(64, 'd');
  write_file(derivation_dir / "manifest.json",
             fake_file_tree_manifest(derivation_id, expression_id,
                                     agent_generation, blob_id));
}

void write_fake_context_pack(const fs::path &store_root,
                             std::string_view derivation_id) {
  const fs::path derivation_dir =
      store_root / "views" / "derivations" / std::string(derivation_id);
  fs::create_directories(derivation_dir);
  write_file(derivation_dir / "manifest.json",
             std::format("{{\"schema_version\":1,\"object_version\":1,\"kind\":"
                         "\"context_pack\",\"derivation_id\":\"{}\"}}",
                         derivation_id));
}

std::size_t count_temp_derivation_dirs(const fs::path &store_root,
                                       std::string_view derivation_id) {
  const fs::path derivations_dir = store_root / "views" / "derivations";
  if (!fs::exists(derivations_dir)) {
    return 0;
  }

  const std::string temp_prefix = std::format(".tmp-{}-", derivation_id);
  std::size_t count = 0;
  for (const fs::directory_entry &entry :
       fs::directory_iterator(derivations_dir)) {
    if (entry.path().filename().string().starts_with(temp_prefix)) {
      ++count;
    }
  }
  return count;
}

} // namespace

TEST_CASE("file tree builds materialize logical paths as symlinks", "[view]") {
  TemporaryDirectory directory("symlink-tree");
  const fs::path system_source = directory.path() / "system.md";
  const fs::path repo_source = directory.path() / "repo.md";
  write_file(system_source, "system rules\n");
  write_file(repo_source, "repo rules\n");

  const memix::Store store(directory.path() / "store");
  ingest_agent(store, system_source, "system/AGENTS.md", "add system");
  ingest_agent(store, repo_source, "repo/AGENTS.md", "add repo");

  const memix::ViewService views(store);
  memix::ViewExpression expression;
  expression.name = "ordered tree";
  expression.selectors = {
      memix::ViewSelector{
          .agent_path = "system/AGENTS.md",
          .scope_label = std::string("system"),
      },
      memix::ViewSelector{
          .agent_path = "repo/AGENTS.md",
          .scope_label = std::string("repo"),
      },
  };

  const auto first = views.build_file_tree(expression);
  REQUIRE(first.has_value());
  REQUIRE(first->inserted);
  const auto second = views.build_file_tree(expression);
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second->inserted);
  REQUIRE(second->derivation_id == first->derivation_id);
  REQUIRE(second->expression_id == first->expression_id);

  const fs::path system_link = first->tree_path / "system" / "AGENTS.md";
  const fs::path repo_link = first->tree_path / "repo" / "AGENTS.md";
  REQUIRE(fs::is_symlink(fs::symlink_status(system_link)));
  REQUIRE(fs::is_symlink(fs::symlink_status(repo_link)));
  REQUIRE(read_file(system_link) == "system rules\n");
  REQUIRE(read_file(repo_link) == "repo rules\n");
  REQUIRE_FALSE(owner_can_write(first->tree_path));
  REQUIRE_FALSE(fs::exists(first->tree_path.parent_path() / "context.md"));
}

TEST_CASE("concurrent file tree builds report one insertion and clean temps",
          "[view]") {
  constexpr int kInputCount = 64;
  constexpr int kBuilderCount = 16;

  TemporaryDirectory directory("concurrent-tree");
  const memix::Store store(directory.path() / "store");

  for (int index = 0; index < kInputCount; ++index) {
    const fs::path source =
        directory.path() / std::format("source-{}.md", index);
    write_file(source, std::format("rule {}\n", index));
    ingest_agent(store, source, std::format("scope/rule-{}.md", index),
                 "add rule");
  }

  memix::ViewExpression expression;
  expression.name = "concurrent tree";

  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  std::vector<std::expected<memix::ViewArtifact, memix::StoreError>> results(
      kBuilderCount);
  std::vector<std::thread> builders;
  builders.reserve(kBuilderCount);

  for (int index = 0; index < kBuilderCount; ++index) {
    builders.emplace_back([&, index]() {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const memix::ViewService views(memix::Store(store.root()));
      results.at(static_cast<std::size_t>(index)) =
          views.build_file_tree(expression);
    });
  }

  while (ready.load(std::memory_order_acquire) != kBuilderCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);

  for (std::thread &builder : builders) {
    builder.join();
  }

  std::size_t inserted_count = 0;
  std::string derivation_id;
  for (const auto &result : results) {
    const std::string error_message = result ? "" : result.error().message;
    INFO(error_message);
    REQUIRE(result.has_value());
    inserted_count += result->inserted ? 1U : 0U;
    if (derivation_id.empty()) {
      derivation_id = result->derivation_id;
    }
    REQUIRE(result->derivation_id == derivation_id);
  }

  REQUIRE(inserted_count == 1);
  REQUIRE(count_temp_derivation_dirs(store.root(), derivation_id) == 0);
}

TEST_CASE("empty selector list maps the whole latest agents tree", "[view]") {
  TemporaryDirectory directory("whole-tree");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "first\n");
  write_file(second_source, "second\n");

  const memix::Store store(directory.path() / "store");
  ingest_agent(store, first_source, "a/first.md", "first");
  ingest_agent(store, second_source, "b/second.md", "second");

  const memix::ViewService views(store);
  const auto artifact = views.build_file_tree(memix::ViewExpression{});
  REQUIRE(artifact.has_value());
  REQUIRE(artifact->input_blob_ids.size() == 2);
  REQUIRE(read_file(artifact->tree_path / "a" / "first.md") == "first\n");
  REQUIRE(read_file(artifact->tree_path / "b" / "second.md") == "second\n");
}

TEST_CASE("file trees are pinned to resolved agent generations", "[view]") {
  TemporaryDirectory directory("generation-pinning");
  const fs::path legacy_source = directory.path() / "legacy.md";
  const fs::path modern_source = directory.path() / "modern.md";
  write_file(legacy_source, "pip workflow\n");
  write_file(modern_source, "uv workflow\n");

  const memix::Store store(directory.path() / "store");
  ingest_agent(store, legacy_source, "lang/python/AGENTS.md", "legacy");

  const memix::ViewService views(store);
  memix::ViewExpression expression;
  expression.selectors = {
      memix::ViewSelector{.agent_path = "lang/python/AGENTS.md"},
  };

  const auto legacy = views.build_file_tree(expression);
  REQUIRE(legacy.has_value());
  ingest_agent(store, modern_source, "lang/python/AGENTS.md", "modern");
  const auto modern = views.build_file_tree(expression);
  REQUIRE(modern.has_value());
  REQUIRE(modern->derivation_id != legacy->derivation_id);
  REQUIRE(modern->agent_generation != legacy->agent_generation);

  const fs::path logical_path = "lang/python/AGENTS.md";
  REQUIRE(read_file(legacy->tree_path / logical_path) == "pip workflow\n");
  REQUIRE(read_file(modern->tree_path / logical_path) == "uv workflow\n");
}

TEST_CASE("optional selectors are skipped and required misses fail", "[view]") {
  TemporaryDirectory directory("optional");
  const fs::path present_source = directory.path() / "present.md";
  write_file(present_source, "present rules\n");

  const memix::Store store(directory.path() / "store");
  ingest_agent(store, present_source, "present.md", "present");

  const memix::ViewService views(store);
  memix::ViewExpression expression;
  expression.selectors = {
      memix::ViewSelector{.agent_path = "present.md"},
      memix::ViewSelector{
          .agent_path = "missing.md",
          .required = false,
      },
  };

  const auto artifact = views.build_file_tree(expression);
  REQUIRE(artifact.has_value());
  REQUIRE(artifact->input_blob_ids.size() == 1);
  REQUIRE(
      fs::is_symlink(fs::symlink_status(artifact->tree_path / "present.md")));
  REQUIRE_FALSE(fs::exists(artifact->tree_path / "missing.md"));
  const std::string manifest = read_file(artifact->manifest_path);
  REQUIRE(manifest.find("\"agent_path\":\"missing.md\"") != std::string::npos);
  REQUIRE(manifest.find("\"selected\":false") != std::string::npos);

  memix::ViewExpression required_missing;
  required_missing.selectors = {
      memix::ViewSelector{.agent_path = "missing.md"},
  };
  REQUIRE_FALSE(views.build_file_tree(required_missing).has_value());
}

TEST_CASE("file tree builds reject duplicate selectors", "[view]") {
  TemporaryDirectory directory("duplicate-selector");
  const memix::ViewService views(memix::Store(directory.path() / "store"));
  memix::ViewExpression expression;
  expression.selectors = {
      memix::ViewSelector{.agent_path = "rules.md"},
      memix::ViewSelector{.agent_path = "rules.md"},
  };

  const auto artifact = views.build_file_tree(expression);
  REQUIRE_FALSE(artifact.has_value());
  REQUIRE(artifact.error().message.find("duplicate agent path")
          != std::string::npos);
}

TEST_CASE("operator versions participate in file tree identity", "[view]") {
  TemporaryDirectory directory("operator-version");
  const fs::path source = directory.path() / "rules.md";
  write_file(source, "rules\n");

  const memix::Store store(directory.path() / "store");
  ingest_agent(store, source, "rules.md", "rules");

  const memix::ViewService views(store);
  memix::ViewExpression expression;
  expression.selectors = {
      memix::ViewSelector{.agent_path = "rules.md"},
  };

  const auto first = views.build_file_tree(expression);
  REQUIRE(first.has_value());
  expression.operator_version = "file-tree-v2";
  const auto second = views.build_file_tree(expression);
  REQUIRE(second.has_value());
  REQUIRE(second->derivation_id != first->derivation_id);
  REQUIRE(second->expression_id != first->expression_id);
}

TEST_CASE("file tree listing returns only file tree artifacts", "[view]") {
  TemporaryDirectory directory("list");
  const fs::path source = directory.path() / "rules.md";
  write_file(source, "rules\n");

  const memix::Store store(directory.path() / "store");
  ingest_agent(store, source, "rules.md", "rules");

  const memix::ViewService views(store);
  memix::ViewExpression expression;
  expression.selectors = {
      memix::ViewSelector{.agent_path = "rules.md"},
  };
  const auto artifact = views.build_file_tree(expression);
  REQUIRE(artifact.has_value());
  write_fake_context_pack(
      store.root(),
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");

  const auto artifacts = views.list_file_trees();
  REQUIRE(artifacts.has_value());
  REQUIRE(artifacts->size() == 1);
  REQUIRE((*artifacts)[0].derivation_id == artifact->derivation_id);
  REQUIRE((*artifacts)[0].inserted == false);
}

TEST_CASE("file tree listing validates manifest identity fields", "[view]") {
  TemporaryDirectory directory("manifest-validation");
  const fs::path store_root = directory.path() / "store";
  const std::string derivation_id(64, 'a');
  const std::string expression_id(64, 'b');
  const std::string agent_generation(64, 'c');
  const std::string blob_id(64, 'd');
  write_fake_file_tree(store_root, derivation_id);

  const fs::path manifest_path =
      store_root / "views" / "derivations" / derivation_id / "manifest.json";
  SECTION("expression id") {
    write_file(manifest_path,
               fake_file_tree_manifest(derivation_id, "not-a-sha256",
                                       agent_generation, blob_id));
    const auto artifacts =
        memix::ViewService{memix::Store(store_root)}.list_file_trees();
    REQUIRE_FALSE(artifacts.has_value());
    REQUIRE(artifacts.error().message.find("expression_id")
            != std::string::npos);
  }
  SECTION("input blob id") {
    write_file(manifest_path,
               fake_file_tree_manifest(derivation_id, expression_id,
                                       agent_generation, "not-a-sha256"));
    const auto artifacts =
        memix::ViewService{memix::Store(store_root)}.list_file_trees();
    REQUIRE_FALSE(artifacts.has_value());
    REQUIRE(artifacts.error().message.find("blob_id") != std::string::npos);
  }
  SECTION("tree path") {
    write_file(manifest_path,
               fake_file_tree_manifest(derivation_id, expression_id,
                                       agent_generation, blob_id, "elsewhere"));
    const auto artifacts =
        memix::ViewService{memix::Store(store_root)}.list_file_trees();
    REQUIRE_FALSE(artifacts.has_value());
    REQUIRE(artifacts.error().message.find("tree") != std::string::npos);
  }
}

TEST_CASE("file tree show accepts unique derivation prefixes", "[view]") {
  TemporaryDirectory directory("prefix");
  const fs::path source = directory.path() / "rules.md";
  write_file(source, "prefix rules\n");

  const memix::Store store(directory.path() / "store");
  ingest_agent(store, source, "rules.md", "rules");

  const memix::ViewService views(store);
  memix::ViewExpression expression;
  expression.selectors = {
      memix::ViewSelector{.agent_path = "rules.md"},
  };
  const auto artifact = views.build_file_tree(expression);
  REQUIRE(artifact.has_value());

  const std::string prefix = artifact->derivation_id.substr(0, 12);
  const auto resolved = views.resolve_file_tree(prefix);
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->derivation_id == artifact->derivation_id);
  REQUIRE(read_file(resolved->tree_path / "rules.md") == "prefix rules\n");
}

TEST_CASE("file tree show rejects ambiguous derivation prefixes", "[view]") {
  TemporaryDirectory directory("ambiguous-prefix");
  const fs::path store_root = directory.path() / "store";
  const std::string first_id = std::string(63, 'a') + "1";
  const std::string second_id = std::string(63, 'a') + "2";
  write_fake_file_tree(store_root, first_id);
  write_fake_file_tree(store_root, second_id);

  const memix::ViewService views{memix::Store(store_root)};
  const auto resolved = views.resolve_file_tree("aaaa");
  REQUIRE_FALSE(resolved.has_value());
  REQUIRE(resolved.error().message.find("ambiguous") != std::string::npos);
}
