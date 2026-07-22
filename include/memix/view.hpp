#pragma once

#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"

#include <array>
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

struct AuthoredViewExpression {
  std::string schema = "memix.view.expr.v1";
  std::string profile;
  std::vector<std::string> sources;
  ViewTargetKind target = ViewTargetKind::FileTree;
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

struct ViewDerivationOutput {
  std::string role;
  std::string artifact;
  fs::path path;
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
};

struct ViewEnvironmentBinding {
  fs::path mount;
  std::string derivation_id;
  std::string output_role;
};

struct ViewEnvironmentClosure {
  std::vector<std::string> derivations;
  std::vector<std::string> artifacts;
  std::vector<std::string> raw_objects;
};

struct ViewEnvironment {
  std::string id;
  fs::path manifest_path;
  std::vector<ViewEnvironmentBinding> bindings;
  ViewEnvironmentClosure closure;
  ViewDerivation derivation;
  bool inserted = false;
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

  [[nodiscard]] ViewResult<ViewIr>
  lower_to_ir(const AuthoredViewExpression &expression) const;

  [[nodiscard]] ViewResult<ViewDerivation> build_ir(const ViewIr &ir) const;

  [[nodiscard]] ViewResult<ViewEnvironment>
  build(const AuthoredViewExpression &expression) const;

  [[nodiscard]] ViewResult<ViewEnvironment>
  resolve_environment(std::string_view environment_prefix) const;

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
  Store store_;
};

[[nodiscard]] ViewResult<std::string> view_ir_json(const ViewIr &ir);
[[nodiscard]] ViewResult<std::string>
view_derivation_json(const ViewDerivation &derivation);
[[nodiscard]] ViewResult<std::string>
view_environment_json(const ViewEnvironment &environment);

} // namespace memix
