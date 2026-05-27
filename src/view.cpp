#include "memix/view.hpp"

#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"

#include "detail/error.hpp"
#include "detail/hash.hpp"
#include "detail/json.hpp"
#include "detail/scope_exit.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <simdjson.h>

namespace fs = memix::fs;

namespace {

constexpr std::string_view kExpressionSchema = "memix.view.expr.v1";
constexpr std::string_view kIrSchema = "memix.view.ir.v1";
constexpr std::string_view kDerivationSchema = "memix.view.derivation.v1";
constexpr std::string_view kEnvironmentSchema = "memix.view.environment.v1";
constexpr std::string_view kViewsDir = "views";
constexpr std::string_view kDerivationsDir = "derivations";
constexpr std::string_view kEnvironmentsDir = "environments";
constexpr std::string_view kManifestFilename = "manifest.json";
constexpr std::string_view kTreeDir = "tree";
constexpr std::string_view kJsonSuffix = ".json";
constexpr std::string_view kLatestVersion = "latest";
constexpr std::string_view kFileTreeOperatorVersion = "file-tree-v1";

struct ViewTargetKindToken {
  memix::ViewTargetKind value;
  std::string_view token;
};

struct ViewVersionKindToken {
  memix::ViewVersionKind value;
  std::string_view token;
};

constexpr std::array kViewTargetKindTokens{
    ViewTargetKindToken{
        .value = memix::ViewTargetKind::FileTree,
        .token = "file_tree",
    },
    ViewTargetKindToken{
        .value = memix::ViewTargetKind::ContextPack,
        .token = "context_pack",
    },
};

constexpr std::array kViewVersionKindTokens{
    ViewVersionKindToken{
        .value = memix::ViewVersionKind::Ref,
        .token = "ref",
    },
    ViewVersionKindToken{
        .value = memix::ViewVersionKind::Generation,
        .token = "generation",
    },
};

template <typename Value>
using ViewResult = memix::ViewResult<Value>;

using ViewVoid = memix::ViewVoid;

struct VersionJson {
  std::string kind;
  std::string value;
};

struct SelectorJson {
  bool recursive = false;
  bool required = true;
};

struct IrInputJson {
  std::string locator;
  std::string type;
  std::string path;
  VersionJson version;
  SelectorJson selector;
};

struct TargetAttrsJson {
  std::optional<std::string> format;
};

struct IrTargetJson {
  std::string op;
  TargetAttrsJson attrs;
};

struct ResolvedInputJson {
  std::string locator;
  std::string type;
  std::string path;
  std::string output_path;
  std::string generation;
  std::string blob;
};

struct PassJson {
  std::string name;
  std::string version;
};

struct OutputJson {
  std::string role;
  std::string artifact;
  std::string path;
};

struct BindingJson {
  std::string mount;
  std::string derivation;
  std::string output;
};

struct ClosureJson {
  std::reference_wrapper<const std::vector<std::string>> derivations;
  std::reference_wrapper<const std::vector<std::string>> artifacts;
  std::reference_wrapper<const std::vector<std::string>> raw_objects;
};

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const VersionJson &version) {
  builder.start_object();
  builder.template append_key_value<"kind">(version.kind);
  builder.append_comma();
  builder.template append_key_value<"value">(version.value);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const SelectorJson &selector) {
  builder.start_object();
  builder.template append_key_value<"recursive">(selector.recursive);
  builder.append_comma();
  builder.template append_key_value<"required">(selector.required);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const IrInputJson &input) {
  builder.start_object();
  builder.template append_key_value<"locator">(input.locator);
  builder.append_comma();
  builder.template append_key_value<"type">(input.type);
  builder.append_comma();
  builder.template append_key_value<"path">(input.path);
  builder.append_comma();
  builder.template append_key_value<"version">(input.version);
  builder.append_comma();
  builder.template append_key_value<"selector">(input.selector);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const TargetAttrsJson &attrs) {
  builder.start_object();
  builder.template append_key_value<"format">(attrs.format);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const IrTargetJson &target) {
  builder.start_object();
  builder.template append_key_value<"op">(target.op);
  builder.append_comma();
  builder.template append_key_value<"attrs">(target.attrs);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ResolvedInputJson &input) {
  builder.start_object();
  builder.template append_key_value<"locator">(input.locator);
  builder.append_comma();
  builder.template append_key_value<"type">(input.type);
  builder.append_comma();
  builder.template append_key_value<"path">(input.path);
  builder.append_comma();
  builder.template append_key_value<"output_path">(input.output_path);
  builder.append_comma();
  builder.template append_key_value<"generation">(input.generation);
  builder.append_comma();
  builder.template append_key_value<"blob">(input.blob);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const PassJson &pass) {
  builder.start_object();
  builder.template append_key_value<"name">(pass.name);
  builder.append_comma();
  builder.template append_key_value<"version">(pass.version);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const OutputJson &output) {
  builder.start_object();
  builder.template append_key_value<"role">(output.role);
  builder.append_comma();
  builder.template append_key_value<"artifact">(output.artifact);
  builder.append_comma();
  builder.template append_key_value<"path">(output.path);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const BindingJson &binding) {
  builder.start_object();
  builder.template append_key_value<"mount">(binding.mount);
  builder.append_comma();
  builder.template append_key_value<"derivation">(binding.derivation);
  builder.append_comma();
  builder.template append_key_value<"output">(binding.output);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ClosureJson &closure) {
  builder.start_object();
  builder.template append_key_value<"derivations">(closure.derivations.get());
  builder.append_comma();
  builder.template append_key_value<"artifacts">(closure.artifacts.get());
  builder.append_comma();
  builder.template append_key_value<"raw_objects">(closure.raw_objects.get());
  builder.end_object();
}

std::unexpected<memix::ViewError> view_error(memix::ErrorKind kind,
                                             std::string message) {
  memix::ViewError error;
  error.message = std::move(message);
  error.kind = kind;
  return std::unexpected(std::move(error));
}

std::unexpected<memix::ViewError> view_error(std::string message) {
  return view_error(memix::ErrorKind::Unknown, std::move(message));
}

std::unexpected<memix::ViewError> view_error(memix::FsError error) {
  return std::unexpected(
      memix::detail::translate_error<memix::ViewError>(std::move(error)));
}

std::unexpected<memix::ViewError> view_error(memix::StoreError error) {
  return std::unexpected(
      memix::detail::translate_error<memix::ViewError>(std::move(error)));
}

std::unexpected<memix::ViewError> view_error(memix::detail::CommonError error) {
  return std::unexpected(
      memix::detail::translate_error<memix::ViewError>(std::move(error)));
}

std::string simdjson_error_text(simdjson::error_code error) {
  return simdjson::error_message(error);
}

ViewResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder) {
  memix::detail::CommonResult<std::string> json =
      memix::detail::json_from_builder(
          builder,
          memix::detail::JsonBuilderMessages{
              .invalid_utf8_message = "view JSON contains invalid UTF-8",
              .build_error_prefix = "failed to build view JSON",
          });
  if (!json) {
    return view_error(std::move(json.error()));
  }
  return std::move(*json);
}

