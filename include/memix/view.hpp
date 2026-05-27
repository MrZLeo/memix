#pragma once

#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memix {

enum class ViewTargetKind : std::uint8_t { FileTree, ContextPack };
std::string_view to_string(ViewTargetKind target) noexcept;
std::optional<ViewTargetKind>
parse_view_target_kind(std::string_view value) noexcept;

enum class ViewVersionKind : std::uint8_t { Ref, Generation };
std::string_view to_string(ViewVersionKind kind) noexcept;

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
  MemoryObjectType type = MemoryObjectType::Agents;
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
  MemoryObjectType type = MemoryObjectType::Agents;
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

private:
  Store store_;
};

[[nodiscard]] ViewResult<std::string> view_ir_json(const ViewIr &ir);
[[nodiscard]] ViewResult<std::string>
view_derivation_json(const ViewDerivation &derivation);
[[nodiscard]] ViewResult<std::string>
view_environment_json(const ViewEnvironment &environment);

} // namespace memix
