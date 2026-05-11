#include "memix/view.hpp"

#include "memix/store.hpp"

#include "common.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <simdjson.h>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kViewsDir = "views";
constexpr std::string_view kExpressionsDir = "expressions";
constexpr std::string_view kDerivationsDir = "derivations";
constexpr std::string_view kManifestFilename = "manifest.json";
constexpr std::string_view kTreeDir = "tree";
constexpr std::string_view kDefaultAgentRef = "agents/latest";
constexpr std::string_view kJsonSuffix = ".json";

template <typename Value>
using ViewResult = memix::detail::Result<Value>;

using ViewVoid = memix::detail::VoidResult;

namespace expected_fs = memix::detail::expected_fs;

using memix::detail::apply_read_only_directory_permissions;
using memix::detail::apply_read_only_file_permissions;
using memix::detail::apply_writable_directory_permissions;
using memix::detail::filesystem_error_message;
using memix::detail::is_lower_hex_prefix;
using memix::detail::is_lower_hex_sha256;
using memix::detail::read_text_file;
using memix::detail::sha256_text;
using memix::detail::write_text_file;

struct ExpressionSelectorJson {
  std::string agent_path;
  std::optional<std::string> scope_label;
  bool required = true;
};

struct ManifestInputJson {
  std::string agent_path;
  std::optional<std::string> scope_label;
  bool required = true;
  bool selected = false;
  std::optional<std::string> blob_id;
};

struct ResolvedInput {
  memix::ViewSelector selector;
  bool selected = false;
  std::optional<memix::StoredObject> blob;
};

struct ManifestJsonRequest {
  std::string_view derivation_id;
  std::string_view expression_id;
  std::reference_wrapper<const memix::ViewExpression> expression;
  std::string_view agent_generation;
  std::reference_wrapper<const std::vector<ResolvedInput>> inputs;
};

struct ExistingDerivationCheck {
  fs::path derivation_dir;
  std::string_view expected_manifest;
  std::reference_wrapper<const std::vector<ResolvedInput>> inputs;
};

struct DerivationWriteRequest {
  fs::path derivations_dir;
  fs::path derivation_dir;
  std::string_view derivation_id;
  std::string_view manifest_json;
  std::reference_wrapper<const std::vector<ResolvedInput>> inputs;
};

struct ManifestReadRequest {
  fs::path manifest_path;
  fs::path derivation_dir;
};

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ExpressionSelectorJson &selector) {
  builder.start_object();
  builder.template append_key_value<"agent_path">(selector.agent_path);
  builder.append_comma();
  builder.template append_key_value<"scope_label">(selector.scope_label);
  builder.append_comma();
  builder.template append_key_value<"required">(selector.required);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ManifestInputJson &input) {
  builder.start_object();
  builder.template append_key_value<"agent_path">(input.agent_path);
  builder.append_comma();
  builder.template append_key_value<"scope_label">(input.scope_label);
  builder.append_comma();
  builder.template append_key_value<"required">(input.required);
  builder.append_comma();
  builder.template append_key_value<"selected">(input.selected);
  builder.append_comma();
  builder.template append_key_value<"blob_id">(input.blob_id);
  builder.end_object();
}

std::unexpected<memix::StoreError> view_error(std::string message) {
  return memix::detail::unexpected_error(std::move(message));
}

std::string simdjson_error_text(simdjson::error_code error) {
  return simdjson::error_message(error);
}

ViewVoid validate_sha256_field(std::string_view value,
                               std::string_view field_name) {
  if (!is_lower_hex_sha256(value)) {
    return view_error(std::format("{} is not a lowercase sha256 digest `{}`",
                                  field_name, value));
  }
  return {};
}

ViewVoid validate_derivation_prefix(std::string_view derivation_prefix) {
  if (!is_lower_hex_prefix(derivation_prefix)) {
    return view_error(std::format(
        "view derivation prefix must be 1 to 64 lowercase hex characters `{}`",
        derivation_prefix));
  }
  return {};
}

ViewResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder) {
  return memix::detail::json_from_builder(
      builder, memix::detail::JsonBuilderMessages{
                   .invalid_utf8_message = "view JSON contains invalid UTF-8",
                   .build_error_prefix = "failed to build view JSON",
               });
}

ViewVoid ensure_directory(const fs::path &path) {
  const ViewResult<bool> created = expected_fs::create_directories(path);
  if (!created) {
    return std::unexpected(created.error());
  }
  return {};
}

fs::path views_dir_for(const fs::path &root) {
  return root / std::string(kViewsDir);
}

fs::path expressions_dir_for(const fs::path &root) {
  return views_dir_for(root) / std::string(kExpressionsDir);
}

fs::path derivations_dir_for(const fs::path &root) {
  return views_dir_for(root) / std::string(kDerivationsDir);
}

fs::path expression_path_for(const fs::path &root,
                             std::string_view expression_id) {
  return expressions_dir_for(root)
       / std::format("{}{}", expression_id, kJsonSuffix);
}

fs::path derivation_dir_for(const fs::path &root,
                            std::string_view derivation_id) {
  return derivations_dir_for(root) / std::string(derivation_id);
}

fs::path tree_dir_for(const fs::path &derivation_dir) {
  return derivation_dir / std::string(kTreeDir);
}

