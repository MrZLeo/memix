#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"
#include "memix/view.hpp"

#include "detail/hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = memix::fs;

static_assert(memix::to_string(memix::ActivationMode::Link) == "link");
static_assert(memix::parse_activation_mode("copy")
              == memix::ActivationMode::Copy);

namespace {

fs::path unique_test_path(std::string_view label) {
  std::random_device random_device;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const memix::FsResult<fs::path> temp_dir = fs::temp_directory_path();
  REQUIRE(temp_dir);
  return *temp_dir
       / std::format("memix-activation-{}-{}-{}", label, nanos,
                     random_device());
}

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label)
      : path_(unique_test_path(label)) {
    REQUIRE(fs::create_directories(path_));
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  TemporaryDirectory(TemporaryDirectory &&) = delete;
  TemporaryDirectory &operator=(TemporaryDirectory &&) = delete;

  ~TemporaryDirectory() {
    make_tree_writable();
    [[maybe_unused]] const auto removed = fs::remove_all(path_);
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
      } else if (!error && std::filesystem::is_regular_file(status)) {
        std::filesystem::permissions(iterator->path(),
                                     std::filesystem::perms::owner_write,
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

bool path_is_symlink(const fs::path &path) {
  std::error_code error;
  const bool value =
      std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
  REQUIRE_FALSE(error);
  return value;
}

fs::path read_link(const fs::path &path) {
  const memix::FsResult<fs::path> target = fs::read_symlink(path);
  REQUIRE(target);
  return *target;
}

fs::path activation_workspace_path(const fs::path &path) {
  fs::path parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const memix::FsResult<fs::path> canonical_parent =
      fs::weakly_canonical(parent);
  REQUIRE(canonical_parent);
  return (*canonical_parent / path.filename()).lexically_normal();
}

void ingest_rules(const memix::Store &store, const fs::path &source,
                  std::string_view message) {
  const memix::StoreResult<memix::FileIngestResult> result = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = source,
      .context_path = "repo/AGENTS.md",
      .message = message,
  });
  REQUIRE(result.has_value());
}

memix::ViewEnvironment
build_rules_environment(const memix::ViewService &views,
                        std::string_view source,
                        const memix::BuildOptions &options = {}) {
  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      views.expression_from_sources({std::string(source)});
  REQUIRE(expression.has_value());
  const memix::ViewResult<memix::ViewEnvironment> environment =
      views.build(*expression, options);
  REQUIRE(environment.has_value());
  return *environment;
}

struct ActivationFixture {
  TemporaryDirectory directory{"fixture"};
  fs::path store_root = directory.path() / "store";
  fs::path workspace = directory.path() / "workspace";
  fs::path source = directory.path() / "AGENTS.md";
  memix::Store store{store_root};
  memix::ViewService views{store};

  ActivationFixture() { REQUIRE(fs::create_directories(workspace)); }

  [[nodiscard]] fs::path memory_link() const { return workspace / ".memory"; }

  [[nodiscard]] fs::path profile_dir(std::string_view profile) const {
    return store_root / "views" / "profiles" / std::string(profile);
  }

  memix::ViewActivation
  activate(const memix::ViewEnvironment &environment, std::string_view profile,
           memix::ActivationMode mode = memix::ActivationMode::Link,
           bool force = false) {
    const memix::ViewResult<memix::ViewActivation> activation =
        views.activate(environment.id, {
                                           .profile = std::string(profile),
                                           .into = memory_link(),
                                           .mode = mode,
                                           .force = force,
                                       });
    REQUIRE(activation.has_value());
    return *activation;
  }
};

constexpr std::string_view kRulesLocator = "rules/repo/AGENTS.md";

} // namespace

TEST_CASE("activation links a workspace into the profile generation chain",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const memix::ViewActivation activation =
      fixture.activate(environment, "test-profile");
  REQUIRE(activation.profile == "test-profile");
  REQUIRE(activation.generation == 1);
  REQUIRE(activation.environment_id == environment.id);
  REQUIRE(activation.switched);
  REQUIRE(activation.materialized);

  const fs::path profile_dir = fixture.profile_dir("test-profile");
  const memix::FsResult<fs::path> canonical_root =
      fs::weakly_canonical(fixture.store_root);
  REQUIRE(canonical_root);
  const fs::path canonical_profile_dir =
      *canonical_root / "views" / "profiles" / "test-profile";
  REQUIRE(path_is_symlink(fixture.memory_link()));
  REQUIRE(read_link(fixture.memory_link())
          == canonical_profile_dir / "current");
  REQUIRE(read_link(profile_dir / "current") == fs::path("generations/1"));
  REQUIRE_FALSE(path_is_symlink(profile_dir / "generations" / "1"));
  REQUIRE_FALSE(path_is_symlink(fixture.memory_link() / "rules"));
  REQUIRE(
      path_is_symlink(fixture.memory_link() / "rules" / "repo" / "AGENTS.md"));
  REQUIRE(path_exists(profile_dir / "generations" / "1.json"));
  std::error_code hard_link_error;
  const std::uintmax_t metadata_links = std::filesystem::hard_link_count(
      profile_dir / "generations" / "1.json", hard_link_error);
  REQUIRE_FALSE(hard_link_error);
  REQUIRE(metadata_links == 1);

  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "repo rules\n");

  const std::string metadata =
      read_file(profile_dir / "generations" / "1.json");
  REQUIRE(metadata.find("\"schema\":\"memix.view.activation.v1\"")
          != std::string::npos);
  REQUIRE(metadata.find("\"generation\":1") != std::string::npos);
  REQUIRE(metadata.find(environment.id) != std::string::npos);
}

TEST_CASE("re-activating the same environment is a no-op", "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);

  fixture.activate(environment, "test-profile");
  const memix::ViewActivation second =
      fixture.activate(environment, "test-profile");
  REQUIRE(second.generation == 1);
  REQUIRE_FALSE(second.switched);
  REQUIRE_FALSE(second.materialized);
}

TEST_CASE("activation includes the corresponding quest in a read-only README",
          "[activation][quest]") {
  ActivationFixture fixture;
  memix::ActivationMode mode = memix::ActivationMode::Link;
  SECTION("link") {}
  SECTION("copy") { mode = memix::ActivationMode::Copy; }
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment = build_rules_environment(
      fixture.views, kRulesLocator,
      {.intent = "实现功能\n保留 `API` 兼容性", .name = "Repository context"});
  const std::string manifest = read_file(environment.derivation.manifest_path);
  const std::string environment_manifest = read_file(environment.manifest_path);
  const fs::path definition_path = fixture.store_root
                                 / "views"
                                 / "quests"
                                 / environment.derivation.ir_hash
                                 / "ir.json";
  const std::string definition = read_file(definition_path);
  REQUIRE(environment.derivation.runtime_quest);
  CHECK(environment.derivation.runtime_quest->contains(definition));

  // A newer resolution of the same quest must not change which environment
  // the activation README describes.
  write_file(fixture.source, "newer rules\n");
  ingest_rules(fixture.store, fixture.source, "newer rules");
  const memix::ViewEnvironment newer = build_rules_environment(
      fixture.views, kRulesLocator,
      {.intent = "Newer instructions", .name = "Newer quest name"});
  REQUIRE(newer.id != environment.id);
  fixture.activate(environment, "test-profile", mode);

  const fs::path readme_path = fixture.memory_link() / "README.md";
  const std::string readme = read_file(readme_path);
  CHECK(readme.contains(environment.id));
  CHECK_FALSE(readme.contains(newer.id));
  CHECK(readme.contains(environment.derivation.id));
  CHECK(readme.contains(environment.derivation.ir_hash));
  CHECK(readme.contains("Repository context"));
  CHECK(readme.contains("实现功能\n保留 `API` 兼容性"));
  CHECK_FALSE(readme.contains("### Definition"));
  CHECK_FALSE(readme.contains(definition));
  CHECK_FALSE(path_is_symlink(readme_path));
  const memix::FsResult<fs::file_status> status = fs::status(readme_path);
  REQUIRE(status);
  CHECK((status->permissions() & fs::perms::owner_write) == fs::perms::none);
  CHECK_FALSE(path_exists(environment.derivation.tree_path / "README.md"));
  CHECK(read_file(environment.derivation.manifest_path) == manifest);
  CHECK(read_file(environment.manifest_path) == environment_manifest);
  CHECK(read_file(definition_path) == definition);
  CHECK(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
        == "repo rules\n");
}