std::string trim_copy(std::string_view text) {
  while (!text.empty()
         && (text.front() == ' '
             || text.front() == '\t'
             || text.front() == '\n'
             || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty()
         && (text.back() == ' '
             || text.back() == '\t'
             || text.back() == '\n'
             || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::vector<std::string> split_source_list(std::string_view text) {
  std::vector<std::string> sources;
  std::string current;
  for (const char character : text) {
    if (character == '\n'
        || character == '\r'
        || character == ','
        || character == ' '
        || character == '\t') {
      if (!current.empty()) {
        sources.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(character);
  }
  if (!current.empty()) {
    sources.push_back(std::move(current));
  }
  return sources;
}

template <typename Value>
ViewResult<Value> require_field_as(simdjson::dom::object fields,
                                   std::string_view key,
                                   std::string_view expected_type) {
  Value parsed{};
  const simdjson::error_code error = fields.at_key(key).get(parsed);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("expression field `{}` must be {}: {}", key,
                                  expected_type, simdjson_error_text(error)));
  }
  return parsed;
}

ViewResult<std::optional<std::string>>
optional_string(simdjson::dom::object fields, std::string_view key) {
  std::string_view value;
  const simdjson::error_code error = fields.at_key(key).get(value);
  if (error == simdjson::NO_SUCH_FIELD) {
    return std::optional<std::string>{};
  }
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("expression field `{}` must be a string: {}",
                                  key, simdjson_error_text(error)));
  }
  return std::optional<std::string>{std::string(value)};
}

ViewResult<simdjson::dom::array>
optional_sources_array(simdjson::dom::object fields, bool &has_sources) {
  simdjson::dom::array sources;
  const simdjson::error_code error = fields.at_key("sources").get(sources);
  if (error == simdjson::NO_SUCH_FIELD) {
    has_sources = false;
    return simdjson::dom::array{};
  }
  has_sources = true;
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("expression field `sources` must be an "
                                  "array: {}",
                                  simdjson_error_text(error)));
  }
  return sources;
}

bool valid_logical_component(std::string_view component) {
  return !component.empty()
      && component != "."
      && component != ".."
      && !component.contains('/')
      && !component.contains('\\');
}

struct ViewPathValidationRequest {
  std::string_view value;
  std::string_view locator;
};

ViewResult<fs::path> validate_view_path(ViewPathValidationRequest request) {
  if (request.value.empty()) {
    return view_error(
        memix::ErrorKind::InvalidArgument,
        std::format("view locator has an empty path `{}`", request.locator));
  }
  if (request.value.contains("//")) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("view locator path contains empty "
                                  "components `{}`",
                                  request.locator));
  }
  if (request.value.contains('\\')) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("view locator path must not contain "
                                  "backslashes `{}`",
                                  request.locator));
  }
  fs::path parsed{std::string(request.value)};
  if (parsed.is_absolute()) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("view locator path must be relative "
                                  "`{}`",
                                  request.locator));
  }
  for (const fs::path &component_path : parsed) {
    const std::string component = component_path.generic_string();
    if (!valid_logical_component(component)) {
      return view_error(memix::ErrorKind::InvalidArgument,
                        std::format("view locator path contains invalid "
                                    "component `{}` in `{}`",
                                    component, request.locator));
    }
  }
  return parsed;
}

memix::ViewVersionSelector parse_version_selector(std::string_view value) {
  std::string version =
      value.empty() ? std::string(kLatestVersion) : std::string(value);
  if (version.starts_with("sha256:")) {
    version = version.substr(std::string_view("sha256:").size());
  }
  if (memix::detail::is_lower_hex_sha256(version)) {
    return memix::ViewVersionSelector{
        .kind = memix::ViewVersionKind::Generation,
        .value = std::move(version),
    };
  }
  return memix::ViewVersionSelector{
      .kind = memix::ViewVersionKind::Ref,
      .value = std::move(version),
  };
}

std::string version_ref_name(memix::MemoryObjectType type,
                             std::string_view version) {
  if (version == kLatestVersion) {
    return std::format("{}/latest", memix::to_string(type));
  }
  constexpr std::string_view kGenerationPrefix = "generation/";
  if (version.starts_with(kGenerationPrefix)) {
    return std::format("{}/generations/{}", memix::to_string(type),
                       version.substr(kGenerationPrefix.size()));
  }
  constexpr std::string_view kRefPrefix = "ref/";
  if (version.starts_with(kRefPrefix)) {
    return std::format("{}/{}", memix::to_string(type),
                       version.substr(kRefPrefix.size()));
  }
  return std::format("{}/{}", memix::to_string(type), version);
}

std::string canonical_locator(memix::MemoryObjectType type,
                              const fs::path &path,
                              const memix::ViewVersionSelector &version,
                              bool recursive) {
  std::string output =
      std::format("{}/{}", memix::to_string(type), path.generic_string());
  if (recursive) {
    output.push_back('/');
  }
  output += std::format("@{}", version.value);
  return output;
}

ViewResult<memix::ViewIrInput> parse_locator(std::string_view locator) {
  const std::string source = trim_copy(locator);
  if (source.empty()) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      "view locator must not be empty");
  }

  const std::size_t version_separator = source.rfind('@');
  const std::string path_text = version_separator == std::string::npos
                                  ? source
                                  : source.substr(0, version_separator);
  const std::string version_text = version_separator == std::string::npos
                                     ? std::string(kLatestVersion)
                                     : source.substr(version_separator + 1);
  if (path_text.empty()) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("view locator path is empty `{}`", source));
  }

  const std::size_t type_separator = path_text.find('/');
  if (type_separator == std::string::npos || type_separator == 0) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("view locator must start with "
                                  "`<type>/` `{}`",
                                  source));
  }
  const std::string type_text = path_text.substr(0, type_separator);
  const std::optional<memix::MemoryObjectType> type =
      memix::parse_memory_object_type(type_text);
  if (!type.has_value()) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("view locator has unsupported type "
                                  "`{}` in `{}`",
                                  type_text, source));
  }

  std::string view_path_text = path_text.substr(type_separator + 1);
  const bool recursive = view_path_text.ends_with('/');
  if (recursive) {
    view_path_text.pop_back();
  }
  ViewResult<fs::path> view_path = validate_view_path({
      .value = view_path_text,
      .locator = source,
  });
  if (!view_path) {
    return std::unexpected(view_path.error());
  }
  memix::ViewVersionSelector version = parse_version_selector(version_text);
  return memix::ViewIrInput{
      .locator = canonical_locator(*type, *view_path, version, recursive),
      .type = *type,
      .path = std::move(*view_path),
      .version = std::move(version),
      .recursive = recursive,
      .required = true,
  };
}

ViewResult<memix::ViewTargetKind>
parse_required_target(std::string_view value) {
  const std::optional<memix::ViewTargetKind> target =
      memix::parse_view_target_kind(value);
  if (!target.has_value()) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("unsupported view target `{}`", value));
  }
  return *target;
}

ViewResult<memix::AuthoredViewExpression>
parse_expression_json(std::string_view text) {
  const simdjson::padded_string padded_json(text);
  simdjson::dom::parser parser;
  simdjson::dom::element document;
  simdjson::error_code error = parser.parse(padded_json).get(document);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("invalid view expression JSON: {}",
                                  simdjson_error_text(error)));
  }

  simdjson::dom::object fields;
  error = document.get(fields);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("view expression JSON must be an "
                                  "object: {}",
                                  simdjson_error_text(error)));
  }

  memix::AuthoredViewExpression expression;
  ViewResult<std::optional<std::string>> schema =
      optional_string(fields, "schema");
  if (!schema) {
    return std::unexpected(schema.error());
  }
  if (schema->has_value()) {
    if (**schema != kExpressionSchema) {
      return view_error(
          memix::ErrorKind::InvalidData,
          std::format("unsupported view expression schema `{}`", **schema));
    }
    expression.schema = **schema;
  }

  ViewResult<std::optional<std::string>> profile =
      optional_string(fields, "profile");
  if (!profile) {
    return std::unexpected(profile.error());
  }
  if (profile->has_value()) {
    expression.profile = **profile;
  }

  ViewResult<std::optional<std::string>> target =
      optional_string(fields, "target");
  if (!target) {
    return std::unexpected(target.error());
  }
  if (target->has_value()) {
    ViewResult<memix::ViewTargetKind> parsed_target =
        parse_required_target(**target);
    if (!parsed_target) {
      return std::unexpected(parsed_target.error());
    }
    expression.target = *parsed_target;
  }

  bool has_sources = false;
  ViewResult<simdjson::dom::array> sources =
      optional_sources_array(fields, has_sources);
  if (!sources) {
    return std::unexpected(sources.error());
  }
  if (has_sources) {
    for (const simdjson::dom::element source_element : *sources) {
      std::string_view source;
      error = source_element.get(source);
      if (error != simdjson::SUCCESS) {
        return view_error(memix::ErrorKind::InvalidData,
                          std::format("view expression sources must be "
                                      "strings: {}",
                                      simdjson_error_text(error)));
      }
      expression.sources.emplace_back(source);
    }
  }

  return expression;
}