ViewResult<fs::path> unique_temp_dir(const fs::path &parent,
                                     std::string_view prefix) {
  return memix::detail::unique_temp_dir(
      parent,
      memix::detail::TempDirOptions{
          .prefix = prefix,
          .failure_message = "failed to allocate a temporary view directory",
      });
}

std::vector<ExpressionSelectorJson>
expression_selector_json(const std::vector<memix::ViewSelector> &selectors) {
  return selectors
       | std::views::transform([](const memix::ViewSelector &selector) {
           return ExpressionSelectorJson{
               .agent_path = selector.agent_path.generic_string(),
               .scope_label = selector.scope_label,
               .required = selector.required,
           };
         })
       | std::ranges::to<std::vector>();
}

std::vector<ManifestInputJson>
manifest_input_json(const std::vector<ResolvedInput> &inputs) {
  return inputs
       | std::views::transform([](const ResolvedInput &input) {
           return ManifestInputJson{
               .agent_path = input.selector.agent_path.generic_string(),
               .scope_label = input.selector.scope_label,
               .required = input.selector.required,
               .selected = input.selected,
               .blob_id = input.blob.transform(
                   [](const memix::StoredObject &blob) { return blob.id; }),
           };
         })
       | std::ranges::to<std::vector>();
}

ViewResult<std::string>
expression_json_for(const memix::ViewExpression &expression,
                    std::string_view resolved_agent_generation) {
  simdjson::builder::string_builder builder;
  const std::vector<ExpressionSelectorJson> selectors =
      expression_selector_json(expression.selectors);
  builder.start_object();
  builder.append_key_value<"schema_version">(1);
  builder.append_comma();
  builder.append_key_value<"object_version">(1);
  builder.append_comma();
  builder.append_key_value<"kind">("view_expression");
  builder.append_comma();
  builder.append_key_value<"view_kind">("file_tree");
  builder.append_comma();
  builder.append_key_value<"name">(expression.name);
  builder.append_comma();
  builder.append_key_value<"operator_version">(expression.operator_version);
  builder.append_comma();
  builder.append_key_value<"agent_generation">(resolved_agent_generation);
  builder.append_comma();
  builder.append_key_value<"selectors">(selectors);
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<std::string>
derivation_basis_json_for(std::string_view expression_id,
                          const memix::ViewExpression &expression,
                          std::string_view resolved_agent_generation,
                          const std::vector<ResolvedInput> &inputs) {
  simdjson::builder::string_builder builder;
  const std::vector<ManifestInputJson> manifest_inputs =
      manifest_input_json(inputs);
  builder.start_object();
  builder.append_key_value<"schema_version">(1);
  builder.append_comma();
  builder.append_key_value<"object_version">(1);
  builder.append_comma();
  builder.append_key_value<"kind">("file_tree_derivation");
  builder.append_comma();
  builder.append_key_value<"expression_id">(expression_id);
  builder.append_comma();
  builder.append_key_value<"operator_version">(expression.operator_version);
  builder.append_comma();
  builder.append_key_value<"agent_generation">(resolved_agent_generation);
  builder.append_comma();
  builder.append_key_value<"inputs">(manifest_inputs);
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<std::string> manifest_json_for(ManifestJsonRequest request) {
  simdjson::builder::string_builder builder;
  const std::vector<ManifestInputJson> manifest_inputs =
      manifest_input_json(request.inputs.get());
  builder.start_object();
  builder.append_key_value<"schema_version">(1);
  builder.append_comma();
  builder.append_key_value<"object_version">(1);
  builder.append_comma();
  builder.append_key_value<"kind">("file_tree_view");
  builder.append_comma();
  builder.append_key_value<"derivation_id">(request.derivation_id);
  builder.append_comma();
  builder.append_key_value<"expression_id">(request.expression_id);
  builder.append_comma();
  builder.append_key_value<"name">(request.expression.get().name);
  builder.append_comma();
  builder.append_key_value<"operator_version">(
      request.expression.get().operator_version);
  builder.append_comma();
  builder.append_key_value<"agent_generation">(request.agent_generation);
  builder.append_comma();
  builder.append_key_value<"inputs">(manifest_inputs);
  builder.append_comma();
  builder.append_key_value<"tree">(kTreeDir);
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<std::string>
resolve_agent_generation(const memix::Store &store,
                         const memix::ViewExpression &expression) {
  if (expression.agent_ref.has_value()
      && expression.agent_generation.has_value()) {
    return view_error("view expression must set only one of agent_ref or "
                      "agent_generation");
  }

  if (expression.agent_generation.has_value()) {
    const std::expected<memix::ContextGeneration, memix::StoreError>
        generation = store.read_generation(memix::MemoryObjectType::Agents,
                                           *expression.agent_generation);
    if (!generation) {
      return std::unexpected(generation.error());
    }
    return generation->id;
  }

  const std::string ref =
      expression.agent_ref.value_or(std::string(kDefaultAgentRef));
  if (ref.empty()) {
    return view_error("view expression agent_ref must not be empty");
  }
  const std::expected<std::optional<std::string>, memix::StoreError> value =
      store.read_ref(ref);
  if (!value) {
    return std::unexpected(value.error());
  }
  if (!value->has_value()) {
    return view_error(std::format("agent ref `{}` does not exist", ref));
  }
  return **value;
}

bool is_missing_agent_path_error(const memix::StoreError &error) {
  return error.message.starts_with("agents path does not exist")
      || error.message.starts_with("agent path does not exist");
}

ResolvedInput selected_input_from_agent_file(const memix::StoreFile &file) {
  return ResolvedInput{
      .selector =
          memix::ViewSelector{
              .agent_path = file.context_path,
              .scope_label = std::nullopt,
              .required = true,
          },
      .selected = true,
      .blob = file.blob,
  };
}

ViewResult<std::vector<ResolvedInput>>
resolve_all_agent_inputs(const memix::Store &store,
                         std::string_view resolved_agent_generation) {
  std::expected<std::vector<memix::StoreFile>, memix::StoreError> files =
      store.list_files(memix::MemoryObjectType::Agents,
                       resolved_agent_generation);
  if (!files) {
    return std::unexpected(files.error());
  }

  return *files | std::views::transform([](const memix::StoreFile &file) {
    return selected_input_from_agent_file(file);
  }) | std::ranges::to<std::vector>();
}

ViewResult<std::vector<ResolvedInput>>
resolve_selected_inputs(const memix::Store &store,
                        const memix::ViewExpression &expression,
                        std::string_view resolved_agent_generation) {
  std::vector<ResolvedInput> inputs;
  inputs.reserve(expression.selectors.size());
  for (const memix::ViewSelector &selector : expression.selectors) {
    std::expected<memix::StoredObject, memix::StoreError> blob =
        store.read_file(memix::MemoryObjectType::Agents, selector.agent_path,
                        resolved_agent_generation);
    if (!blob) {
      if (!selector.required && is_missing_agent_path_error(blob.error())) {
        inputs.push_back(ResolvedInput{
            .selector = selector,
            .selected = false,
            .blob = std::nullopt,
        });
        continue;
      }
      return std::unexpected(blob.error());
    }
    inputs.push_back(ResolvedInput{
        .selector = selector,
        .selected = true,
        .blob = std::move(*blob),
    });
  }
  return inputs;
}

ViewResult<std::vector<ResolvedInput>>
resolve_inputs(const memix::Store &store,
               const memix::ViewExpression &expression,
               std::string_view resolved_agent_generation) {
  if (expression.selectors.empty()) {
    return resolve_all_agent_inputs(store, resolved_agent_generation);
  }
  return resolve_selected_inputs(store, expression, resolved_agent_generation);
}

ViewVoid validate_expression(const memix::ViewExpression &expression) {
  if (expression.name.empty()) {
    return view_error("view expression name must not be empty");
  }
  if (expression.operator_version.empty()) {
    return view_error("view expression operator_version must not be empty");
  }

  std::vector<std::string> selector_paths;
  selector_paths.reserve(expression.selectors.size());
  for (const memix::ViewSelector &selector : expression.selectors) {
    const std::string path = selector.agent_path.generic_string();
    if (path.empty()) {
      return view_error("view selector agent_path must not be empty");
    }
    if (std::ranges::find(selector_paths, path) != selector_paths.end()) {
      return view_error(std::format(
          "view expression selects duplicate agent path `{}`", path));
    }
    selector_paths.push_back(path);
  }
  return {};
}

ViewVoid write_immutable_text_file_if_absent(const fs::path &path,
                                             std::string_view contents) {
  const ViewResult<bool> path_exists = expected_fs::exists(path);
  if (!path_exists) {
    return std::unexpected(path_exists.error());
  }
  if (*path_exists) {
    const ViewResult<std::string> existing_contents = read_text_file(path);
    if (!existing_contents) {
      return std::unexpected(existing_contents.error());
    }
    if (*existing_contents != contents) {
      return view_error(
          std::format("immutable view file differs `{}`", path.string()));
    }
    return {};
  }

  const ViewVoid parent_created = ensure_directory(path.parent_path());
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }

  const ViewResult<fs::path> temp_dir =
      unique_temp_dir(path.parent_path(), path.filename().string());
  if (!temp_dir) {
    return std::unexpected(temp_dir.error());
  }
  const ViewResult<bool> created_temp_dir =
      expected_fs::create_directory(*temp_dir);
  if (!created_temp_dir) {
    return std::unexpected(created_temp_dir.error());
  }
  if (!*created_temp_dir) {
    return view_error(std::format(
        "failed to create temporary view directory `{}`", temp_dir->string()));
  }

  const auto cleanup = [&temp_dir]() {
    const ViewResult<std::uintmax_t> removed =
        expected_fs::remove_all(*temp_dir);
    (void)removed;
  };

  const fs::path temp_file = *temp_dir / "object";
  const ViewVoid written = write_text_file(temp_file, contents);
  if (!written) {
    cleanup();
    return std::unexpected(written.error());
  }
  const ViewVoid temp_file_read_only =
      apply_read_only_file_permissions(temp_file);
  if (!temp_file_read_only) {
    cleanup();
    return std::unexpected(temp_file_read_only.error());
  }

  const ViewVoid renamed = expected_fs::rename(temp_file, path);
  if (!renamed) {
    cleanup();
    const ViewResult<bool> raced_existing = expected_fs::exists(path);
    if (!raced_existing) {
      return std::unexpected(raced_existing.error());
    }
    if (*raced_existing) {
      const ViewResult<std::string> existing_contents = read_text_file(path);
      if (!existing_contents) {
        return std::unexpected(existing_contents.error());
      }
      if (*existing_contents == contents) {
        return {};
      }
      return view_error(
          std::format("immutable view file differs `{}`", path.string()));
    }
    return std::unexpected(renamed.error());
  }

  cleanup();
  return {};
}

ViewVoid write_expression_object(const memix::Store &store,
                                 std::string_view expression_id,
                                 std::string_view expression_json) {
  const ViewVoid directory_created =
      ensure_directory(expressions_dir_for(store.root()));
  if (!directory_created) {
    return std::unexpected(directory_created.error());
  }
  return write_immutable_text_file_if_absent(
      expression_path_for(store.root(), expression_id), expression_json);
}

fs::path link_path_for(const fs::path &tree_dir, const ResolvedInput &input) {
  return tree_dir / input.selector.agent_path;
}

ViewResult<fs::path> symlink_target_for(const fs::path &link_path,
                                        const memix::StoredObject &blob) {
  const fs::path target = blob.object_dir / blob.content_path;
  return expected_fs::relative(target, link_path.parent_path());
}

ViewVoid create_input_symlink(const fs::path &tree_dir,
                              const ResolvedInput &input) {
  if (!input.selected || !input.blob.has_value()) {
    return {};
  }

  const fs::path link_path = link_path_for(tree_dir, input);
  const ViewVoid parent_created = ensure_directory(link_path.parent_path());
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }
  const ViewResult<fs::path> target =
      symlink_target_for(link_path, *input.blob);
  if (!target) {
    return std::unexpected(target.error());
  }
  return expected_fs::create_symlink(*target, link_path);
}

ViewVoid create_tree_symlinks(const fs::path &tree_dir,
                              const std::vector<ResolvedInput> &inputs) {
  const ViewVoid tree_created = ensure_directory(tree_dir);
  if (!tree_created) {
    return std::unexpected(tree_created.error());
  }
  for (const ResolvedInput &input : inputs) {
    const ViewVoid linked = create_input_symlink(tree_dir, input);
    if (!linked) {
      return std::unexpected(linked.error());
    }
  }
  return {};
}

ViewVoid apply_read_only_tree_permissions(const fs::path &tree_dir) {
  std::vector<fs::path> directories;
  directories.push_back(tree_dir);

  std::error_code iterator_error;
  const fs::recursive_directory_iterator end;
  for (fs::recursive_directory_iterator iterator{tree_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    const bool entry_is_directory = entry.is_directory(entry_error);
    if (entry_error) {
      return view_error(filesystem_error_message("failed to inspect",
                                                 entry_error, entry.path()));
    }
    if (entry_is_directory) {
      directories.push_back(entry.path());
    }
  }
  if (iterator_error) {
    return view_error(filesystem_error_message("failed to list directory",
                                               iterator_error, tree_dir));
  }

  for (const fs::path &directory : std::views::reverse(directories)) {
    const ViewVoid permissions_applied =
        apply_read_only_directory_permissions(directory);
    if (!permissions_applied) {
      return std::unexpected(permissions_applied.error());
    }
  }
  return {};
}

ViewVoid apply_writable_tree_permissions(const fs::path &tree_dir) {
  const ViewResult<bool> exists = expected_fs::exists(tree_dir);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return {};
  }

  std::vector<fs::path> directories;
  directories.push_back(tree_dir);

  std::error_code iterator_error;
  const fs::recursive_directory_iterator end;
  for (fs::recursive_directory_iterator iterator{tree_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    const bool entry_is_directory = entry.is_directory(entry_error);
    if (entry_error) {
      return view_error(filesystem_error_message("failed to inspect",
                                                 entry_error, entry.path()));
    }
    if (entry_is_directory) {
      directories.push_back(entry.path());
    }
  }
  if (iterator_error) {
    return view_error(filesystem_error_message("failed to list directory",
                                               iterator_error, tree_dir));
  }

  for (const fs::path &directory : directories) {
    const ViewVoid permissions_applied =
        apply_writable_directory_permissions(directory);
    if (!permissions_applied) {
      return std::unexpected(permissions_applied.error());
    }
  }
  return {};
}

memix::ViewArtifact artifact_for(std::string derivation_id,
                                 std::string expression_id,
                                 std::string agent_generation,
                                 const std::vector<ResolvedInput> &inputs,
                                 const fs::path &derivation_dir,
                                 bool inserted) {
  std::vector<std::string> input_blob_ids;
  for (const ResolvedInput &input : inputs) {
    if (input.selected && input.blob.has_value()) {
      input_blob_ids.push_back(input.blob->id);
    }
  }
  return memix::ViewArtifact{
      .derivation_id = std::move(derivation_id),
      .expression_id = std::move(expression_id),
      .agent_generation = std::move(agent_generation),
      .input_blob_ids = std::move(input_blob_ids),
      .manifest_path = derivation_dir / std::string(kManifestFilename),
      .tree_path = tree_dir_for(derivation_dir),
      .inserted = inserted,
  };
}

ViewResult<bool> derivation_exists(const fs::path &derivation_dir) {
  const ViewResult<bool> exists = expected_fs::exists(derivation_dir);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return false;
  }
  const ViewResult<bool> is_directory =
      expected_fs::is_directory(derivation_dir);
  if (!is_directory) {
    return std::unexpected(is_directory.error());
  }
  if (!*is_directory) {
    return view_error(
        std::format("view derivation path is not a directory `{}`",
                    derivation_dir.string()));
  }
  return true;
}

ViewVoid verify_input_symlink(const fs::path &tree_dir,
                              const ResolvedInput &input) {
  if (!input.selected || !input.blob.has_value()) {
    return {};
  }
  const fs::path link_path = link_path_for(tree_dir, input);
  const ViewResult<bool> is_symlink = expected_fs::is_symlink(link_path);
  if (!is_symlink) {
    return std::unexpected(is_symlink.error());
  }
  if (!*is_symlink) {
    return view_error(std::format("view tree path is not a symlink `{}`",
                                  link_path.string()));
  }

  const ViewResult<fs::path> expected_target =
      symlink_target_for(link_path, *input.blob);
  if (!expected_target) {
    return std::unexpected(expected_target.error());
  }
  const ViewResult<fs::path> actual_target =
      expected_fs::read_symlink(link_path);
  if (!actual_target) {
    return std::unexpected(actual_target.error());
  }
  if (*actual_target != *expected_target) {
    return view_error(std::format("view tree symlink target differs `{}`",
                                  link_path.string()));
  }
  return {};
}

ViewVoid verify_existing_derivation(const ExistingDerivationCheck &check) {
  const ViewResult<std::string> manifest =
      read_text_file(check.derivation_dir / std::string(kManifestFilename));
  if (!manifest) {
    return std::unexpected(manifest.error());
  }
  if (*manifest != check.expected_manifest) {
    return view_error(std::format("view derivation manifest differs `{}`",
                                  check.derivation_dir.string()));
  }

  const fs::path tree_dir = tree_dir_for(check.derivation_dir);
  const ViewResult<bool> is_directory = expected_fs::is_directory(tree_dir);
  if (!is_directory) {
    return std::unexpected(is_directory.error());
  }
  if (!*is_directory) {
    return view_error(
        std::format("view tree directory is missing `{}`", tree_dir.string()));
  }
  for (const ResolvedInput &input : check.inputs.get()) {
    const ViewVoid verified = verify_input_symlink(tree_dir, input);
    if (!verified) {
      return std::unexpected(verified.error());
    }
  }
  return {};
}

ViewResult<bool>
write_derivation_atomically(const DerivationWriteRequest &request) {
  const ViewResult<fs::path> temp_dir =
      unique_temp_dir(request.derivations_dir, request.derivation_id);
  if (!temp_dir) {
    return std::unexpected(temp_dir.error());
  }
  const ViewResult<bool> created = expected_fs::create_directory(*temp_dir);
  if (!created) {
    return std::unexpected(created.error());
  }
  if (!*created) {
    return view_error(std::format(
        "failed to create temporary view directory `{}`", temp_dir->string()));
  }

  const auto cleanup = [&temp_dir]() {
    const ViewVoid made_writable =
        apply_writable_tree_permissions(tree_dir_for(*temp_dir));
    (void)made_writable;
    const ViewResult<std::uintmax_t> removed =
        expected_fs::remove_all(*temp_dir);
    (void)removed;
  };

  ViewVoid written = write_text_file(*temp_dir / std::string(kManifestFilename),
                                     request.manifest_json);
  if (!written) {
    cleanup();
    return std::unexpected(written.error());
  }
  written = apply_read_only_file_permissions(*temp_dir
                                             / std::string(kManifestFilename));
  if (!written) {
    cleanup();
    return std::unexpected(written.error());
  }

  written = create_tree_symlinks(tree_dir_for(*temp_dir), request.inputs.get());
  if (!written) {
    cleanup();
    return std::unexpected(written.error());
  }
  written = apply_read_only_tree_permissions(tree_dir_for(*temp_dir));
  if (!written) {
    cleanup();
    return std::unexpected(written.error());
  }

  ViewVoid renamed = expected_fs::rename(*temp_dir, request.derivation_dir);
  if (!renamed) {
    const ViewResult<bool> exists = derivation_exists(request.derivation_dir);
    cleanup();
    if (exists && *exists) {
      return false;
    }
    return std::unexpected(renamed.error());
  }
  return true;
}

template <typename Value>
ViewResult<Value> require_field_as(simdjson::dom::object fields,
                                   std::string_view key,
                                   std::string_view expected_type) {
  Value parsed{};
  const simdjson::error_code error = fields.at_key(key).get(parsed);
  if (error != simdjson::SUCCESS) {
    return view_error(std::format("manifest field `{}` must be {}: {}", key,
                                  expected_type, simdjson_error_text(error)));
  }
  return parsed;
}

ViewResult<std::string> require_string(simdjson::dom::object fields,
                                       std::string_view key) {
  return require_field_as<std::string_view>(fields, key, "a string")
      .transform([](std::string_view value) { return std::string(value); });
}

ViewResult<std::string> require_sha256_string(simdjson::dom::object fields,
                                              std::string_view key) {
  ViewResult<std::string> value = require_string(fields, key);
  if (!value) {
    return std::unexpected(value.error());
  }
  const std::string field_name = std::format("manifest field `{}`", key);
  const ViewVoid valid = validate_sha256_field(*value, field_name);
  if (!valid) {
    return std::unexpected(valid.error());
  }
  return value;
}

ViewResult<int> require_int(simdjson::dom::object fields,
                            std::string_view key) {
  ViewResult<std::int64_t> parsed =
      require_field_as<std::int64_t>(fields, key, "an integer");
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  if (*parsed < std::numeric_limits<int>::min()
      || *parsed > std::numeric_limits<int>::max()) {
    return view_error(
        std::format("manifest field `{}` is outside the supported range", key));
  }
  return static_cast<int>(*parsed);
}

ViewResult<bool> require_bool(simdjson::dom::object fields,
                              std::string_view key) {
  return require_field_as<bool>(fields, key, "a boolean");
}

ViewResult<simdjson::dom::array> require_array(simdjson::dom::object fields,
                                               std::string_view key) {
  return require_field_as<simdjson::dom::array>(fields, key, "an array");
}

ViewResult<std::optional<std::string>>
optional_string_field(simdjson::dom::object fields, std::string_view key) {
  simdjson::dom::element element;
  const simdjson::error_code error = fields.at_key(key).get(element);
  if (error != simdjson::SUCCESS) {
    return view_error(std::format("manifest field `{}` must exist: {}", key,
                                  simdjson_error_text(error)));
  }
  if (element.is_null()) {
    return std::optional<std::string>{};
  }
  std::string_view value;
  const simdjson::error_code string_error = element.get(value);
  if (string_error != simdjson::SUCCESS) {
    return view_error(std::format("manifest field `{}` must be null or a "
                                  "string: {}",
                                  key, simdjson_error_text(string_error)));
  }
  return std::optional<std::string>{std::string(value)};
}

ViewResult<simdjson::dom::object>
read_manifest_object(const ManifestReadRequest &request,
                     simdjson::dom::parser &parser,
                     simdjson::padded_string &padded_json) {
  const ViewResult<std::string> manifest_text =
      read_text_file(request.manifest_path);
  if (!manifest_text) {
    return std::unexpected(manifest_text.error());
  }
  padded_json = simdjson::padded_string(*manifest_text);
  simdjson::dom::element document;
  simdjson::error_code error = parser.parse(padded_json).get(document);
  if (error != simdjson::SUCCESS) {
    return view_error(std::format("invalid view manifest `{}`: {}",
                                  request.manifest_path.string(),
                                  simdjson_error_text(error)));
  }

  simdjson::dom::object fields;
  error = document.get(fields);
  if (error != simdjson::SUCCESS) {
    return view_error(std::format("view manifest must be an object `{}`: {}",
                                  request.manifest_path.string(),
                                  simdjson_error_text(error)));
  }
  return fields;
}

ViewResult<std::optional<std::string>>
read_manifest_kind(simdjson::dom::object fields) {
  ViewResult<std::string> kind = require_string(fields, "kind");
  if (!kind) {
    return std::unexpected(kind.error());
  }
  if (*kind != "file_tree_view") {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{std::move(*kind)};
}

ViewVoid validate_manifest_header(simdjson::dom::object fields) {
  const ViewResult<int> schema_version = require_int(fields, "schema_version");
  if (!schema_version) {
    return std::unexpected(schema_version.error());
  }
  const ViewResult<int> object_version = require_int(fields, "object_version");
  if (!object_version) {
    return std::unexpected(object_version.error());
  }
  if (*schema_version != 1 || *object_version != 1) {
    return view_error(
        "view manifest schema or object version is not supported");
  }
  return {};
}

ViewResult<std::string>
read_manifest_derivation_id(simdjson::dom::object fields,
                            const ManifestReadRequest &request) {
  ViewResult<std::string> derivation_id =
      require_sha256_string(fields, "derivation_id");
  if (!derivation_id) {
    return std::unexpected(derivation_id.error());
  }
  if (request.derivation_dir.filename() != fs::path(*derivation_id)) {
    return view_error(std::format("view manifest id does not match directory "
                                  "`{}`",
                                  request.manifest_path.string()));
  }
  return derivation_id;
}

ViewResult<std::vector<std::string>>
read_manifest_input_blob_ids(simdjson::dom::object fields) {
  ViewResult<simdjson::dom::array> inputs = require_array(fields, "inputs");
  if (!inputs) {
    return std::unexpected(inputs.error());
  }

  std::vector<std::string> input_blob_ids;
  for (const simdjson::dom::element input_element : *inputs) {
    simdjson::dom::object input_fields;
    const simdjson::error_code input_error = input_element.get(input_fields);
    if (input_error != simdjson::SUCCESS) {
      return view_error(std::format("view manifest input must be an object: {}",
                                    simdjson_error_text(input_error)));
    }
    const ViewResult<bool> selected = require_bool(input_fields, "selected");
    if (!selected) {
      return std::unexpected(selected.error());
    }
    const ViewResult<std::optional<std::string>> blob_id =
        optional_string_field(input_fields, "blob_id");
    if (!blob_id) {
      return std::unexpected(blob_id.error());
    }
    if (*selected) {
      if (!blob_id->has_value()) {
        return view_error("selected view manifest input must include blob_id");
      }
      const ViewVoid blob_id_valid =
          validate_sha256_field(**blob_id, "manifest input `blob_id`");
      if (!blob_id_valid) {
        return std::unexpected(blob_id_valid.error());
      }
      input_blob_ids.push_back(**blob_id);
    } else if (blob_id->has_value()) {
      return view_error(
          "unselected view manifest input must not include blob_id");
    }
  }
  return input_blob_ids;
}

ViewVoid validate_manifest_tree_field(simdjson::dom::object fields) {
  ViewResult<std::string> tree = require_string(fields, "tree");
  if (!tree) {
    return std::unexpected(tree.error());
  }
  if (*tree != kTreeDir) {
    return view_error(std::format("view manifest tree must be `{}`", kTreeDir));
  }
  return {};
}

ViewVoid validate_manifest_tree_exists(const ManifestReadRequest &request) {
  const fs::path tree_path = tree_dir_for(request.derivation_dir);
  const ViewResult<bool> tree_exists = expected_fs::is_directory(tree_path);
  if (!tree_exists) {
    return std::unexpected(tree_exists.error());
  }
  if (!*tree_exists) {
    return view_error(
        std::format("view tree directory is missing `{}`", tree_path.string()));
  }
  return {};
}

ViewResult<std::optional<memix::ViewArtifact>>
artifact_from_manifest(const ManifestReadRequest &request) {
  simdjson::dom::parser parser;
  simdjson::padded_string padded_json;
  ViewResult<simdjson::dom::object> fields =
      read_manifest_object(request, parser, padded_json);
  if (!fields) {
    return std::unexpected(fields.error());
  }

  ViewResult<std::optional<std::string>> kind = read_manifest_kind(*fields);
  if (!kind) {
    return std::unexpected(kind.error());
  }
  if (!kind->has_value()) {
    return std::optional<memix::ViewArtifact>{};
  }

  const ViewVoid header_valid = validate_manifest_header(*fields);
  if (!header_valid) {
    return std::unexpected(header_valid.error());
  }
  ViewResult<std::string> derivation_id =
      read_manifest_derivation_id(*fields, request);
  if (!derivation_id) {
    return std::unexpected(derivation_id.error());
  }
  ViewResult<std::string> expression_id =
      require_sha256_string(*fields, "expression_id");
  if (!expression_id) {
    return std::unexpected(expression_id.error());
  }
  ViewResult<std::string> agent_generation =
      require_sha256_string(*fields, "agent_generation");
  if (!agent_generation) {
    return std::unexpected(agent_generation.error());
  }
  ViewResult<std::vector<std::string>> input_blob_ids =
      read_manifest_input_blob_ids(*fields);
  if (!input_blob_ids) {
    return std::unexpected(input_blob_ids.error());
  }
  const ViewVoid tree_field_valid = validate_manifest_tree_field(*fields);
  if (!tree_field_valid) {
    return std::unexpected(tree_field_valid.error());
  }
  const ViewVoid tree_exists = validate_manifest_tree_exists(request);
  if (!tree_exists) {
    return std::unexpected(tree_exists.error());
  }

  return std::optional<memix::ViewArtifact>{memix::ViewArtifact{
      .derivation_id = std::move(*derivation_id),
      .expression_id = std::move(*expression_id),
      .agent_generation = std::move(*agent_generation),
      .input_blob_ids = std::move(*input_blob_ids),
      .manifest_path = request.manifest_path,
      .tree_path = tree_dir_for(request.derivation_dir),
      .inserted = false,
  }};
}

ViewResult<std::string>
resolve_file_tree_derivation_id(const memix::ViewService &service,
                                std::string_view derivation_prefix) {
  const ViewVoid prefix_valid = validate_derivation_prefix(derivation_prefix);
  if (!prefix_valid) {
    return std::unexpected(prefix_valid.error());
  }

  const std::expected<std::vector<memix::ViewArtifact>, memix::StoreError>
      artifacts = service.list_file_trees();
  if (!artifacts) {
    return std::unexpected(artifacts.error());
  }

  std::vector<std::string> matches;
  for (const memix::ViewArtifact &artifact : *artifacts) {
    if (artifact.derivation_id.starts_with(derivation_prefix)) {
      matches.push_back(artifact.derivation_id);
    }
  }
  if (matches.empty()) {
    return view_error(std::format("no view derivation matches prefix `{}`",
                                  derivation_prefix));
  }
  if (matches.size() > 1) {
    return view_error(
        std::format("view derivation prefix `{}` is ambiguous ({} matches)",
                    derivation_prefix, matches.size()));
  }
  return matches.front();
}

} // namespace

namespace memix {

std::expected<ViewArtifact, StoreError>
ViewService::build_file_tree(const ViewExpression &expression) const {
  const ViewVoid expression_valid = validate_expression(expression);
  if (!expression_valid) {
    return std::unexpected(expression_valid.error());
  }

  ViewResult<std::string> agent_generation =
      resolve_agent_generation(store_, expression);
  if (!agent_generation) {
    return std::unexpected(agent_generation.error());
  }
  ViewResult<std::vector<ResolvedInput>> inputs =
      resolve_inputs(store_, expression, *agent_generation);
  if (!inputs) {
    return std::unexpected(inputs.error());
  }

  ViewResult<std::string> expression_json =
      expression_json_for(expression, *agent_generation);
  if (!expression_json) {
    return std::unexpected(expression_json.error());
  }
  ViewResult<std::string> expression_id = sha256_text(*expression_json);
  if (!expression_id) {
    return std::unexpected(expression_id.error());
  }
  const ViewVoid expression_written =
      write_expression_object(store_, *expression_id, *expression_json);
  if (!expression_written) {
    return std::unexpected(expression_written.error());
  }

  ViewResult<std::string> derivation_basis = derivation_basis_json_for(
      *expression_id, expression, *agent_generation, *inputs);
  if (!derivation_basis) {
    return std::unexpected(derivation_basis.error());
  }
  ViewResult<std::string> derivation_id = sha256_text(*derivation_basis);
  if (!derivation_id) {
    return std::unexpected(derivation_id.error());
  }
  ViewResult<std::string> manifest_json = manifest_json_for({
      .derivation_id = *derivation_id,
      .expression_id = *expression_id,
      .expression = expression,
      .agent_generation = *agent_generation,
      .inputs = *inputs,
  });
  if (!manifest_json) {
    return std::unexpected(manifest_json.error());
  }

  const fs::path derivations_dir = derivations_dir_for(store_.root());
  const ViewVoid derivations_created = ensure_directory(derivations_dir);
  if (!derivations_created) {
    return std::unexpected(derivations_created.error());
  }
  const fs::path derivation_dir =
      derivation_dir_for(store_.root(), *derivation_id);
  ViewResult<bool> exists = derivation_exists(derivation_dir);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (*exists) {
    const ViewVoid verified = verify_existing_derivation({
        .derivation_dir = derivation_dir,
        .expected_manifest = *manifest_json,
        .inputs = *inputs,
    });
    if (!verified) {
      return std::unexpected(verified.error());
    }
    return artifact_for(std::move(*derivation_id), std::move(*expression_id),
                        std::move(*agent_generation), *inputs, derivation_dir,
                        false);
  }

  const ViewResult<bool> inserted = write_derivation_atomically({
      .derivations_dir = derivations_dir,
      .derivation_dir = derivation_dir,
      .derivation_id = *derivation_id,
      .manifest_json = *manifest_json,
      .inputs = *inputs,
  });
  if (!inserted) {
    return std::unexpected(inserted.error());
  }

  const ViewVoid verified = verify_existing_derivation({
      .derivation_dir = derivation_dir,
      .expected_manifest = *manifest_json,
      .inputs = *inputs,
  });
  if (!verified) {
    return std::unexpected(verified.error());
  }
  return artifact_for(std::move(*derivation_id), std::move(*expression_id),
                      std::move(*agent_generation), *inputs, derivation_dir,
                      *inserted);
}

std::expected<ViewArtifact, StoreError>
ViewService::resolve_file_tree(std::string_view derivation_prefix) const {
  ViewResult<std::string> derivation_id =
      resolve_file_tree_derivation_id(*this, derivation_prefix);
  if (!derivation_id) {
    return std::unexpected(derivation_id.error());
  }

  ViewResult<std::optional<ViewArtifact>> artifact = artifact_from_manifest({
      .manifest_path = derivation_dir_for(store_.root(), *derivation_id)
                     / std::string(kManifestFilename),
      .derivation_dir = derivation_dir_for(store_.root(), *derivation_id),
  });
  if (!artifact) {
    return std::unexpected(artifact.error());
  }
  if (!artifact->has_value()) {
    return view_error(
        std::format("view derivation is not a file tree `{}`", *derivation_id));
  }
  return std::move(**artifact);
}

std::expected<std::vector<ViewArtifact>, StoreError>
ViewService::list_file_trees() const {
  const fs::path derivations_dir = derivations_dir_for(store_.root());
  const ViewResult<bool> exists = expected_fs::exists(derivations_dir);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return std::vector<ViewArtifact>{};
  }
  const ViewResult<bool> is_directory =
      expected_fs::is_directory(derivations_dir);
  if (!is_directory) {
    return std::unexpected(is_directory.error());
  }
  if (!*is_directory) {
    return view_error(std::format("views derivations path is not a directory "
                                  "`{}`",
                                  derivations_dir.string()));
  }

  std::vector<ViewArtifact> artifacts;
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{derivations_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    const bool entry_is_directory = entry.is_directory(entry_error);
    if (entry_error) {
      return view_error(filesystem_error_message("failed to inspect",
                                                 entry_error, entry.path()));
    }
    if (!entry_is_directory) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (!is_lower_hex_sha256(name)) {
      continue;
    }
    ViewResult<std::optional<ViewArtifact>> artifact = artifact_from_manifest({
        .manifest_path = entry.path() / std::string(kManifestFilename),
        .derivation_dir = entry.path(),
    });
    if (!artifact) {
      return std::unexpected(artifact.error());
    }
    if (artifact->has_value()) {
      artifacts.push_back(std::move(**artifact));
    }
  }
  if (iterator_error) {
    return view_error(filesystem_error_message(
        "failed to list directory", iterator_error, derivations_dir));
  }

  std::ranges::sort(artifacts, {}, &ViewArtifact::derivation_id);
  return artifacts;
}

} // namespace memix