TEST_CASE(
    "quest instruction changes create a new environment with a new README",
    "[activation][quest]") {
  ActivationFixture fixture;
  memix::ActivationMode mode = memix::ActivationMode::Link;
  SECTION("link") {}
  SECTION("copy") { mode = memix::ActivationMode::Copy; }
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment first =
      build_rules_environment(fixture.views, kRulesLocator,
                              {.intent = "Original intent", .name = "First"});
  fixture.activate(first, "test-profile", mode);
  const std::string original = read_file(fixture.memory_link() / "README.md");
  const auto expression =
      fixture.views.expression_from_sources({std::string(kRulesLocator)});
  REQUIRE(expression);
  REQUIRE(fixture.views.define_quest(*expression, "Updated intent", "Second"));
  const memix::ViewActivation unchanged =
      fixture.activate(first, "test-profile", mode);
  CHECK(unchanged.generation == 1);
  CHECK_FALSE(unchanged.switched);
  CHECK(read_file(fixture.memory_link() / "README.md") == original);

  const fs::path other_workspace =
      fixture.directory.path() / "other" / ".memory";
  const auto elsewhere = fixture.views.activate(
      first.id,
      {.profile = "other-profile", .into = other_workspace, .mode = mode});
  REQUIRE(elsewhere);
  CHECK(read_file(other_workspace / "README.md") == original);

  const memix::ViewEnvironment updated =
      build_rules_environment(fixture.views, kRulesLocator);
  REQUIRE(first.id != updated.id);
  CHECK(first.derivation.id != updated.derivation.id);
  CHECK(first.derivation.ir_hash == updated.derivation.ir_hash);
  CHECK(first.derivation.resolved_files.front().blob.id
        == updated.derivation.resolved_files.front().blob.id);
  const memix::ViewActivation second =
      fixture.activate(updated, "test-profile", mode);
  REQUIRE(second.generation == 2);
  REQUIRE(second.switched);
  CHECK(read_file(fixture.memory_link() / "README.md")
            .contains("Updated intent"));
  CHECK(read_file(fixture.profile_dir("test-profile")
                  / "generations"
                  / "1"
                  / "README.md")
        == original);
  const memix::ViewActivation repeated =
      fixture.activate(updated, "test-profile", mode);
  CHECK(repeated.generation == 2);
  CHECK_FALSE(repeated.switched);

  const auto rolled_back = fixture.views.rollback("test-profile");
  REQUIRE(rolled_back);
  CHECK(rolled_back->generation == 1);
  CHECK(read_file(fixture.memory_link() / "README.md") == original);
  const auto resolved = fixture.views.resolve_quest(first.derivation.ir_hash);
  REQUIRE(resolved);
  CHECK(resolved->id == updated.id);
  REQUIRE(
      fixture.views.define_quest(*expression, "Original intent", "Renamed"));
  const auto renamed = fixture.views.resolve_quest(first.derivation.ir_hash);
  REQUIRE(renamed);
  CHECK(renamed->id != first.id);
  CHECK(renamed->derivation.ir_hash == first.derivation.ir_hash);
  REQUIRE(fixture.views.define_quest(*expression, "Original intent", "First"));
  const auto restored = fixture.views.resolve_quest(first.derivation.ir_hash);
  REQUIRE(restored);
  CHECK(restored->id == first.id);
  CHECK(restored->derivation.runtime_quest == first.derivation.runtime_quest);
}

TEST_CASE("activation preserves frozen instructions after deleting the quest",
          "[activation][quest]") {
  ActivationFixture fixture;
  memix::ActivationMode mode = memix::ActivationMode::Link;
  SECTION("link") {}
  SECTION("copy") { mode = memix::ActivationMode::Copy; }
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const std::string intent =
      "Frozen instructions\n\n### Definition\n\n    Keep this example";
  const memix::ViewEnvironment environment = build_rules_environment(
      fixture.views, kRulesLocator, {.intent = intent, .name = "Frozen name"});
  REQUIRE(fs::remove_all(fixture.store_root
                         / "views"
                         / "quests"
                         / environment.derivation.ir_hash));
  fixture.activate(environment, "test-profile", mode);
  const std::string readme = read_file(fixture.memory_link() / "README.md");
  CHECK(readme.contains(environment.derivation.ir_hash));
  CHECK(readme.contains(intent));
  CHECK(readme.contains("Frozen name"));
  CHECK_FALSE(readme.contains("memix.view.ir."));
  CHECK_FALSE(readme.contains("legacy"));
}

TEST_CASE(
    "activation and rollback omit definitions from previously cached READMEs",
    "[activation][quest]") {
  ActivationFixture fixture;
  memix::ActivationMode mode = memix::ActivationMode::Link;
  SECTION("link") {}
  SECTION("copy") { mode = memix::ActivationMode::Copy; }
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment = build_rules_environment(
      fixture.views, kRulesLocator,
      {.intent = "Frozen instructions", .name = "Frozen name"});
  fixture.activate(environment, "test-profile", mode);
  const std::string readme = read_file(fixture.memory_link() / "README.md");
  REQUIRE_FALSE(readme.contains("### Definition"));
  REQUIRE(environment.derivation.runtime_quest);
  const std::string previous = std::format(
      "# Activated memory\n\n- Environment: `{}`\n- Derivation: `{}`\n\n{}",
      environment.id, environment.derivation.id,
      *environment.derivation.runtime_quest);
  REQUIRE(previous.contains("### Definition"));
  const fs::path cached_readme =
      fixture.profile_dir("test-profile") / "generations" / "1" / "README.md";
  REQUIRE(fs::permissions(cached_readme, fs::perms::owner_all));
  write_file(cached_readme, previous);

  const auto activated = fixture.activate(environment, "test-profile", mode);
  CHECK(activated.environment_id == environment.id);
  CHECK(activated.derivation_id == environment.derivation.id);
  CHECK(read_file(fixture.memory_link() / "README.md") == readme);
  const auto rolled_back = fixture.views.rollback("test-profile", 1);
  REQUIRE(rolled_back);
  CHECK(rolled_back->environment_id == environment.id);
  CHECK(rolled_back->derivation_id == environment.derivation.id);
  CHECK(read_file(fixture.memory_link() / "README.md") == readme);
  const auto rebuilt = build_rules_environment(fixture.views, kRulesLocator);
  CHECK(rebuilt.id == environment.id);
  CHECK(rebuilt.derivation.runtime_quest
        == environment.derivation.runtime_quest);
}