VersionJson version_json_for(const memix::ViewVersionSelector &version) {
  return VersionJson{
      .kind = std::string(memix::to_string(version.kind)),
      .value = version.value,
  };
}

IrInputJson ir_input_json_for(const memix::ViewIrInput &input) {
  return IrInputJson{
      .locator = input.locator,
      .type = std::string(memix::to_string(input.type)),
      .path = input.path.generic_string(),
      .version = version_json_for(input.version),
      .selector =
          SelectorJson{
              .recursive = input.recursive,
              .required = input.required,
          },
  };
}

IrTargetJson ir_target_json_for(const memix::ViewIrTarget &target) {
  std::optional<std::string> format;
  if (!target.format.empty()) {
    format = target.format;
  }
  return IrTargetJson{
      .op = std::string(memix::to_string(target.kind)),
      .attrs = TargetAttrsJson{.format = std::move(format)},
  };
}

ViewResult<std::string>
ir_basis_json_for(std::string_view ir_hash,
                  const std::vector<ResolvedInputJson> &inputs,
                  memix::ViewTargetKind target) {
  const std::vector<PassJson> passes{
      PassJson{.name = "resolve-refs", .version = "v1"},
      PassJson{.name = "expand-dirs", .version = "v1"},
      PassJson{.name = "pack-file-tree", .version = "v1"},
  };

  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">("memix.view.derivation.basis.v1");
  builder.append_comma();
  builder.append_key_value<"ir_hash">(ir_hash);
  builder.append_comma();
  builder.append_key_value<"target">(memix::to_string(target));
  builder.append_comma();
  builder.append_key_value<"passes">(passes);
  builder.append_comma();
  builder.append_key_value<"resolved_inputs">(inputs);
  builder.end_object();
  return json_from_builder(builder);
}

std::vector<ResolvedInputJson>
resolved_input_json(const std::vector<memix::ResolvedViewFile> &files) {
  return files
       | std::views::transform([](const memix::ResolvedViewFile &file) {
           return ResolvedInputJson{
               .locator = file.locator,
               .type = std::string(memix::to_string(file.type)),
               .path = file.source_path.generic_string(),
               .output_path = file.output_path.generic_string(),
               .generation = file.source_generation,
               .blob = file.blob.id,
           };
         })
       | std::ranges::to<std::vector>();
}

std::vector<OutputJson>
output_json(const std::vector<memix::ViewDerivationOutput> &outputs) {
  return outputs
       | std::views::transform([](const memix::ViewDerivationOutput &out) {
           return OutputJson{
               .role = out.role,
               .artifact = out.artifact,
               .path = out.path.generic_string(),
           };
         })
       | std::ranges::to<std::vector>();
}

std::vector<BindingJson>
binding_json(const std::vector<memix::ViewEnvironmentBinding> &bindings) {
  return bindings
       | std::views::transform(
             [](const memix::ViewEnvironmentBinding &binding) {
               return BindingJson{
                   .mount = binding.mount.generic_string(),
                   .derivation = binding.derivation_id,
                   .output = binding.output_role,
               };
             })
       | std::ranges::to<std::vector>();
}

ViewResult<std::string> sha256_text(std::string_view text) {
  memix::detail::CommonResult<std::string> hash =
      memix::detail::sha256_text(text);
  if (!hash) {
    return view_error(std::move(hash.error()));
  }
  return std::move(*hash);
}

ViewVoid ensure_directory(const fs::path &path) {
  const memix::FsResult<bool> created = fs::create_directories(path);
  if (!created) {
    return view_error(created.error());
  }
  return {};
}

ViewResult<std::vector<fs::path>> collect_directories(const fs::path &root) {
  std::vector<fs::path> directories{root};
  std::error_code iterator_error;
  const fs::recursive_directory_iterator end;
  for (fs::recursive_directory_iterator iterator{root, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    if (!entry.is_directory(entry_error)) {
      if (entry_error) {
        return view_error(fs::make_fs_error("failed to inspect directory",
                                            entry_error, entry.path()));
      }
      continue;
    }
    directories.push_back(entry.path());
  }
  if (iterator_error) {
    return view_error(
        fs::make_fs_error("failed to list directory", iterator_error, root));
  }
  return directories;
}

ViewVoid apply_directory_tree_permissions(const fs::path &root, bool writable) {
  ViewResult<std::vector<fs::path>> directories = collect_directories(root);
  if (!directories) {
    return std::unexpected(directories.error());
  }
  if (!writable) {
    std::ranges::reverse(*directories);
  }
  for (const fs::path &directory : *directories) {
    const memix::FsVoid applied =
        writable ? fs::apply_writable_directory_permissions(directory)
                 : fs::apply_read_only_directory_permissions(directory);
    if (!applied) {
      return view_error(applied.error());
    }
  }
  return {};
}

fs::path views_dir_for(const fs::path &root) {
  return root / std::string(kViewsDir);
}

fs::path derivations_dir_for(const fs::path &root) {
  return views_dir_for(root) / std::string(kDerivationsDir);
}

fs::path environments_dir_for(const fs::path &root) {
  return views_dir_for(root) / std::string(kEnvironmentsDir);
}

fs::path derivation_dir_for(const fs::path &root,
                            std::string_view derivation_id) {
  return derivations_dir_for(root) / std::string(derivation_id);
}

fs::path environment_path_for(const fs::path &root,
                              std::string_view environment_id) {
  return environments_dir_for(root)
       / std::format("{}{}", environment_id, kJsonSuffix);
}

ViewResult<fs::path> unique_temp_dir(const fs::path &parent,
                                     std::string_view prefix) {
  memix::FsResult<fs::path> temp_dir = fs::unique_temp_dir(
      parent,
      fs::TempDirOptions{
          .prefix = prefix,
          .failure_message = "failed to allocate a temporary view directory",
      });
  if (!temp_dir) {
    return view_error(std::move(temp_dir.error()));
  }
  return std::move(*temp_dir);
}

struct ExistingTextFileValidation {
  fs::path path;
  std::string_view contents;
  std::string_view label;
};

ViewVoid
validate_existing_text_file(const ExistingTextFileValidation &request) {
  const memix::FsResult<std::string> existing =
      fs::read_text_file(request.path, request.label);
  if (!existing) {
    return view_error(existing.error());
  }
  if (*existing != request.contents) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("immutable view file differs `{}`", request.path.string()));
  }
  return {};
}

