#pragma once

#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memix {

enum class ViewTargetKind : std::uint8_t { FileTree, ContextPack };
enum class ViewVersionKind : std::uint8_t { Ref, Generation };
enum class ActivationMode : std::uint8_t { Link, Copy };
enum class ViewInputKind : std::uint8_t { Context, Environment };

namespace detail {

struct ViewTargetKindToken {
  ViewTargetKind value;
  std::string_view token;
};

struct ViewVersionKindToken {
  ViewVersionKind value;
  std::string_view token;
};

struct ActivationModeToken {
  ActivationMode value;
  std::string_view token;
};

struct ViewInputKindToken {
  ViewInputKind value;
  std::string_view token;
};

inline constexpr std::array kViewTargetKindTokens{
    ViewTargetKindToken{
        .value = ViewTargetKind::FileTree,
        .token = "file_tree",
    },
    ViewTargetKindToken{
        .value = ViewTargetKind::ContextPack,
        .token = "context_pack",
    },
};

inline constexpr std::array kViewVersionKindTokens{
    ViewVersionKindToken{
        .value = ViewVersionKind::Ref,
        .token = "ref",
    },
    ViewVersionKindToken{
        .value = ViewVersionKind::Generation,
        .token = "generation",
    },
};

inline constexpr std::array kActivationModeTokens{
    ActivationModeToken{
        .value = ActivationMode::Link,
        .token = "link",
    },
    ActivationModeToken{
        .value = ActivationMode::Copy,
        .token = "copy",
    },
};

inline constexpr std::array kViewInputKindTokens{
    ViewInputKindToken{
        .value = ViewInputKind::Context,
        .token = "context",
    },
    ViewInputKindToken{
        .value = ViewInputKind::Environment,
        .token = "environment",
    },
};

} // namespace detail

[[nodiscard]] constexpr std::string_view
to_string(ViewTargetKind target) noexcept {
  return detail::token_for(detail::kViewTargetKindTokens, target);
}

[[nodiscard]] constexpr std::optional<ViewTargetKind>
parse_view_target_kind(std::string_view value) noexcept {
  return detail::value_for_token<ViewTargetKind>(detail::kViewTargetKindTokens,
                                                 value);
}

[[nodiscard]] constexpr std::string_view
to_string(ViewVersionKind kind) noexcept {
  return detail::token_for(detail::kViewVersionKindTokens, kind);
}

[[nodiscard]] constexpr std::string_view
to_string(ActivationMode mode) noexcept {
  return detail::token_for(detail::kActivationModeTokens, mode);
}

[[nodiscard]] constexpr std::optional<ActivationMode>
parse_activation_mode(std::string_view value) noexcept {
  return detail::value_for_token<ActivationMode>(detail::kActivationModeTokens,
                                                 value);
}

[[nodiscard]] constexpr std::string_view
to_string(ViewInputKind kind) noexcept {
  return detail::token_for(detail::kViewInputKindTokens, kind);
}

[[nodiscard]] constexpr std::optional<ViewInputKind>
parse_view_input_kind(std::string_view value) noexcept {
  return detail::value_for_token<ViewInputKind>(detail::kViewInputKindTokens,
                                                value);
}

struct AuthoredViewInput {
  ViewInputKind kind = ViewInputKind::Context;
  std::string locator;
  std::string environment_id;
  std::string output_role = "file_tree";
  fs::path mount;
};

struct AuthoredViewExpression {
  std::string schema = "memix.view.expr.v1";
  std::string profile;
  std::vector<std::string> sources;
  ViewTargetKind target = ViewTargetKind::FileTree;
  std::vector<AuthoredViewInput> inputs;
};

struct ViewVersionSelector {
  ViewVersionKind kind = ViewVersionKind::Ref;
  std::string value = "latest";
};

struct ViewIrInput {
  std::string locator;
  MemoryObjectType type = MemoryObjectType::Rules;
  fs::path path;
  ViewVersionSelector version;
  bool recursive = false;
  bool required = true;
  ViewInputKind kind = ViewInputKind::Context;
  std::string environment_id;
  std::string output_role = "file_tree";
  fs::path mount;
};

struct ViewIrTarget {
  ViewTargetKind kind = ViewTargetKind::FileTree;
  std::string format;
};

struct ViewIr {
  std::string schema = "memix.view.ir.v1";
  std::vector<ViewIrInput> inputs;
  ViewIrTarget target;
};

struct ResolvedViewFile {
  std::string locator;
  MemoryObjectType type = MemoryObjectType::Rules;
  fs::path source_path;
  std::string source_generation;
  fs::path output_path;
  StoredObject blob;
};

struct ResolvedViewInputResolution {
  std::string locator;
  std::string source_generation;
  ViewVersionSelector version;
};

struct ViewDerivationOutput {
  std::string role;
  std::string artifact;
  fs::path path;
};