TEST_CASE("runtime quest snapshots are checked against the derivation identity",
          "[activation][quest]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  memix::ViewEnvironment environment = build_rules_environment(
      fixture.views, kRulesLocator,
      {.intent = "Frozen instructions", .name = "Frozen name"});
  SECTION("context input") {}
  SECTION("environment input") {
    memix::AuthoredViewExpression expression;
    expression.inputs.push_back({
        .kind = memix::ViewInputKind::Environment,
        .locator = {},
        .environment_id = environment.id,
        .output_role = "file_tree",
        .mount = "prior",
    });
    const auto composed = fixture.views.build(expression);
    REQUIRE(composed);
    environment = *composed;
  }
  memix::ViewDerivation changed = environment.derivation;
  REQUIRE(changed.runtime_quest);
  *changed.runtime_quest += "\nChanged after building\n";
  const auto changed_json = memix::view_derivation_json(changed);
  REQUIRE(changed_json);
  REQUIRE(fs::permissions(changed.manifest_path, fs::perms::owner_all));
  write_file(changed.manifest_path, *changed_json);
  const auto resolved = fixture.views.resolve_environment(environment.id);
  REQUIRE_FALSE(resolved);
  CHECK(resolved.error().kind == memix::ErrorKind::InvalidData);
  const auto activated =
      fixture.views.activate(environment.id, {.profile = "test-profile",
                                              .into = fixture.memory_link()});
  REQUIRE_FALSE(activated);
  CHECK(activated.error().kind == memix::ErrorKind::InvalidData);
  CHECK_FALSE(path_exists(fixture.memory_link()));

  changed.runtime_quest.reset();
  const auto missing_json = memix::view_derivation_json(changed);
  REQUIRE(missing_json);
  std::string missing = *missing_json;
  const std::string schema_prefix = "memix.view.derivation.v";
  const auto schema = missing.find(schema_prefix);
  REQUIRE(schema != std::string::npos);
  missing[schema + schema_prefix.size()] = '3';
  write_file(changed.manifest_path, missing);
  const auto incomplete = fixture.views.resolve_environment(environment.id);
  REQUIRE_FALSE(incomplete);
  CHECK(incomplete.error().kind == memix::ErrorKind::InvalidData);
}

TEST_CASE(
    "legacy environments reproduce a fixed README on activation and rollback",
    "[activation][quest]") {
  ActivationFixture fixture;
  memix::ActivationMode mode = memix::ActivationMode::Link;
  SECTION("link") {}
  SECTION("copy") { mode = memix::ActivationMode::Copy; }
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment = build_rules_environment(
      fixture.views, kRulesLocator,
      {.intent = "Mutable instructions", .name = "Mutable name"});
  memix::ViewDerivation legacy = environment.derivation;
  legacy.runtime_quest.reset();
  const auto legacy_json = memix::view_derivation_json(legacy);
  REQUIRE(legacy_json);
  REQUIRE(fs::permissions(legacy.manifest_path, fs::perms::owner_all));
  write_file(legacy.manifest_path, *legacy_json);

  fixture.activate(environment, "test-profile", mode);
  const std::string original = read_file(fixture.memory_link() / "README.md");
  CHECK(original.contains("instructions were not recorded"));
  CHECK_FALSE(original.contains("Mutable instructions"));
  CHECK_FALSE(original.contains("Mutable name"));
  const fs::path cached_readme =
      fixture.profile_dir("test-profile") / "generations" / "1" / "README.md";
  REQUIRE(fs::permissions(cached_readme, fs::perms::owner_all));
  write_file(cached_readme, "README from an older activation\n");
  const auto same = fixture.views.rollback("test-profile", 1);
  REQUIRE(same);
  CHECK_FALSE(same->switched);
  CHECK(same->materialized);
  CHECK(read_file(fixture.memory_link() / "README.md") == original);

  const memix::ViewEnvironment updated = build_rules_environment(
      fixture.views, kRulesLocator,
      {.intent = "Updated instructions", .name = "Updated name"});
  fixture.activate(updated, "test-profile", mode);
  REQUIRE(fs::permissions(cached_readme, fs::perms::owner_all));
  write_file(cached_readme, "Another outdated activation README\n");
  REQUIRE(fs::remove_all(fixture.store_root
                         / "views"
                         / "quests"
                         / environment.derivation.ir_hash));
  const auto rolled_back = fixture.views.rollback("test-profile");
  REQUIRE(rolled_back);
  CHECK(rolled_back->generation == 1);
  CHECK(read_file(fixture.memory_link() / "README.md") == original);

  const fs::path elsewhere = fixture.directory.path() / "elsewhere" / ".memory";
  REQUIRE(fixture.views.activate(
      environment.id,
      {.profile = "other-profile", .into = elsewhere, .mode = mode}));
  CHECK(read_file(elsewhere / "README.md") == original);
  CHECK(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
        == "repo rules\n");
}

TEST_CASE(
    "activation upgrades legacy generation links and can roll back to them",
    "[activation]") {
  ActivationFixture fixture;
  memix::ActivationMode mode = memix::ActivationMode::Link;
  SECTION("link") {}
  SECTION("copy") { mode = memix::ActivationMode::Copy; }
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "test-profile", mode);
  const fs::path generation =
      fixture.profile_dir("test-profile") / "generations" / "1";
  REQUIRE(fs::permissions(generation, fs::perms::owner_all));
  REQUIRE(fs::permissions(generation / "README.md", fs::perms::owner_all));
  REQUIRE(fs::permissions(generation / "rules", fs::perms::owner_all));
  REQUIRE(fs::permissions(generation / "rules" / "repo", fs::perms::owner_all));
  REQUIRE(fs::remove_all(generation));
  REQUIRE(fs::create_directory_symlink(environment.derivation.tree_path,
                                       generation));
  const memix::ViewActivation upgraded =
      fixture.activate(environment, "test-profile", mode);
  REQUIRE(upgraded.generation == 2);
  const std::string readme = read_file(fixture.memory_link() / "README.md");
  CHECK(readme.contains(environment.derivation.ir_hash));
  const auto rolled_back = fixture.views.rollback("test-profile");
  REQUIRE(rolled_back);
  CHECK(rolled_back->generation == 1);
  CHECK(read_file(fixture.memory_link() / "README.md") == readme);
  CHECK(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
        == "repo rules\n");
}

TEST_CASE(
    "activation refuses a conflicting environment README without switching",
    "[activation]") {
  ActivationFixture fixture;
  memix::ActivationMode mode = memix::ActivationMode::Link;
  SECTION("link") {}
  SECTION("copy") { mode = memix::ActivationMode::Copy; }
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "test-profile", mode);
  const std::string original = read_file(fixture.memory_link() / "README.md");
  memix::AuthoredViewExpression expression;
  expression.inputs.push_back({
      .kind = memix::ViewInputKind::Environment,
      .locator = {},
      .environment_id = environment.id,
      .output_role = "file_tree",
      .mount = "README.md",
  });
  const auto conflicting = fixture.views.build(expression);
  REQUIRE(conflicting);
  const auto failed = fixture.views.activate(
      conflicting->id,
      {.profile = "test-profile", .into = fixture.memory_link(), .mode = mode});
  REQUIRE_FALSE(failed);
  CHECK(failed.error().kind == memix::ErrorKind::Conflict);
  CHECK(read_file(fixture.memory_link() / "README.md") == original);
  CHECK_FALSE(
      path_exists(fixture.profile_dir("test-profile") / "generations" / "2"));
  CHECK(read_file(conflicting->derivation.tree_path
                  / "README.md"
                  / "rules"
                  / "repo"
                  / "AGENTS.md")
        == "repo rules\n");
}