ViewResult<bool> write_text_file_if_absent(const fs::path &path,
                                           std::string_view contents) {
  const memix::FsResult<bool> exists = fs::exists(path);
  if (!exists) {
    return view_error(exists.error());
  }
  if (*exists) {
    const ViewVoid validated = validate_existing_text_file({
        .path = path,
        .contents = contents,
        .label = "view file",
    });
    if (!validated) {
      return std::unexpected(validated.error());
    }
    return false;
  }
  const ViewVoid parent_created = ensure_directory(path.parent_path());
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }

  ViewResult<fs::path> temp_dir =
      unique_temp_dir(path.parent_path(), path.filename().string());
  if (!temp_dir) {
    return std::unexpected(temp_dir.error());
  }
  const fs::path &temp_path = *temp_dir;
  const memix::FsResult<bool> created = fs::create_directory(temp_path);
  if (!created) {
    return view_error(created.error());
  }
  if (!*created) {
    return view_error(std::format("failed to create temporary view file "
                                  "directory `{}`",
                                  temp_path.string()));
  }
  const memix::scope_exit cleanup([temp_path] {
    memix::detail::consume_cleanup_result(fs::remove_all(temp_path));
  });

  const fs::path temp_file = temp_path / "object";
  const memix::FsVoid written = fs::write_text_file(temp_file, contents);
  if (!written) {
    return view_error(written.error());
  }
  const memix::FsVoid read_only =
      fs::apply_read_only_file_permissions(temp_file);
  if (!read_only) {
    return view_error(read_only.error());
  }

  const memix::FsVoid linked = fs::create_hard_link(temp_file, path);
  if (!linked) {
    const memix::FsResult<bool> raced_exists = fs::exists(path);
    if (!raced_exists) {
      return view_error(raced_exists.error());
    }
    if (*raced_exists) {
      const ViewVoid validated = validate_existing_text_file({
          .path = path,
          .contents = contents,
          .label = "view file",
      });
      if (!validated) {
        return std::unexpected(validated.error());
      }
      return false;
    }
    return view_error(linked.error());
  }

  return true;
}

ViewResult<std::string>
resolve_source_generation(const memix::Store &store,
                          const memix::ViewIrInput &input) {
  if (input.version.kind == memix::ViewVersionKind::Generation) {
    memix::StoreResult<memix::ContextGeneration> generation =
        store.read_generation(input.type, input.version.value);
    if (!generation) {
      return view_error(generation.error());
    }
    return generation->id;
  }

  const std::string ref_name =
      version_ref_name(input.type, input.version.value);
  memix::StoreResult<std::optional<std::string>> ref = store.read_ref(ref_name);
  if (!ref) {
    return view_error(ref.error());
  }
  if (!ref->has_value()) {
    return view_error(
        memix::ErrorKind::NotFound,
        std::format("view source ref `{}` does not exist", ref_name));
  }
  return **ref;
}

bool path_is_under(const fs::path &path, const fs::path &prefix) {
  const auto path_components = path | std::ranges::to<std::vector<fs::path>>();
  const auto prefix_components =
      prefix | std::ranges::to<std::vector<fs::path>>();
  if (prefix_components.size() >= path_components.size()) {
    return false;
  }
  return std::ranges::equal(prefix_components,
                            path_components
                                | std::views::take(prefix_components.size()));
}

fs::path output_path_for(memix::MemoryObjectType type, const fs::path &path) {
  return fs::path(std::string(memix::to_string(type))) / path;
}

ViewResult<std::vector<memix::ResolvedViewFile>>
resolve_input(const memix::Store &store, const memix::ViewIrInput &input) {
  ViewResult<std::string> generation = resolve_source_generation(store, input);
  if (!generation) {
    return std::unexpected(generation.error());
  }

  std::vector<memix::ResolvedViewFile> files;
  if (!input.recursive) {
    memix::StoreResult<memix::StoredObject> blob =
        store.read_file(input.type, input.path, *generation);
    if (!blob) {
      if (!input.required
          && blob.error().kind == memix::ErrorKind::ContextPathNotFound) {
        return files;
      }
      return view_error(blob.error());
    }
    files.push_back(memix::ResolvedViewFile{
        .locator = input.locator,
        .type = input.type,
        .source_path = input.path,
        .source_generation = *generation,
        .output_path = output_path_for(input.type, input.path),
        .blob = std::move(*blob),
    });
    return files;
  }

  memix::StoreResult<std::vector<memix::StoreFile>> source_files =
      store.list_files(input.type, *generation);
  if (!source_files) {
    return view_error(source_files.error());
  }
  for (memix::StoreFile &file : *source_files) {
    if (!path_is_under(file.context_path, input.path)) {
      continue;
    }
    files.push_back(memix::ResolvedViewFile{
        .locator = input.locator,
        .type = input.type,
        .source_path = file.context_path,
        .source_generation = *generation,
        .output_path = output_path_for(input.type, file.context_path),
        .blob = std::move(file.blob),
    });
  }
  if (files.empty() && input.required) {
    return view_error(memix::ErrorKind::ContextPathNotFound,
                      std::format("view source directory is empty or "
                                  "missing `{}`",
                                  input.locator));
  }
  return files;
}

ViewResult<std::vector<memix::ResolvedViewFile>>
resolve_inputs(const memix::Store &store, const memix::ViewIr &ir) {
  std::vector<memix::ResolvedViewFile> files;
  for (const memix::ViewIrInput &input : ir.inputs) {
    ViewResult<std::vector<memix::ResolvedViewFile>> input_files =
        resolve_input(store, input);
    if (!input_files) {
      return std::unexpected(input_files.error());
    }
    std::ranges::move(*input_files, std::back_inserter(files));
  }

  std::ranges::sort(files, [](const memix::ResolvedViewFile &lhs,
                              const memix::ResolvedViewFile &rhs) {
    const auto lhs_key =
        std::tuple{lhs.output_path.generic_string(), lhs.blob.id, lhs.locator};
    const auto rhs_key =
        std::tuple{rhs.output_path.generic_string(), rhs.blob.id, rhs.locator};
    return lhs_key < rhs_key;
  });

  std::vector<memix::ResolvedViewFile> unique_files;
  for (memix::ResolvedViewFile &file : files) {
    if (!unique_files.empty()
        && unique_files.back().output_path == file.output_path) {
      if (unique_files.back().blob.id != file.blob.id) {
        return view_error(memix::ErrorKind::Conflict,
                          std::format("view sources map different blobs "
                                      "to `{}`",
                                      file.output_path.generic_string()));
      }
      continue;
    }
    unique_files.push_back(std::move(file));
  }
  return unique_files;
}

std::vector<std::string>
unique_blob_ids(const std::vector<memix::ResolvedViewFile> &files) {
  std::vector<std::string> ids;
  ids.reserve(files.size());
  for (const memix::ResolvedViewFile &file : files) {
    ids.push_back(file.blob.id);
  }
  std::ranges::sort(ids);
  ids.erase(std::ranges::unique(ids).begin(), ids.end());
  return ids;
}

ViewResult<std::string> artifact_id_for(std::string_view derivation_id,
                                        std::string_view role) {
  return sha256_text(std::format("{}:{}", role, derivation_id));
}

ViewResult<std::vector<memix::ViewDerivationOutput>>
outputs_for_derivation(std::string_view derivation_id,
                       memix::ViewTargetKind target) {
  if (target != memix::ViewTargetKind::FileTree) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      "only file_tree target builds are supported in v1");
  }

  ViewResult<std::string> tree_artifact =
      artifact_id_for(derivation_id, "file_tree");
  if (!tree_artifact) {
    return std::unexpected(tree_artifact.error());
  }
  ViewResult<std::string> manifest_artifact =
      artifact_id_for(derivation_id, "provenance_manifest");
  if (!manifest_artifact) {
    return std::unexpected(manifest_artifact.error());
  }
  return std::vector<memix::ViewDerivationOutput>{
      memix::ViewDerivationOutput{
          .role = "file_tree",
          .artifact = *tree_artifact,
          .path = fs::path(kTreeDir),
      },
      memix::ViewDerivationOutput{
          .role = "provenance_manifest",
          .artifact = *manifest_artifact,
          .path = fs::path(kManifestFilename),
      },
  };
}

ViewVoid create_symlink_for_file(const fs::path &tree_dir,
                                 const memix::ResolvedViewFile &file) {
  const fs::path link_path = tree_dir / file.output_path;
  const ViewVoid parent_created = ensure_directory(link_path.parent_path());
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }
  const fs::path target = file.blob.object_dir / file.blob.content_path;
  memix::FsResult<fs::path> relative_target =
      fs::relative(target, link_path.parent_path());
  if (!relative_target) {
    return view_error(relative_target.error());
  }
  const memix::FsVoid symlink_created =
      fs::create_symlink(*relative_target, link_path);
  if (!symlink_created) {
    return view_error(symlink_created.error());
  }
  return {};
}

