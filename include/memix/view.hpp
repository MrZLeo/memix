#pragma once

#include "memix/store.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memix {

struct ViewSelector {
  std::filesystem::path agent_path;
  std::optional<std::string> scope_label;
  bool required = true;
};

struct ViewExpression {
  std::string name = "file-tree";
  std::optional<std::string> agent_ref;
  std::optional<std::string> agent_generation;
  std::vector<ViewSelector> selectors;
  std::string operator_version = "file-tree-v1";
};

struct ViewArtifact {
  std::string derivation_id;
  std::string expression_id;
  std::string agent_generation;
  std::vector<std::string> input_blob_ids;
  std::filesystem::path manifest_path;
  std::filesystem::path tree_path;
  bool inserted = false;
};

class ViewService {
public:
  explicit ViewService(Store store) : store_(std::move(store)) {}

  [[nodiscard]] const Store &store() const noexcept { return store_; }

  [[nodiscard]] std::expected<ViewArtifact, StoreError>
  build_file_tree(const ViewExpression &expression) const;

  [[nodiscard]] std::expected<ViewArtifact, StoreError>
  resolve_file_tree(std::string_view derivation_prefix) const;

  [[nodiscard]] std::expected<std::vector<ViewArtifact>, StoreError>
  list_file_trees() const;

private:
  Store store_;
};

} // namespace memix