TEST_CASE("link activation reuses the current generation across workspaces",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const memix::ViewActivation first = fixture.activate(environment, "shared");
  const fs::path other_link = fixture.directory.path() / "other" / ".memory";
  REQUIRE(fs::create_directories(other_link.parent_path()));
  const memix::ViewResult<memix::ViewActivation> second =
      fixture.views.activate(environment.id, {
                                                 .profile = "shared",
                                                 .into = other_link,
                                             });
  REQUIRE(second.has_value());
  REQUIRE(second->generation == first.generation);
  REQUIRE_FALSE(second->switched);
  REQUIRE(second->materialized);
  REQUIRE(read_file(other_link / "rules" / "repo" / "AGENTS.md")
          == "repo rules\n");

  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> generations =
      fixture.views.generations("shared");
  REQUIRE(generations.has_value());
  REQUIRE(generations->size() == 1);
  REQUIRE(generations->front().environment_id == environment.id);
  REQUIRE(generations->front().current);

  const memix::ViewActivation third = fixture.activate(environment, "shared");
  REQUIRE(third.generation == first.generation);
  REQUIRE_FALSE(third.switched);
  REQUIRE_FALSE(third.materialized);
  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> after =
      fixture.views.generations("shared");
  REQUIRE(after.has_value());
  REQUIRE(after->size() == 1);
}

TEST_CASE("activating a new environment swaps current and keeps old "
          "generations",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(first_environment, "test-profile");

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  REQUIRE(second_environment.id != first_environment.id);

  const memix::ViewActivation activation =
      fixture.activate(second_environment, "test-profile");
  REQUIRE(activation.generation == 2);
  REQUIRE(activation.switched);

  const fs::path profile_dir = fixture.profile_dir("test-profile");
  REQUIRE(read_link(profile_dir / "current") == fs::path("generations/2"));
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v2\n");

  const fs::path old_tree =
      profile_dir / "generations" / "1" / "rules" / "repo" / "AGENTS.md";
  REQUIRE(read_file(old_tree) == "rules v1\n");
}

TEST_CASE("rollback moves current backward and forward", "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  fixture.activate(build_rules_environment(fixture.views, kRulesLocator),
                   "test-profile");
  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  fixture.activate(build_rules_environment(fixture.views, kRulesLocator),
                   "test-profile");

  const memix::ViewResult<memix::ViewActivation> rolled_back =
      fixture.views.rollback("test-profile");
  REQUIRE(rolled_back.has_value());
  REQUIRE(rolled_back->generation == 1);
  REQUIRE(rolled_back->switched);
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v1\n");

  const memix::ViewResult<memix::ViewActivation> rolled_forward =
      fixture.views.rollback("test-profile", 2);
  REQUIRE(rolled_forward.has_value());
  REQUIRE(rolled_forward->generation == 2);
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v2\n");

  const memix::ViewResult<memix::ViewActivation> missing =
      fixture.views.rollback("test-profile", 99);
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().kind == memix::ErrorKind::NotFound);

  const memix::ViewResult<memix::ViewActivation> same =
      fixture.views.rollback("test-profile", 2);
  REQUIRE(same.has_value());
  REQUIRE_FALSE(same->switched);
}

TEST_CASE("activation refuses foreign paths unless forced", "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);

  write_file(fixture.memory_link(), "user data\n");
  const memix::ViewResult<memix::ViewActivation> clobbered =
      fixture.views.activate(environment.id, {
                                                 .profile = "test-profile",
                                                 .into = fixture.memory_link(),
                                             });
  REQUIRE_FALSE(clobbered.has_value());
  REQUIRE(clobbered.error().kind == memix::ErrorKind::AlreadyExists);
  REQUIRE(read_file(fixture.memory_link()) == "user data\n");

  const memix::FsResult<bool> removed = fs::remove(fixture.memory_link());
  REQUIRE(removed.value_or(false));
  const fs::path elsewhere = fixture.directory.path() / "elsewhere";
  REQUIRE(fs::create_directories(elsewhere));
  REQUIRE(fs::create_directory_symlink(elsewhere, fixture.memory_link())
              .has_value());
  const memix::ViewResult<memix::ViewActivation> foreign_link =
      fixture.views.activate(environment.id, {
                                                 .profile = "test-profile",
                                                 .into = fixture.memory_link(),
                                             });
  REQUIRE_FALSE(foreign_link.has_value());
  REQUIRE(foreign_link.error().kind == memix::ErrorKind::AlreadyExists);

  const memix::ViewActivation forced = fixture.activate(
      environment, "test-profile", memix::ActivationMode::Link, true);
  REQUIRE(forced.materialized);
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "repo rules\n");
}

TEST_CASE("failed activation leaves the profile and linked workspaces intact",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(first_environment, "test-profile");

  const fs::path original_target = read_link(fixture.memory_link());
  const fs::path other_workspace = fixture.directory.path() / "other";
  REQUIRE(fs::create_directories(other_workspace));
  const fs::path other_link = other_workspace / ".memory";
  REQUIRE(fs::create_directory_symlink(original_target, other_link));

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const fs::path foreign = fixture.directory.path() / "foreign" / ".memory";
  REQUIRE(fs::create_directories(foreign));
  write_file(foreign / "user-data", "keep me\n");
  const memix::ViewResult<memix::ViewActivation> failed =
      fixture.views.activate(second_environment.id,
                             {
                                 .profile = "test-profile",
                                 .into = foreign,
                             });
  REQUIRE_FALSE(failed.has_value());
  REQUIRE(failed.error().kind == memix::ErrorKind::AlreadyExists);

  const fs::path profile_dir = fixture.profile_dir("test-profile");
  REQUIRE(read_link(profile_dir / "current") == fs::path("generations/1"));
  REQUIRE(read_link(fixture.memory_link()) == original_target);
  REQUIRE(read_link(other_link) == original_target);
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v1\n");
  REQUIRE(read_file(other_link / "rules" / "repo" / "AGENTS.md")
          == "rules v1\n");
  REQUIRE(read_file(foreign / "user-data") == "keep me\n");

  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> generations =
      fixture.views.generations("test-profile");
  REQUIRE(generations.has_value());
  REQUIRE(generations->size() == 1);
  REQUIRE(generations->front().environment_id == first_environment.id);
  REQUIRE(generations->front().current);
}

TEST_CASE("failed copy activation after a switch restores current and "
          "discards the generation",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(first_environment, "test-profile",
                   memix::ActivationMode::Copy);

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  REQUIRE(second_environment.id != first_environment.id);

  // Corrupt the new derivation tree with a dangling symlink so copy
  // materialization fails after the profile current has already switched.
  const fs::path tree_file =
      second_environment.derivation.tree_path / "rules" / "repo" / "AGENTS.md";
  REQUIRE(fs::permissions(tree_file.parent_path(), fs::perms::owner_all));
  const memix::FsResult<bool> removed_tree_file = fs::remove(tree_file);
  REQUIRE(removed_tree_file.value_or(false));
  REQUIRE(
      fs::create_symlink(fixture.directory.path() / "missing-blob", tree_file)
          .has_value());

  const memix::ViewResult<memix::ViewActivation> failed =
      fixture.views.activate(second_environment.id,
                             {
                                 .profile = "test-profile",
                                 .into = fixture.memory_link(),
                                 .mode = memix::ActivationMode::Copy,
                             });
  REQUIRE_FALSE(failed.has_value());

  const fs::path profile_dir = fixture.profile_dir("test-profile");
  REQUIRE(read_link(profile_dir / "current") == fs::path("generations/1"));
  REQUIRE_FALSE(path_exists(profile_dir / "generations" / "2"));
  REQUIRE_FALSE(path_exists(profile_dir / "generations" / "2.json"));

  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> generations =
      fixture.views.generations("test-profile");
  REQUIRE(generations.has_value());
  REQUIRE(generations->size() == 1);
  REQUIRE(generations->front().environment_id == first_environment.id);
  REQUIRE(generations->front().current);

  // The previously copied workspace is untouched and still current.
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v1\n");
  const memix::ViewResult<memix::ViewActivationStatus> status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(status.has_value());
  REQUIRE(status->generation == 1);
  REQUIRE_FALSE(status->stale);
}