ViewVoid create_file_tree(const fs::path &tree_dir,
                          const std::vector<memix::ResolvedViewFile> &files) {
  const ViewVoid tree_created = ensure_directory(tree_dir);
  if (!tree_created) {
    return std::unexpected(tree_created.error());
  }
  for (const memix::ResolvedViewFile &file : files) {
    const ViewVoid linked = create_symlink_for_file(tree_dir, file);
    if (!linked) {
      return std::unexpected(linked.error());
    }
  }
  return {};
}

ViewVoid freeze_derivation_directory(const fs::path &derivation_dir) {
  const memix::FsVoid manifest_read_only = fs::apply_read_only_file_permissions(
      derivation_dir / std::string(kManifestFilename));
  if (!manifest_read_only) {
    return view_error(manifest_read_only.error());
  }
  return apply_directory_tree_permissions(derivation_dir, false);
}

ViewVoid thaw_derivation_directory(const fs::path &derivation_dir) {
  return apply_directory_tree_permissions(derivation_dir, true);
}

memix::ViewError view_error_from_fs(memix::FsError error) {
  return memix::detail::translate_error<memix::ViewError>(std::move(error));
}

ViewVoid fail_after_thawing_derivation(const fs::path &derivation_dir,
                                       memix::ViewError original_error) {
  const ViewVoid thawed = thaw_derivation_directory(derivation_dir);
  if (!thawed) {
    return view_error(std::format("{}; additionally failed to restore "
                                  "temporary derivation permissions: {}",
                                  original_error.message,
                                  thawed.error().message));
  }
  return std::unexpected(std::move(original_error));
}

ViewResult<bool> handle_derivation_rename_failure(
    const fs::path &temp_dir, const memix::ViewDerivation &derivation,
    std::string_view manifest_json, const memix::FsError &rename_error) {
  const memix::FsResult<bool> raced_exists =
      fs::exists(derivation.manifest_path.parent_path());
  if (!raced_exists) {
    const ViewVoid failed = fail_after_thawing_derivation(
        temp_dir, view_error_from_fs(raced_exists.error()));
    return std::unexpected(failed.error());
  }
  if (!*raced_exists) {
    const ViewVoid failed = fail_after_thawing_derivation(
        temp_dir, view_error_from_fs(rename_error));
    return std::unexpected(failed.error());
  }

  const ViewVoid validated = validate_existing_text_file({
      .path = derivation.manifest_path,
      .contents = manifest_json,
      .label = "view derivation manifest",
  });
  if (!validated) {
    const ViewVoid failed =
        fail_after_thawing_derivation(temp_dir, validated.error());
    return std::unexpected(failed.error());
  }
  const ViewVoid thawed = thaw_derivation_directory(temp_dir);
  if (!thawed) {
    return std::unexpected(thawed.error());
  }
  return false;
}

ViewResult<bool>
write_derivation_directory(const memix::Store &store,
                           const memix::ViewDerivation &derivation,
                           std::string_view manifest_json) {
  const fs::path derivations_dir = derivations_dir_for(store.root());
  const ViewVoid derivations_created = ensure_directory(derivations_dir);
  if (!derivations_created) {
    return std::unexpected(derivations_created.error());
  }
  const fs::path derivation_dir =
      derivation_dir_for(store.root(), derivation.id);
  const memix::FsResult<bool> exists = fs::exists(derivation_dir);
  if (!exists) {
    return view_error(exists.error());
  }
  if (*exists) {
    const ViewVoid validated = validate_existing_text_file({
        .path = derivation.manifest_path,
        .contents = manifest_json,
        .label = "view derivation manifest",
    });
    if (!validated) {
      return std::unexpected(validated.error());
    }
    return false;
  }

  ViewResult<fs::path> temp_dir =
      unique_temp_dir(derivations_dir, derivation.id);
  if (!temp_dir) {
    return std::unexpected(temp_dir.error());
  }
  const fs::path &temp_path = *temp_dir;
  const memix::FsResult<bool> created = fs::create_directory(temp_path);
  if (!created) {
    return view_error(created.error());
  }
  if (!*created) {
    return view_error(std::format("failed to create temporary view "
                                  "derivation `{}`",
                                  temp_path.string()));
  }
  memix::scope_exit cleanup([temp_path] {
    memix::detail::consume_cleanup_result(fs::remove_all(temp_path));
  });
  const memix::FsVoid written = fs::write_text_file(
      temp_path / std::string(kManifestFilename), manifest_json);
  if (!written) {
    return view_error(written.error());
  }
  const ViewVoid tree_created = create_file_tree(
      temp_path / std::string(kTreeDir), derivation.resolved_files);
  if (!tree_created) {
    return std::unexpected(tree_created.error());
  }
  const memix::FsVoid renamed = fs::rename(temp_path, derivation_dir);
  if (!renamed) {
    return handle_derivation_rename_failure(temp_path, derivation,
                                            manifest_json, renamed.error());
  }
  cleanup.release();
  const ViewVoid frozen = freeze_derivation_directory(derivation_dir);
  if (!frozen) {
    return std::unexpected(frozen.error());
  }
  return true;
}

ViewResult<std::string>
environment_basis_json_for(const memix::ViewDerivation &derivation) {
  const std::vector<BindingJson> bindings{
      BindingJson{
          .mount = "tree",
          .derivation = derivation.id,
          .output = "file_tree",
      },
  };
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">("memix.view.environment.basis.v1");
  builder.append_comma();
  builder.append_key_value<"bindings">(bindings);
  builder.append_comma();
  builder.append_key_value<"derivation">(derivation.id);
  builder.end_object();
  return json_from_builder(builder);
}

memix::ViewEnvironmentClosure
closure_for(const memix::ViewDerivation &derivation) {
  std::vector<std::string> artifacts;
  artifacts.reserve(derivation.outputs.size());
  for (const memix::ViewDerivationOutput &output : derivation.outputs) {
    artifacts.push_back(output.artifact);
  }
  std::ranges::sort(artifacts);
  artifacts.erase(std::ranges::unique(artifacts).begin(), artifacts.end());
  return memix::ViewEnvironmentClosure{
      .derivations = {derivation.id},
      .artifacts = std::move(artifacts),
      .raw_objects = unique_blob_ids(derivation.resolved_files),
  };
}

ViewResult<memix::ViewEnvironment>
environment_for_derivation(const memix::Store &store,
                           memix::ViewDerivation derivation) {
  ViewResult<std::string> basis = environment_basis_json_for(derivation);
  if (!basis) {
    return std::unexpected(basis.error());
  }
  ViewResult<std::string> environment_id = sha256_text(*basis);
  if (!environment_id) {
    return std::unexpected(environment_id.error());
  }

  memix::ViewEnvironment environment;
  environment.id = *environment_id;
  environment.manifest_path =
      environment_path_for(store.root(), environment.id);
  environment.bindings = {
      memix::ViewEnvironmentBinding{
          .mount = fs::path(kTreeDir),
          .derivation_id = derivation.id,
          .output_role = "file_tree",
      },
  };
  environment.closure = closure_for(derivation);
  environment.derivation = std::move(derivation);

  ViewResult<std::string> json = memix::view_environment_json(environment);
  if (!json) {
    return std::unexpected(json.error());
  }
  const ViewResult<bool> inserted =
      write_text_file_if_absent(environment.manifest_path, *json);
  if (!inserted) {
    return std::unexpected(inserted.error());
  }
  environment.inserted = *inserted;
  return environment;
}