struct ViewEnvironmentClosure {
  std::vector<std::string> derivations;
  std::vector<std::string> artifacts;
  std::vector<std::string> raw_objects;
  std::vector<std::string> environments;
};

struct ResolvedViewEnvironmentInput {
  std::string environment_id;
  std::string derivation_id;
  std::string artifact_id;
  std::string output_role = "file_tree";
  fs::path mount;
  ViewEnvironmentClosure closure;
};

struct ViewDerivation {
  std::string id;
  std::string ir_hash;
  ViewTargetKind target = ViewTargetKind::FileTree;
  std::vector<ResolvedViewFile> resolved_files;
  std::vector<ViewDerivationOutput> outputs;
  fs::path manifest_path;
  fs::path tree_path;
  bool inserted = false;
  std::vector<ResolvedViewEnvironmentInput> environment_inputs;
  // Absent only for legacy manifests. New manifests contain one entry for
  // every context input, including inputs that resolved no files.
  std::optional<std::vector<ResolvedViewInputResolution>> input_resolutions;
  // Frozen runtime instructions, included in the derivation identity.
  // Absent only for legacy manifests; never filled from mutable quest metadata.
  std::optional<std::string> runtime_quest;
};

struct ViewEnvironmentBinding {
  fs::path mount;
  std::string derivation_id;
  std::string output_role;
};

struct ViewEnvironment {
  std::string id;
  fs::path manifest_path;
  std::vector<ViewEnvironmentBinding> bindings;
  ViewEnvironmentClosure closure;
  ViewDerivation derivation;
  bool inserted = false;
};

struct ViewCatalogOptions {
  fs::path path;
  std::optional<std::size_t> depth;
};

struct ViewCatalogEntry {
  fs::path path;
  bool directory = false;
  std::size_t file_count = 0;
  std::uintmax_t total_size_bytes = 0;
  std::string blob_id;
};

struct ViewEnvironmentInfo {
  std::string id;
  std::string derivation_id;
  std::size_t file_count = 0;
  std::uintmax_t total_size_bytes = 0;
  std::size_t upstream_environment_count = 0;
};

struct QuestInfo {
  std::string ir_hash;
  std::string intent;
  std::string name;
  std::string created_at;
  std::size_t resolution_count = 0;
  std::string last_resolved_at;
  std::string latest_derivation_id;
  std::string latest_environment_id;
  std::size_t read_count = 0;
  std::string last_read_at;
  std::size_t activation_count = 0;
  std::string last_activated_at;
};

struct QuestEnvironmentReads {
  std::string environment_id;
  std::size_t read_count = 0;
};

struct QuestProfileActivations {
  std::string profile;
  std::size_t activation_count = 0;
};

struct QuestStats {
  std::string ir_hash;
  std::size_t resolution_count = 0;
  std::string last_resolved_at;
  std::size_t activation_count = 0;
  std::string first_activated_at;
  std::string last_activated_at;
  std::vector<QuestProfileActivations> activations_by_profile;
  std::size_t read_count = 0;
  std::string first_read_at;
  std::string last_read_at;
  std::size_t cat_count = 0;
  std::size_t tree_count = 0;
  std::vector<QuestEnvironmentReads> reads_by_environment;
};

struct QuestDefinition {
  std::string ir_hash;
  fs::path quest_dir;
  bool existed = false;
};

struct QuestFreshnessInput {
  std::string locator;
  std::string recorded_generation;
  // Empty for pinned inputs and when a followed ref no longer exists. Use
  // `pinned` and `fresh` to distinguish those cases.
  std::optional<std::string> current_generation;
  bool pinned = false;
  bool fresh = true;
};

struct QuestFreshness {
  std::string ir_hash;
  std::string derivation_id;
  std::string resolved_at;
  // Contains context inputs only. Composed environment inputs are omitted:
  // their resolved environment IDs are content-pinned and cannot drift. For
  // legacy derivation manifests without per-input resolutions, freshness falls
  // back to resolved-file locator matching and conservatively marks an input
  // not represented there as not fresh.
  std::vector<QuestFreshnessInput> inputs;
  bool stale = false;
};

struct QuestDefinitionOptions {
  std::optional<std::string> intent;
  std::optional<std::string> name;
};

struct BuildOptions {
  std::string intent;
  std::string name;
};

// Profiles are the only mutable layer: a named sequence of generations with
// a switchable `current` pointer, Nix-profile style.

struct ActivationOptions {
  std::string profile;
  fs::path into;
  ActivationMode mode = ActivationMode::Link;
  bool force = false;
};

struct ViewGenerationInfo {
  std::uint64_t number = 0;
  std::string environment_id;
  std::string derivation_id;
  std::string activated_at;
  ActivationMode mode = ActivationMode::Link;
  bool current = false;
};

struct ViewActivation {
  std::string profile;
  std::uint64_t generation = 0;
  std::string environment_id;
  std::string derivation_id;
  fs::path workspace_path;
  bool switched = false;
  bool materialized = false;
};