TEST_CASE("default profile names isolate workspaces and validate names",
          "[activation]") {
  const fs::path workspace_a("/tmp/memix-test/workspace-a");
  const fs::path workspace_b("/tmp/memix-test/workspace-b");
  const std::string profile_a = memix::default_profile_name(workspace_a);
  REQUIRE(profile_a.starts_with("ws-"));
  REQUIRE(profile_a.size() == 3 + 16);
  REQUIRE(profile_a == memix::default_profile_name(workspace_a));
  REQUIRE(profile_a != memix::default_profile_name(workspace_b));

  REQUIRE(memix::validated_profile_name("default").has_value());
  REQUIRE(memix::validated_profile_name("my.team-1_x").has_value());
  REQUIRE_FALSE(memix::validated_profile_name("").has_value());
  REQUIRE_FALSE(memix::validated_profile_name("ws-custom").has_value());
  REQUIRE_FALSE(memix::validated_profile_name("Bad Name").has_value());
  REQUIRE_FALSE(memix::validated_profile_name("../escape").has_value());
  REQUIRE_FALSE(memix::validated_profile_name("-leading").has_value());
}

TEST_CASE("copy activation materializes real files and tracks staleness",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const memix::ViewActivation activation = fixture.activate(
      first_environment, "test-profile", memix::ActivationMode::Copy);
  REQUIRE(activation.materialized);

  const fs::path copied =
      fixture.memory_link() / "rules" / "repo" / "AGENTS.md";
  REQUIRE_FALSE(path_is_symlink(fixture.memory_link()));
  REQUIRE_FALSE(path_is_symlink(copied));
  REQUIRE(read_file(copied) == "rules v1\n");
  REQUIRE(path_exists(fixture.memory_link() / ".memix-activation.json"));

  const memix::ViewResult<memix::ViewActivationStatus> status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(status.has_value());
  REQUIRE(status->active);
  REQUIRE(status->mode == memix::ActivationMode::Copy);
  REQUIRE(status->profile == "test-profile");
  REQUIRE(status->generation == 1);
  REQUIRE_FALSE(status->stale);

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const fs::path other_workspace = fixture.directory.path() / "other";
  REQUIRE(fs::create_directories(other_workspace));
  const memix::ViewResult<memix::ViewActivation> other_activation =
      fixture.views.activate(second_environment.id,
                             {
                                 .profile = "test-profile",
                                 .into = other_workspace / ".memory",
                                 .mode = memix::ActivationMode::Copy,
                             });
  REQUIRE(other_activation.has_value());
  REQUIRE(other_activation->generation == 2);

  const memix::ViewResult<memix::ViewActivationStatus> stale_status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(stale_status.has_value());
  REQUIRE(stale_status->stale);
  const memix::ViewResult<memix::ViewActivationStatus> fresh_status =
      fixture.views.activation_status(other_workspace / ".memory");
  REQUIRE(fresh_status.has_value());
  REQUIRE_FALSE(fresh_status->stale);
}

TEST_CASE("copy rollback re-materializes the recorded workspace",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  fixture.activate(build_rules_environment(fixture.views, kRulesLocator),
                   "test-profile", memix::ActivationMode::Copy);

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  fixture.activate(build_rules_environment(fixture.views, kRulesLocator),
                   "test-profile", memix::ActivationMode::Copy);
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v2\n");

  const memix::ViewResult<memix::ViewActivation> rolled_back =
      fixture.views.rollback("test-profile");
  REQUIRE(rolled_back.has_value());
  REQUIRE(rolled_back->generation == 1);
  REQUIRE(rolled_back->materialized);
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v1\n");

  const memix::ViewResult<memix::ViewActivationStatus> status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(status.has_value());
  REQUIRE_FALSE(status->stale);
}

TEST_CASE("rollback restores the copy generation after changing activation "
          "mode",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const memix::ViewActivation linked =
      fixture.activate(first_environment, "test-profile");
  const memix::ViewActivation copied = fixture.activate(
      first_environment, "test-profile", memix::ActivationMode::Copy, true);
  REQUIRE(copied.generation > linked.generation);
  REQUIRE(copied.switched);
  REQUIRE(copied.materialized);

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  const memix::ViewActivation newer = fixture.activate(
      second_environment, "test-profile", memix::ActivationMode::Copy);
  REQUIRE(newer.generation > copied.generation);
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v2\n");

  const memix::ViewResult<memix::ViewActivation> rolled_back =
      fixture.views.rollback("test-profile");
  REQUIRE(rolled_back.has_value());
  REQUIRE(rolled_back->generation == copied.generation);
  REQUIRE(rolled_back->environment_id == first_environment.id);
  REQUIRE(rolled_back->materialized);
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v1\n");

  const memix::ViewResult<memix::ViewActivationStatus> status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(status.has_value());
  REQUIRE(status->mode == memix::ActivationMode::Copy);
  REQUIRE(status->profile == "test-profile");
  REQUIRE(status->generation == copied.generation);
  REQUIRE(status->environment_id == first_environment.id);
  REQUIRE_FALSE(status->stale);

  const std::string marker =
      read_file(fixture.memory_link() / ".memix-activation.json");
  REQUIRE(marker.find("\"profile\":\"test-profile\"") != std::string::npos);
  REQUIRE(marker.find(std::format("\"generation\":{}", copied.generation))
          != std::string::npos);
  REQUIRE(
      marker.find(std::format("\"environment\":\"{}\"", first_environment.id))
      != std::string::npos);
  REQUIRE(marker.find("\"mode\":\"copy\"") != std::string::npos);
  const fs::path expected_workspace =
      activation_workspace_path(fixture.memory_link());
  REQUIRE(marker.find(std::format("\"workspace\":\"{}\"",
                                  expected_workspace.generic_string()))
          != std::string::npos);
}

TEST_CASE("copy status detects a recreated profile with the same generation",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(first_environment, "test-profile",
                   memix::ActivationMode::Copy);

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  REQUIRE(second_environment.id != first_environment.id);

  const fs::path wipe_workspace =
      fixture.directory.path() / "wipe-workspace" / ".memory";
  const memix::ViewResult<memix::ViewActivation> wipe_link =
      fixture.views.activate(first_environment.id,
                             {
                                 .profile = "test-profile",
                                 .into = wipe_workspace,
                             });
  REQUIRE(wipe_link.has_value());
  const memix::ViewResult<memix::ViewDeactivation> wiped =
      fixture.views.deactivate(wipe_workspace, true);
  REQUIRE(wiped.has_value());
  REQUIRE(wiped->wiped_profile);

  const memix::ViewResult<memix::ViewActivation> recreated =
      fixture.views.activate(second_environment.id,
                             {
                                 .profile = "test-profile",
                                 .into = wipe_workspace,
                             });
  REQUIRE(recreated.has_value());
  REQUIRE(recreated->generation == 1);

  const memix::ViewResult<memix::ViewActivationStatus> status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(status.has_value());
  REQUIRE(status->generation == 1);
  REQUIRE(status->environment_id == first_environment.id);
  REQUIRE(status->stale);
}