ViewResult<std::string> resolve_unique_environment_id(const memix::Store &store,
                                                      std::string_view prefix) {
  if (!memix::detail::is_lower_hex_prefix(prefix)) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("environment id prefix must be lowercase "
                                  "hex `{}`",
                                  prefix));
  }
  const fs::path environments_dir = environments_dir_for(store.root());
  const memix::FsResult<bool> exists = fs::exists(environments_dir);
  if (!exists) {
    return view_error(exists.error());
  }
  if (!*exists) {
    return view_error(memix::ErrorKind::NotFound,
                      std::format("no environment matches `{}`", prefix));
  }
  std::vector<std::string> matches;
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{environments_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    if (!entry.is_regular_file(entry_error)) {
      if (entry_error) {
        return view_error(
            fs::make_fs_error("failed to inspect", entry_error, entry.path()));
      }
      continue;
    }
    std::string name = entry.path().filename().string();
    if (!name.ends_with(kJsonSuffix)) {
      continue;
    }
    name.resize(name.size() - kJsonSuffix.size());
    if (memix::detail::is_lower_hex_sha256(name) && name.starts_with(prefix)) {
      matches.push_back(std::move(name));
    }
  }
  if (iterator_error) {
    return view_error(fs::make_fs_error("failed to list directory",
                                        iterator_error, environments_dir));
  }
  if (matches.empty()) {
    return view_error(memix::ErrorKind::NotFound,
                      std::format("no environment matches `{}`", prefix));
  }
  if (matches.size() > 1) {
    return view_error(
        memix::ErrorKind::Conflict,
        std::format("environment prefix `{}` is ambiguous", prefix));
  }
  return matches.front();
}

ViewResult<std::vector<std::string>>
read_string_array(simdjson::dom::object fields, std::string_view key) {
  simdjson::dom::array values;
  const simdjson::error_code error = fields.at_key(key).get(values);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("environment field `{}` must be an array: "
                                  "{}",
                                  key, simdjson_error_text(error)));
  }
  std::vector<std::string> output;
  for (const simdjson::dom::element value_element : values) {
    std::string_view value;
    const simdjson::error_code value_error = value_element.get(value);
    if (value_error != simdjson::SUCCESS) {
      return view_error(memix::ErrorKind::InvalidData,
                        std::format("environment field `{}` values must be "
                                    "strings: {}",
                                    key, simdjson_error_text(value_error)));
    }
    output.emplace_back(value);
  }
  return output;
}

ViewResult<std::string> read_string_field(simdjson::dom::object fields,
                                          std::string_view key) {
  return require_field_as<std::string_view>(fields, key, "a string")
      .transform([](std::string_view value) { return std::string(value); });
}

ViewResult<memix::MemoryObjectType>
read_memory_object_type(simdjson::dom::object fields, std::string_view key) {
  ViewResult<std::string> type_name = read_string_field(fields, key);
  if (!type_name) {
    return std::unexpected(type_name.error());
  }
  const std::optional<memix::MemoryObjectType> type =
      memix::parse_memory_object_type(*type_name);
  if (!type.has_value()) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported memory object type `{}`", *type_name));
  }
  return *type;
}

ViewResult<memix::ViewDerivationOutput>
derivation_output_from_json(simdjson::dom::element output_element) {
  simdjson::dom::object fields;
  const simdjson::error_code error = output_element.get(fields);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("derivation output must be an object: {}",
                                  simdjson_error_text(error)));
  }
  ViewResult<std::string> role = read_string_field(fields, "role");
  ViewResult<std::string> artifact = read_string_field(fields, "artifact");
  ViewResult<std::string> path = read_string_field(fields, "path");
  if (!role) {
    return std::unexpected(role.error());
  }
  if (!artifact) {
    return std::unexpected(artifact.error());
  }
  if (!path) {
    return std::unexpected(path.error());
  }
  return memix::ViewDerivationOutput{
      .role = std::move(*role),
      .artifact = std::move(*artifact),
      .path = fs::path(std::move(*path)),
  };
}

ViewResult<std::vector<memix::ViewDerivationOutput>>
derivation_outputs_from_json(simdjson::dom::object fields) {
  simdjson::dom::array outputs;
  const simdjson::error_code error = fields.at_key("outputs").get(outputs);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("derivation outputs must be an array: {}",
                                  simdjson_error_text(error)));
  }
  std::vector<memix::ViewDerivationOutput> parsed;
  for (const simdjson::dom::element output_element : outputs) {
    ViewResult<memix::ViewDerivationOutput> output =
        derivation_output_from_json(output_element);
    if (!output) {
      return std::unexpected(output.error());
    }
    parsed.push_back(std::move(*output));
  }
  return parsed;
}

ViewResult<memix::ResolvedViewFile>
resolved_file_from_json(const memix::Store &store,
                        simdjson::dom::element input_element) {
  simdjson::dom::object fields;
  const simdjson::error_code error = input_element.get(fields);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("resolved input must be an object: {}",
                                  simdjson_error_text(error)));
  }
  ViewResult<std::string> locator = read_string_field(fields, "locator");
  ViewResult<memix::MemoryObjectType> type =
      read_memory_object_type(fields, "type");
  ViewResult<std::string> path = read_string_field(fields, "path");
  ViewResult<std::string> output_path =
      read_string_field(fields, "output_path");
  ViewResult<std::string> generation = read_string_field(fields, "generation");
  ViewResult<std::string> blob_id = read_string_field(fields, "blob");
  if (!locator) {
    return std::unexpected(locator.error());
  }
  if (!type) {
    return std::unexpected(type.error());
  }
  if (!path) {
    return std::unexpected(path.error());
  }
  if (!output_path) {
    return std::unexpected(output_path.error());
  }
  if (!generation) {
    return std::unexpected(generation.error());
  }
  if (!blob_id) {
    return std::unexpected(blob_id.error());
  }

  memix::StoreResult<memix::StoredObject> blob =
      store.read_blob(*type, *blob_id);
  if (!blob) {
    return view_error(blob.error());
  }
  return memix::ResolvedViewFile{
      .locator = std::move(*locator),
      .type = *type,
      .source_path = fs::path(std::move(*path)),
      .source_generation = std::move(*generation),
      .output_path = fs::path(std::move(*output_path)),
      .blob = std::move(*blob),
  };
}

ViewResult<std::vector<memix::ResolvedViewFile>>
resolved_files_from_json(const memix::Store &store,
                         simdjson::dom::object fields) {
  simdjson::dom::array inputs;
  const simdjson::error_code error =
      fields.at_key("resolved_inputs").get(inputs);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("derivation resolved_inputs must be an "
                                  "array: {}",
                                  simdjson_error_text(error)));
  }
  std::vector<memix::ResolvedViewFile> files;
  for (const simdjson::dom::element input_element : inputs) {
    ViewResult<memix::ResolvedViewFile> file =
        resolved_file_from_json(store, input_element);
    if (!file) {
      return std::unexpected(file.error());
    }
    files.push_back(std::move(*file));
  }
  return files;
}