struct ViewActivationStatus {
  bool active = false;
  ActivationMode mode = ActivationMode::Link;
  std::string profile;
  std::uint64_t generation = 0;
  std::string environment_id;
  std::string derivation_id;
  bool stale = false;
  fs::path workspace_path;
};

struct ViewDeactivation {
  std::string profile;
  fs::path workspace_path;
  bool wiped_profile = false;
};

// Default profile names isolate workspaces: `ws-<sha256(path)[:16]>`.
// Pass a canonical path (see fs::weakly_canonical) so the same workspace
// always maps to the same profile.
[[nodiscard]] std::string default_profile_name(const fs::path &workspace_dir);

// Validates a user-provided profile name (`[a-z0-9][a-z0-9._-]*`, no `ws-`
// prefix, which is reserved for default workspace profiles).
[[nodiscard]] ViewResult<std::string>
validated_profile_name(std::string_view profile);

class ViewService {
public:
  explicit ViewService(Store store) : store_(std::move(store)) {}

  [[nodiscard]] const Store &store() const noexcept { return store_; }

  [[nodiscard]] ViewResult<AuthoredViewExpression>
  parse_expression(std::string_view expression_or_sources) const;

  [[nodiscard]] ViewResult<AuthoredViewExpression> expression_from_sources(
      std::vector<std::string> sources,
      ViewTargetKind target = ViewTargetKind::FileTree) const;

  [[nodiscard]] ViewResult<AuthoredViewExpression> expression_from_inputs(
      std::vector<AuthoredViewInput> inputs,
      ViewTargetKind target = ViewTargetKind::FileTree) const;

  [[nodiscard]] ViewResult<ViewIr>
  lower_to_ir(const AuthoredViewExpression &expression) const;

  [[nodiscard]] ViewResult<ViewDerivation> build_ir(const ViewIr &ir) const;

  [[nodiscard]] ViewResult<ViewEnvironment>
  build(const AuthoredViewExpression &expression) const;

  [[nodiscard]] ViewResult<ViewEnvironment>
  build(const AuthoredViewExpression &expression,
        const BuildOptions &options) const;

  [[nodiscard]] ViewResult<QuestDefinition>
  define_quest(const AuthoredViewExpression &expression,
               std::string_view intent, std::string_view name) const;

  [[nodiscard]] ViewResult<QuestDefinition>
  define_quest(const AuthoredViewExpression &expression,
               const QuestDefinitionOptions &options) const;

  [[nodiscard]] ViewResult<std::vector<QuestInfo>> list_quests() const;

  [[nodiscard]] ViewResult<std::vector<QuestInfo>>
  find_quests(std::vector<std::string> terms,
              std::optional<fs::path> covers = std::nullopt) const;

  [[nodiscard]] ViewResult<ViewEnvironment>
  resolve_quest(std::string_view quest_prefix) const;

  [[nodiscard]] ViewResult<QuestFreshness>
  quest_freshness(std::string_view quest_prefix) const;

  [[nodiscard]] ViewResult<QuestStats>
  quest_stats(std::string_view quest_prefix) const;

  [[nodiscard]] ViewResult<ViewEnvironment>
  resolve_environment(std::string_view environment_prefix) const;

  [[nodiscard]] ViewResult<std::vector<ViewEnvironmentInfo>>
  list_environments() const;

  [[nodiscard]] ViewResult<std::vector<ViewCatalogEntry>>
  environment_tree(std::string_view environment_prefix,
                   const ViewCatalogOptions &options = {}) const;

  [[nodiscard]] ViewResult<ResolvedViewFile>
  read_environment_file(std::string_view environment_prefix,
                        const fs::path &path) const;

  [[nodiscard]] ViewResult<ViewActivation>
  activate(std::string_view environment_prefix,
           const ActivationOptions &options) const;

  [[nodiscard]] ViewResult<ViewActivation>
  rollback(std::string_view profile,
           std::optional<std::uint64_t> generation = std::nullopt) const;

  [[nodiscard]] ViewResult<std::vector<ViewGenerationInfo>>
  generations(std::string_view profile) const;

  [[nodiscard]] ViewResult<ViewActivationStatus>
  activation_status(const fs::path &into) const;

  [[nodiscard]] ViewResult<ViewDeactivation>
  deactivate(const fs::path &into, bool wipe_profile = false) const;

private:
  [[nodiscard]] ViewResult<ViewDerivation>
  build_ir(const ViewIr &ir, const BuildOptions &options) const;

  void
  record_activation(const ViewEnvironment &environment,
                    const ViewActivation &activation, ActivationMode mode,
                    std::optional<std::string_view> via = std::nullopt) const;

  Store store_;
};

[[nodiscard]] ViewResult<std::string> view_ir_json(const ViewIr &ir);
[[nodiscard]] ViewResult<std::string>
view_derivation_json(const ViewDerivation &derivation);
[[nodiscard]] ViewResult<std::string>
view_environment_json(const ViewEnvironment &environment);

} // namespace memix