TEST_CASE("copy activation updates the marker when changing profiles",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const memix::ViewActivation first =
      fixture.activate(environment, "profile-a", memix::ActivationMode::Copy);
  REQUIRE(first.generation == 1);
  const memix::ViewActivation second =
      fixture.activate(environment, "profile-b", memix::ActivationMode::Copy);
  REQUIRE(second.generation == 1);
  REQUIRE(second.materialized);

  const memix::ViewResult<memix::ViewActivationStatus> status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(status.has_value());
  REQUIRE(status->mode == memix::ActivationMode::Copy);
  REQUIRE(status->profile == "profile-b");
  REQUIRE(status->generation == second.generation);
  REQUIRE(status->environment_id == environment.id);
  REQUIRE_FALSE(status->stale);

  const std::string marker =
      read_file(fixture.memory_link() / ".memix-activation.json");
  REQUIRE(marker.find("\"profile\":\"profile-b\"") != std::string::npos);
  REQUIRE(marker.find("\"profile\":\"profile-a\"") == std::string::npos);
}

TEST_CASE("copy activation rejects a matching forged marker without replacing "
          "data",
          "[activation]") {
  const ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);

  constexpr std::string_view profile = "target-profile";
  const fs::path valuable = fixture.memory_link();
  REQUIRE(fs::create_directories(valuable));
  write_file(valuable / "sentinel", "valuable user data\n");

  const std::string forged_marker =
      std::format("{{\"schema\":\"memix.view.activation.marker.v1\","
                  "\"profile\":\"{}\",\"generation\":1,"
                  "\"environment\":\"{}\",\"derivation\":\"{}\","
                  "\"activated_at\":\"2026-01-01T00:00:00Z\",\"mode\":\"copy\","
                  "\"workspace\":\"{}\"}}",
                  profile, environment.id, environment.derivation.id,
                  activation_workspace_path(valuable).generic_string());
  const fs::path marker_path = valuable / ".memix-activation.json";
  write_file(marker_path, forged_marker);

  const memix::ViewResult<memix::ViewActivation> activation =
      fixture.views.activate(environment.id,
                             {
                                 .profile = std::string(profile),
                                 .into = valuable,
                                 .mode = memix::ActivationMode::Copy,
                             });
  CHECK_FALSE(activation.has_value());
  CHECK_FALSE(path_is_symlink(valuable));
  const bool sentinel_exists = path_exists(valuable / "sentinel");
  CHECK(sentinel_exists);
  if (sentinel_exists) {
    CHECK(read_file(valuable / "sentinel") == "valuable user data\n");
  }
  const bool marker_exists = path_exists(marker_path);
  CHECK(marker_exists);
  if (marker_exists) {
    CHECK(read_file(marker_path) == forged_marker);
  }

  const fs::path target_profile = fixture.profile_dir(profile);
  CHECK_FALSE(path_exists(target_profile / "current"));
  CHECK_FALSE(path_exists(target_profile / "generations"));
  CHECK_FALSE(path_exists(target_profile / "generations" / "1"));
  CHECK_FALSE(path_exists(target_profile / "generations" / "1.json"));
}

TEST_CASE("forced copy activation replaces a symlink to another copy",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const fs::path original_copy = fixture.directory.path() / "original-copy";
  const memix::ViewResult<memix::ViewActivation> original =
      fixture.views.activate(environment.id,
                             {
                                 .profile = "original-profile",
                                 .into = original_copy,
                                 .mode = memix::ActivationMode::Copy,
                             });
  REQUIRE(original.has_value());
  REQUIRE(fs::create_directory_symlink(original_copy, fixture.memory_link()));
  REQUIRE(path_is_symlink(fixture.memory_link()));

  const memix::ViewActivation replacement = fixture.activate(
      environment, "replacement-profile", memix::ActivationMode::Copy, true);
  REQUIRE(replacement.materialized);
  REQUIRE_FALSE(path_is_symlink(fixture.memory_link()));
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "repo rules\n");

  const memix::ViewResult<memix::ViewActivationStatus> replacement_status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(replacement_status.has_value());
  REQUIRE(replacement_status->profile == "replacement-profile");
  REQUIRE_FALSE(replacement_status->stale);

  const memix::ViewResult<memix::ViewActivationStatus> original_status =
      fixture.views.activation_status(original_copy);
  REQUIRE(original_status.has_value());
  REQUIRE(original_status->profile == "original-profile");
  REQUIRE_FALSE(original_status->stale);
}

TEST_CASE("re-activating a copy repairs missing files", "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "test-profile", memix::ActivationMode::Copy);

  const fs::path copied =
      fixture.memory_link() / "rules" / "repo" / "AGENTS.md";
  REQUIRE(fs::permissions(copied, fs::perms::owner_all));
  REQUIRE(fs::remove(copied).value_or(false));
  REQUIRE_FALSE(path_exists(copied));
  const fs::path readme_path = fixture.memory_link() / "README.md";
  const std::string readme = read_file(readme_path);
  REQUIRE(fs::permissions(readme_path, fs::perms::owner_all));
  REQUIRE(fs::remove(readme_path).value_or(false));

  const memix::ViewActivation repaired = fixture.activate(
      environment, "test-profile", memix::ActivationMode::Copy);
  REQUIRE(repaired.materialized);
  REQUIRE(read_file(copied) == "repo rules\n");
  REQUIRE(read_file(readme_path) == readme);
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{fixture.workspace, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    REQUIRE_FALSE(
        iterator->path().filename().string().starts_with(".tmp-old-.memory-"));
  }
  REQUIRE_FALSE(iterator_error);
}

TEST_CASE("failed copy rollback does not switch the current generation",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(first_environment, "test-profile",
                   memix::ActivationMode::Copy);

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  const memix::ViewActivation second = fixture.activate(
      second_environment, "test-profile", memix::ActivationMode::Copy);

  const fs::path marker = fixture.memory_link() / ".memix-activation.json";
  REQUIRE(fs::permissions(marker, fs::perms::owner_all));
  write_file(marker, "{not valid json\n");
  const memix::ViewResult<memix::ViewActivation> rolled_back =
      fixture.views.rollback("test-profile");
  REQUIRE_FALSE(rolled_back.has_value());

  REQUIRE(read_link(fixture.profile_dir("test-profile") / "current")
          == fs::path(std::format("generations/{}", second.generation)));
  REQUIRE(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
          == "rules v2\n");
  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> generations =
      fixture.views.generations("test-profile");
  REQUIRE(generations.has_value());
  REQUIRE(generations->back().number == second.generation);
  REQUIRE(generations->back().current);
}

TEST_CASE("activation status resolves link chains and rejects foreign paths",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "test-profile");

  const memix::ViewResult<memix::ViewActivationStatus> status =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(status.has_value());
  REQUIRE(status->active);
  REQUIRE(status->mode == memix::ActivationMode::Link);
  REQUIRE(status->profile == "test-profile");
  REQUIRE(status->generation == 1);
  REQUIRE(status->environment_id == environment.id);
  REQUIRE(status->derivation_id == environment.derivation.id);

  const fs::path untouched = fixture.workspace / "nothing-here";
  const memix::ViewResult<memix::ViewActivationStatus> inactive =
      fixture.views.activation_status(untouched);
  REQUIRE(inactive.has_value());
  REQUIRE_FALSE(inactive->active);

  const fs::path foreign = fixture.workspace / "foreign";
  REQUIRE(fs::create_directories(foreign));
  const memix::ViewResult<memix::ViewActivationStatus> foreign_status =
      fixture.views.activation_status(foreign);
  REQUIRE_FALSE(foreign_status.has_value());
  REQUIRE(foreign_status.error().kind == memix::ErrorKind::NotFound);
}