ViewResult<memix::ViewDerivation>
derivation_from_json(const memix::Store &store, std::string_view text,
                     const fs::path &manifest_path) {
  const simdjson::padded_string padded_json(text);
  simdjson::dom::parser parser;
  simdjson::dom::element document;
  simdjson::error_code error = parser.parse(padded_json).get(document);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("invalid derivation JSON `{}`: {}",
                                  manifest_path.string(),
                                  simdjson_error_text(error)));
  }
  simdjson::dom::object fields;
  error = document.get(fields);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("derivation JSON must be an object "
                                  "`{}`: {}",
                                  manifest_path.string(),
                                  simdjson_error_text(error)));
  }

  ViewResult<std::string> schema = read_string_field(fields, "schema");
  ViewResult<std::string> derivation_id =
      read_string_field(fields, "derivation_id");
  ViewResult<std::string> ir_hash = read_string_field(fields, "ir_hash");
  ViewResult<std::string> target_name = read_string_field(fields, "target");
  if (!schema) {
    return std::unexpected(schema.error());
  }
  if (!derivation_id) {
    return std::unexpected(derivation_id.error());
  }
  if (!ir_hash) {
    return std::unexpected(ir_hash.error());
  }
  if (!target_name) {
    return std::unexpected(target_name.error());
  }
  if (*schema != kDerivationSchema) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported derivation schema `{}`", *schema));
  }
  if (manifest_path.parent_path().filename() != *derivation_id) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("derivation manifest id `{}` does not match "
                                  "path `{}`",
                                  *derivation_id, manifest_path.string()));
  }
  ViewResult<memix::ViewTargetKind> target =
      parse_required_target(*target_name);
  if (!target) {
    return std::unexpected(target.error());
  }
  ViewResult<std::vector<memix::ResolvedViewFile>> resolved_files =
      resolved_files_from_json(store, fields);
  if (!resolved_files) {
    return std::unexpected(resolved_files.error());
  }
  ViewResult<std::vector<memix::ViewDerivationOutput>> outputs =
      derivation_outputs_from_json(fields);
  if (!outputs) {
    return std::unexpected(outputs.error());
  }

  return memix::ViewDerivation{
      .id = std::move(*derivation_id),
      .ir_hash = std::move(*ir_hash),
      .target = *target,
      .resolved_files = std::move(*resolved_files),
      .outputs = std::move(*outputs),
      .manifest_path = manifest_path,
      .tree_path = manifest_path.parent_path() / std::string(kTreeDir),
      .inserted = false,
  };
}

ViewResult<memix::ViewDerivation>
resolve_derivation(const memix::Store &store, std::string_view derivation_id) {
  if (!memix::detail::is_lower_hex_sha256(derivation_id)) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("derivation id is not a lowercase sha256 "
                                  "digest `{}`",
                                  derivation_id));
  }
  const fs::path manifest_path = derivation_dir_for(store.root(), derivation_id)
                               / std::string(kManifestFilename);
  memix::FsResult<std::string> text =
      fs::read_text_file(manifest_path, "view derivation");
  if (!text) {
    return view_error(text.error());
  }
  return derivation_from_json(store, *text, manifest_path);
}

ViewResult<memix::ViewEnvironment>
environment_from_json(std::string_view text, const fs::path &manifest_path) {
  const simdjson::padded_string padded_json(text);
  simdjson::dom::parser parser;
  simdjson::dom::element document;
  simdjson::error_code error = parser.parse(padded_json).get(document);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("invalid environment JSON `{}`: {}",
                                  manifest_path.string(),
                                  simdjson_error_text(error)));
  }
  simdjson::dom::object fields;
  error = document.get(fields);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("environment JSON must be an object "
                                  "`{}`: {}",
                                  manifest_path.string(),
                                  simdjson_error_text(error)));
  }
  ViewResult<std::string> schema =
      require_field_as<std::string_view>(fields, "schema", "a string")
          .transform([](std::string_view value) { return std::string(value); });
  if (!schema) {
    return std::unexpected(schema.error());
  }
  if (*schema != kEnvironmentSchema) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported environment schema `{}`", *schema));
  }
  ViewResult<std::string> environment_id =
      require_field_as<std::string_view>(fields, "environment_id", "a string")
          .transform([](std::string_view value) { return std::string(value); });
  if (!environment_id) {
    return std::unexpected(environment_id.error());
  }
  if (!memix::detail::is_lower_hex_sha256(*environment_id)) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("environment id is not a lowercase sha256 "
                                  "digest `{}`",
                                  *environment_id));
  }
  std::string expected_environment_id = manifest_path.filename().string();
  if (!expected_environment_id.ends_with(kJsonSuffix)) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("environment manifest path must end with "
                                  "`.json` `{}`",
                                  manifest_path.string()));
  }
  expected_environment_id.resize(expected_environment_id.size()
                                 - kJsonSuffix.size());
  if (*environment_id != expected_environment_id) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("environment manifest id `{}` does not "
                                  "match path `{}`",
                                  *environment_id, manifest_path.string()));
  }

  memix::ViewEnvironment environment;
  environment.id = *environment_id;
  environment.manifest_path = manifest_path;

  simdjson::dom::array bindings;
  error = fields.at_key("bindings").get(bindings);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("environment bindings must be an array: "
                                  "{}",
                                  simdjson_error_text(error)));
  }
  for (const simdjson::dom::element binding_element : bindings) {
    simdjson::dom::object binding_fields;
    error = binding_element.get(binding_fields);
    if (error != simdjson::SUCCESS) {
      return view_error(memix::ErrorKind::InvalidData,
                        std::format("environment binding must be an object: "
                                    "{}",
                                    simdjson_error_text(error)));
    }
    ViewResult<std::string> mount =
        require_field_as<std::string_view>(binding_fields, "mount", "a string")
            .transform(
                [](std::string_view value) { return std::string(value); });
    ViewResult<std::string> derivation =
        require_field_as<std::string_view>(binding_fields, "derivation",
                                           "a string")
            .transform(
                [](std::string_view value) { return std::string(value); });
    ViewResult<std::string> output =
        require_field_as<std::string_view>(binding_fields, "output", "a string")
            .transform(
                [](std::string_view value) { return std::string(value); });
    if (!mount) {
      return std::unexpected(mount.error());
    }
    if (!derivation) {
      return std::unexpected(derivation.error());
    }
    if (!output) {
      return std::unexpected(output.error());
    }
    environment.bindings.push_back(memix::ViewEnvironmentBinding{
        .mount = fs::path(*mount),
        .derivation_id = *derivation,
        .output_role = *output,
    });
  }

  simdjson::dom::object closure_fields;
  error = fields.at_key("closure").get(closure_fields);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("environment closure must be an object: "
                                  "{}",
                                  simdjson_error_text(error)));
  }
  ViewResult<std::vector<std::string>> derivations =
      read_string_array(closure_fields, "derivations");
  ViewResult<std::vector<std::string>> artifacts =
      read_string_array(closure_fields, "artifacts");
  ViewResult<std::vector<std::string>> raw_objects =
      read_string_array(closure_fields, "raw_objects");
  if (!derivations) {
    return std::unexpected(derivations.error());
  }
  if (!artifacts) {
    return std::unexpected(artifacts.error());
  }
  if (!raw_objects) {
    return std::unexpected(raw_objects.error());
  }
  environment.closure = memix::ViewEnvironmentClosure{
      .derivations = std::move(*derivations),
      .artifacts = std::move(*artifacts),
      .raw_objects = std::move(*raw_objects),
  };
  return environment;
}

} // namespace

namespace memix {

std::string_view to_string(ViewTargetKind target) noexcept {
  // MSVC array iterators are not pointers.
  // NOLINTNEXTLINE(readability-qualified-auto)
  auto it = std::ranges::find(kViewTargetKindTokens, target,
                              &ViewTargetKindToken::value);
  if (it == std::ranges::end(kViewTargetKindTokens))
    return {};
  return it->token;
}

std::optional<ViewTargetKind>
parse_view_target_kind(std::string_view value) noexcept {
  // MSVC array iterators are not pointers.
  // NOLINTNEXTLINE(readability-qualified-auto)
  auto it = std::ranges::find(kViewTargetKindTokens, value,
                              &ViewTargetKindToken::token);
  if (it == std::ranges::end(kViewTargetKindTokens))
    return std::nullopt;
  return it->value;
}

std::string_view to_string(ViewVersionKind kind) noexcept {
  // MSVC array iterators are not pointers.
  // NOLINTNEXTLINE(readability-qualified-auto)
  auto it = std::ranges::find(kViewVersionKindTokens, kind,
                              &ViewVersionKindToken::value);
  if (it == std::ranges::end(kViewVersionKindTokens))
    return {};
  return it->token;
}

ViewResult<AuthoredViewExpression>
ViewService::parse_expression(std::string_view expression_or_sources) const {
  const std::string trimmed = trim_copy(expression_or_sources);
  if (trimmed.empty()) {
    return AuthoredViewExpression{};
  }
  if (trimmed.front() == '{') {
    return parse_expression_json(trimmed);
  }
  return expression_from_sources(split_source_list(trimmed));
}

ViewResult<AuthoredViewExpression>
ViewService::expression_from_sources(std::vector<std::string> sources,
                                     ViewTargetKind target) const {
  const Store &unused_store = store_;
  (void)unused_store;
  return AuthoredViewExpression{
      .schema = std::string(kExpressionSchema),
      .profile = {},
      .sources = std::move(sources),
      .target = target,
  };
}

ViewResult<ViewIr>
ViewService::lower_to_ir(const AuthoredViewExpression &expression) const {
  const Store &unused_store = store_;
  (void)unused_store;
  if (!expression.schema.empty() && expression.schema != kExpressionSchema) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("unsupported view expression schema "
                                  "`{}`",
                                  expression.schema));
  }
  ViewIr ir;
  ir.target.kind = expression.target;
  if (expression.target == ViewTargetKind::ContextPack) {
    ir.target.format = "markdown";
  }
  ir.inputs.reserve(expression.sources.size());
  for (const std::string &source : expression.sources) {
    ViewResult<ViewIrInput> input = parse_locator(source);
    if (!input) {
      return std::unexpected(input.error());
    }
    ir.inputs.push_back(std::move(*input));
  }
  return ir;
}

ViewResult<ViewDerivation> ViewService::build_ir(const ViewIr &ir) const {
  if (ir.target.kind != ViewTargetKind::FileTree) {
    return view_error(ErrorKind::InvalidArgument,
                      "only file_tree target builds are supported in v1");
  }
  ViewResult<std::string> ir_json = view_ir_json(ir);
  if (!ir_json) {
    return std::unexpected(ir_json.error());
  }
  ViewResult<std::string> ir_hash = sha256_text(*ir_json);
  if (!ir_hash) {
    return std::unexpected(ir_hash.error());
  }
  ViewResult<std::vector<ResolvedViewFile>> files = resolve_inputs(store_, ir);
  if (!files) {
    return std::unexpected(files.error());
  }
  const std::vector<ResolvedInputJson> inputs = resolved_input_json(*files);
  ViewResult<std::string> basis =
      ir_basis_json_for(*ir_hash, inputs, ir.target.kind);
  if (!basis) {
    return std::unexpected(basis.error());
  }
  ViewResult<std::string> derivation_id = sha256_text(*basis);
  if (!derivation_id) {
    return std::unexpected(derivation_id.error());
  }
  ViewResult<std::vector<ViewDerivationOutput>> outputs =
      outputs_for_derivation(*derivation_id, ir.target.kind);
  if (!outputs) {
    return std::unexpected(outputs.error());
  }

  const fs::path derivation_dir =
      derivation_dir_for(store_.root(), *derivation_id);
  ViewDerivation derivation{
      .id = *derivation_id,
      .ir_hash = *ir_hash,
      .target = ir.target.kind,
      .resolved_files = std::move(*files),
      .outputs = std::move(*outputs),
      .manifest_path = derivation_dir / std::string(kManifestFilename),
      .tree_path = derivation_dir / std::string(kTreeDir),
      .inserted = false,
  };
  ViewResult<std::string> manifest = view_derivation_json(derivation);
  if (!manifest) {
    return std::unexpected(manifest.error());
  }
  const ViewResult<bool> inserted =
      write_derivation_directory(store_, derivation, *manifest);
  if (!inserted) {
    return std::unexpected(inserted.error());
  }
  derivation.inserted = *inserted;
  return derivation;
}

ViewResult<ViewEnvironment>
ViewService::build(const AuthoredViewExpression &expression) const {
  ViewResult<ViewIr> ir = lower_to_ir(expression);
  if (!ir) {
    return std::unexpected(ir.error());
  }
  ViewResult<ViewDerivation> derivation = build_ir(*ir);
  if (!derivation) {
    return std::unexpected(derivation.error());
  }
  return environment_for_derivation(store_, std::move(*derivation));
}

ViewResult<ViewEnvironment>
ViewService::resolve_environment(std::string_view environment_prefix) const {
  ViewResult<std::string> environment_id =
      resolve_unique_environment_id(store_, environment_prefix);
  if (!environment_id) {
    return std::unexpected(environment_id.error());
  }
  const fs::path path = environment_path_for(store_.root(), *environment_id);
  memix::FsResult<std::string> text =
      fs::read_text_file(path, "view environment");
  if (!text) {
    return view_error(text.error());
  }
  ViewResult<ViewEnvironment> environment = environment_from_json(*text, path);
  if (!environment) {
    return std::unexpected(environment.error());
  }
  if (environment->bindings.size() != 1) {
    return view_error(ErrorKind::InvalidData,
                      std::format("view environment `{}` must have exactly "
                                  "one binding in v1",
                                  *environment_id));
  }
  const ViewEnvironmentBinding &binding = environment->bindings.front();
  if (binding.output_role != "file_tree") {
    return view_error(ErrorKind::InvalidData,
                      std::format("unsupported environment output `{}`",
                                  binding.output_role));
  }
  if (!std::ranges::contains(environment->closure.derivations,
                             binding.derivation_id)) {
    return view_error(ErrorKind::InvalidData,
                      std::format("environment `{}` binding derivation `{}` "
                                  "is missing from the closure",
                                  *environment_id, binding.derivation_id));
  }
  ViewResult<ViewDerivation> derivation =
      resolve_derivation(store_, binding.derivation_id);
  if (!derivation) {
    return std::unexpected(derivation.error());
  }
  environment->derivation = std::move(*derivation);
  return environment;
}

ViewResult<std::string> view_ir_json(const ViewIr &ir) {
  const std::vector<IrInputJson> inputs =
      ir.inputs
      | std::views::transform(ir_input_json_for)
      | std::ranges::to<std::vector>();
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">(kIrSchema);
  builder.append_comma();
  builder.append_key_value<"inputs">(inputs);
  builder.append_comma();
  builder.append_key_value<"target">(ir_target_json_for(ir.target));
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<std::string> view_derivation_json(const ViewDerivation &derivation) {
  const std::vector<PassJson> passes{
      PassJson{.name = "resolve-refs", .version = "v1"},
      PassJson{.name = "expand-dirs", .version = "v1"},
      PassJson{.name = "pack-file-tree", .version = "v1"},
  };
  const std::vector<ResolvedInputJson> inputs =
      resolved_input_json(derivation.resolved_files);
  const std::vector<OutputJson> outputs = output_json(derivation.outputs);

  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">(kDerivationSchema);
  builder.append_comma();
  builder.append_key_value<"derivation_id">(derivation.id);
  builder.append_comma();
  builder.append_key_value<"ir_hash">(derivation.ir_hash);
  builder.append_comma();
  builder.append_key_value<"target">(memix::to_string(derivation.target));
  builder.append_comma();
  builder.append_key_value<"operator_version">(kFileTreeOperatorVersion);
  builder.append_comma();
  builder.append_key_value<"passes">(passes);
  builder.append_comma();
  builder.append_key_value<"policy_snapshot">(std::optional<std::string>{});
  builder.append_comma();
  builder.append_key_value<"resolved_inputs">(inputs);
  builder.append_comma();
  builder.append_key_value<"outputs">(outputs);
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<std::string>
view_environment_json(const ViewEnvironment &environment) {
  const std::vector<BindingJson> bindings = binding_json(environment.bindings);
  const ClosureJson closure{
      .derivations = environment.closure.derivations,
      .artifacts = environment.closure.artifacts,
      .raw_objects = environment.closure.raw_objects,
  };
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">(kEnvironmentSchema);
  builder.append_comma();
  builder.append_key_value<"environment_id">(environment.id);
  builder.append_comma();
  builder.append_key_value<"bindings">(bindings);
  builder.append_comma();
  builder.append_key_value<"closure">(closure);
  builder.end_object();
  return json_from_builder(builder);
}

} // namespace memix