TEST_CASE("deactivate removes the workspace link and optionally wipes the "
          "profile",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "test-profile");

  const memix::ViewResult<memix::ViewDeactivation> deactivation =
      fixture.views.deactivate(fixture.memory_link());
  REQUIRE(deactivation.has_value());
  REQUIRE(deactivation->profile == "test-profile");
  REQUIRE_FALSE(deactivation->wiped_profile);
  REQUIRE_FALSE(path_exists(fixture.memory_link()));
  REQUIRE(path_exists(fixture.profile_dir("test-profile")));

  fixture.activate(environment, "test-profile", memix::ActivationMode::Copy);
  const fs::path copied =
      fixture.memory_link() / "rules" / "repo" / "AGENTS.md";
  const memix::FsResult<fs::file_status> copied_status = fs::status(copied);
  REQUIRE(copied_status.has_value());
  REQUIRE((copied_status->permissions() & fs::perms::owner_write)
          == fs::perms::none);
  const memix::FsResult<fs::file_status> tree_status_before =
      fs::status(environment.derivation.tree_path);
  REQUIRE(tree_status_before.has_value());
  const memix::ViewResult<memix::ViewDeactivation> wiped =
      fixture.views.deactivate(fixture.memory_link(), true);
  REQUIRE(wiped.has_value());
  REQUIRE(wiped->wiped_profile);
  REQUIRE_FALSE(path_exists(fixture.profile_dir("test-profile")));
  const memix::FsResult<fs::file_status> tree_status_after =
      fs::status(environment.derivation.tree_path);
  REQUIRE(tree_status_after.has_value());
  REQUIRE(tree_status_after->permissions()
          == tree_status_before->permissions());

  const memix::ViewResult<memix::ViewDeactivation> missing =
      fixture.views.deactivate(fixture.memory_link());
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().kind == memix::ErrorKind::NotFound);
}

TEST_CASE("deactivate rejects a self-referential marker for a missing "
          "profile",
          "[activation]") {
  const ActivationFixture fixture;
  const fs::path copied = fixture.memory_link();
  REQUIRE(fs::create_directories(copied));
  write_file(copied / "sentinel", "keep me\n");
  write_file(copied / ".memix-activation.json",
             std::format("{{\"schema\":\"memix.view.activation.marker.v1\","
                         "\"profile\":\"missing-profile\",\"generation\":1,"
                         "\"environment\":\"forged-environment\","
                         "\"derivation\":\"forged-derivation\","
                         "\"activated_at\":\"2026-01-01T00:00:00Z\","
                         "\"mode\":\"copy\",\"workspace\":\"{}\"}}",
                         activation_workspace_path(copied).generic_string()));

  const memix::ViewResult<memix::ViewDeactivation> deactivated =
      fixture.views.deactivate(copied);
  CHECK_FALSE(deactivated.has_value());
  const bool sentinel_exists = path_exists(copied / "sentinel");
  CHECK(sentinel_exists);
  CHECK(path_exists(copied / ".memix-activation.json"));
  if (sentinel_exists) {
    CHECK(read_file(copied / "sentinel") == "keep me\n");
  }
}

TEST_CASE("deactivate refuses multiply linked copy files", "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "test-profile", memix::ActivationMode::Copy);

  const fs::path copied =
      fixture.memory_link() / "rules" / "repo" / "AGENTS.md";
  REQUIRE(fs::permissions(copied, fs::perms::owner_all));
  REQUIRE(fs::remove(copied).value_or(false));
  const fs::path external = fixture.directory.path() / "external-rules";
  write_file(external, "external data\n");
  REQUIRE(fs::apply_read_only_file_permissions(external));
  const memix::FsResult<fs::file_status> external_status_before =
      fs::status(external);
  REQUIRE(external_status_before.has_value());
  REQUIRE(fs::create_hard_link(external, copied));

  const memix::ViewResult<memix::ViewDeactivation> deactivated =
      fixture.views.deactivate(fixture.memory_link());
  REQUIRE_FALSE(deactivated.has_value());
  REQUIRE(deactivated.error().kind == memix::ErrorKind::Conflict);
  REQUIRE(path_exists(fixture.memory_link()));
  REQUIRE(read_file(copied) == "external data\n");
  REQUIRE(read_file(external) == "external data\n");
  const memix::FsResult<fs::file_status> external_status_after =
      fs::status(external);
  REQUIRE(external_status_after.has_value());
  REQUIRE(external_status_after->permissions()
          == external_status_before->permissions());
}

TEST_CASE("a held profile lock blocks deactivation", "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "test-profile", memix::ActivationMode::Copy);

  const fs::path profile_dir = fixture.profile_dir("test-profile");
  const fs::path lock = profile_dir / "lock";
  REQUIRE(fs::create_directory(lock).value_or(false));
  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> generations =
      fixture.views.generations("test-profile");
  CHECK_FALSE(generations.has_value());
  if (!generations.has_value()) {
    CHECK(generations.error().kind == memix::ErrorKind::Locked);
  }
  CHECK(path_exists(lock));
  const memix::ViewResult<memix::ViewDeactivation> deactivated =
      fixture.views.deactivate(fixture.memory_link(), true);
  CHECK_FALSE(deactivated.has_value());
  if (!deactivated.has_value()) {
    CHECK(deactivated.error().kind == memix::ErrorKind::Locked);
  }
  CHECK(path_exists(fixture.memory_link() / "rules" / "repo" / "AGENTS.md"));
  CHECK(path_exists(fixture.memory_link() / ".memix-activation.json"));
  CHECK(path_exists(profile_dir / "current"));
  CHECK(path_exists(profile_dir / "generations" / "1.json"));

  if (path_exists(lock)) {
    REQUIRE(fs::remove(lock).value_or(false));
  }

  const fs::path linked_workspace =
      fixture.directory.path() / "linked-workspace" / ".memory";
  const memix::ViewResult<memix::ViewActivation> linked =
      fixture.views.activate(environment.id, {
                                                 .profile = "test-profile",
                                                 .into = linked_workspace,
                                             });
  REQUIRE(linked.has_value());
  REQUIRE(fs::create_directory(lock).value_or(false));
  const memix::ViewResult<memix::ViewDeactivation> link_deactivation =
      fixture.views.deactivate(linked_workspace);
  CHECK_FALSE(link_deactivation.has_value());
  if (!link_deactivation.has_value()) {
    CHECK(link_deactivation.error().kind == memix::ErrorKind::Locked);
  }
  CHECK(path_is_symlink(linked_workspace));
  CHECK(path_exists(profile_dir / "current"));
  if (path_exists(lock)) {
    REQUIRE(fs::remove(lock).value_or(false));
  }
}

TEST_CASE("a held workspace lock blocks cross-profile mutation",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "original-profile");

  const fs::path original_target = read_link(fixture.memory_link());
  const fs::path attach_path = activation_workspace_path(fixture.memory_link());
  const fs::path workspace_lock =
      attach_path.parent_path()
      / std::format(".memix-activation-lock-{}",
                    memix::detail::sha256_text(attach_path.generic_string()));
  REQUIRE(fs::create_directory(workspace_lock).value_or(false));

  const memix::ViewResult<memix::ViewActivation> cross_profile =
      fixture.views.activate(environment.id,
                             {
                                 .profile = "replacement-profile",
                                 .into = fixture.memory_link(),
                                 .force = true,
                             });
  CHECK_FALSE(cross_profile.has_value());
  if (!cross_profile.has_value()) {
    CHECK(cross_profile.error().kind == memix::ErrorKind::Locked);
  }
  CHECK(path_is_symlink(fixture.memory_link()));
  CHECK(read_link(fixture.memory_link()) == original_target);
  CHECK(read_file(fixture.memory_link() / "rules" / "repo" / "AGENTS.md")
        == "repo rules\n");
  CHECK(read_link(fixture.profile_dir("original-profile") / "current")
        == fs::path("generations/1"));
  CHECK_FALSE(
      path_exists(fixture.profile_dir("replacement-profile") / "current"));

  const memix::ViewResult<memix::ViewDeactivation> deactivated =
      fixture.views.deactivate(fixture.memory_link());
  CHECK_FALSE(deactivated.has_value());
  if (!deactivated.has_value()) {
    CHECK(deactivated.error().kind == memix::ErrorKind::Locked);
  }
  CHECK(path_is_symlink(fixture.memory_link()));
  CHECK(read_link(fixture.memory_link()) == original_target);
  CHECK(path_exists(fixture.profile_dir("original-profile") / "current"));

  REQUIRE(fs::remove(workspace_lock).value_or(false));
}

TEST_CASE("profile wipe rejects forged metadata for a valid copy profile",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(environment, "test-profile", memix::ActivationMode::Copy);

  const fs::path marker_path = fixture.memory_link() / ".memix-activation.json";
  std::string marker = read_file(marker_path);
  const std::size_t environment_position = marker.find(environment.id);
  REQUIRE(environment_position != std::string::npos);
  marker.replace(environment_position, environment.id.size(),
                 "forged-environment");
  const std::size_t derivation_position =
      marker.find(environment.derivation.id);
  REQUIRE(derivation_position != std::string::npos);
  marker.replace(derivation_position, environment.derivation.id.size(),
                 "forged-derivation");
  REQUIRE(fs::permissions(marker_path, fs::perms::owner_all));
  write_file(marker_path, marker);

  const memix::ViewResult<memix::ViewDeactivation> deactivated =
      fixture.views.deactivate(fixture.memory_link(), true);
  CHECK_FALSE(deactivated.has_value());
  if (!deactivated.has_value()) {
    CHECK(deactivated.error().kind == memix::ErrorKind::InvalidData);
  }
  CHECK(path_exists(marker_path));
  CHECK(path_exists(fixture.memory_link() / "rules" / "repo" / "AGENTS.md"));
  CHECK(path_exists(fixture.profile_dir("test-profile") / "current"));
  CHECK(path_exists(
      fixture.profile_dir("test-profile") / "generations" / "1.json"));
}

TEST_CASE("profile wipe accepts a legitimate stale copy generation",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(first_environment, "test-profile",
                   memix::ActivationMode::Copy);

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  const fs::path other_link =
      fixture.directory.path() / "other-workspace" / ".memory";
  const memix::ViewResult<memix::ViewActivation> newer = fixture.views.activate(
      second_environment.id, {
                                 .profile = "test-profile",
                                 .into = other_link,
                             });
  REQUIRE(newer.has_value());

  const memix::ViewResult<memix::ViewActivationStatus> stale =
      fixture.views.activation_status(fixture.memory_link());
  REQUIRE(stale.has_value());
  REQUIRE(stale->stale);
  const memix::ViewResult<memix::ViewDeactivation> wiped =
      fixture.views.deactivate(fixture.memory_link(), true);
  REQUIRE(wiped.has_value());
  REQUIRE(wiped->wiped_profile);
  REQUIRE_FALSE(path_exists(fixture.memory_link()));
  REQUIRE_FALSE(path_exists(fixture.profile_dir("test-profile")));
  REQUIRE(path_is_symlink(other_link));
}

TEST_CASE("unsafe copy marker profiles are rejected before deactivation",
          "[activation]") {
  const ActivationFixture fixture;
  const fs::path victim = fixture.directory.path() / "external-victim";
  REQUIRE(fs::create_directories(victim));
  write_file(victim / "sentinel", "keep me\n");

  const fs::path copied = fixture.memory_link();
  REQUIRE(fs::create_directories(copied));
  write_file(copied / ".memix-activation.json",
             std::format("{{\"schema\":\"memix.view.activation.marker.v1\","
                         "\"profile\":\"{}\",\"generation\":1,"
                         "\"environment\":\"forged-environment\","
                         "\"derivation\":\"forged-derivation\","
                         "\"activated_at\":\"2026-01-01T00:00:00Z\","
                         "\"mode\":\"copy\",\"workspace\":\"{}\"}}",
                         victim.generic_string(), copied.generic_string()));

  const memix::ViewResult<memix::ViewActivationStatus> status =
      fixture.views.activation_status(copied);
  CHECK_FALSE(status.has_value());
  const memix::ViewResult<memix::ViewDeactivation> deactivated =
      fixture.views.deactivate(copied, true);
  CHECK_FALSE(deactivated.has_value());
  CHECK(path_exists(copied / ".memix-activation.json"));
  CHECK(path_exists(victim / "sentinel"));
  CHECK(read_file(victim / "sentinel") == "keep me\n");
}

TEST_CASE("generations lists recorded activations with the current marker",
          "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "rules v1\n");
  ingest_rules(fixture.store, fixture.source, "v1");
  const memix::ViewEnvironment first_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(first_environment, "test-profile");

  write_file(fixture.source, "rules v2\n");
  ingest_rules(fixture.store, fixture.source, "v2");
  const memix::ViewEnvironment second_environment =
      build_rules_environment(fixture.views, kRulesLocator);
  fixture.activate(second_environment, "test-profile");

  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> generations =
      fixture.views.generations("test-profile");
  REQUIRE(generations.has_value());
  REQUIRE(generations->size() == 2);
  REQUIRE((*generations)[0].number == 1);
  REQUIRE((*generations)[0].environment_id == first_environment.id);
  REQUIRE_FALSE((*generations)[0].current);
  REQUIRE((*generations)[1].number == 2);
  REQUIRE((*generations)[1].environment_id == second_environment.id);
  REQUIRE((*generations)[1].current);
  REQUIRE_FALSE((*generations)[1].activated_at.empty());

  const memix::ViewResult<memix::ViewActivation> rolled_back =
      fixture.views.rollback("test-profile");
  REQUIRE(rolled_back.has_value());
  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> after =
      fixture.views.generations("test-profile");
  REQUIRE(after.has_value());
  REQUIRE((*after)[0].current);
  REQUIRE_FALSE((*after)[1].current);

  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> missing =
      fixture.views.generations("no-such-profile");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().kind == memix::ErrorKind::NotFound);
}

TEST_CASE("a held profile lock blocks activation", "[activation]") {
  ActivationFixture fixture;
  write_file(fixture.source, "repo rules\n");
  ingest_rules(fixture.store, fixture.source, "rules");
  const memix::ViewEnvironment environment =
      build_rules_environment(fixture.views, kRulesLocator);

  const fs::path lock = fixture.profile_dir("test-profile") / "lock";
  REQUIRE(fs::create_directories(lock));
  const memix::ViewResult<memix::ViewActivation> blocked =
      fixture.views.activate(environment.id, {
                                                 .profile = "test-profile",
                                                 .into = fixture.memory_link(),
                                             });
  REQUIRE_FALSE(blocked.has_value());
  REQUIRE(blocked.error().kind == memix::ErrorKind::Locked);

  REQUIRE(fs::remove(lock).value_or(false));
  const memix::ViewActivation activation =
      fixture.activate(environment, "test-profile");
  REQUIRE(activation.switched);
  REQUIRE_FALSE(path_exists(lock));
}

TEST_CASE("rollback does not create a missing profile", "[activation]") {
  ActivationFixture fixture;
  const fs::path missing_profile = fixture.profile_dir("missing-profile");

  const memix::ViewResult<memix::ViewActivation> rolled_back =
      fixture.views.rollback("missing-profile");

  REQUIRE_FALSE(rolled_back.has_value());
  REQUIRE(rolled_back.error().kind == memix::ErrorKind::NotFound);
  REQUIRE_FALSE(path_exists(missing_profile));
}
