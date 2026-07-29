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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <limits>
#include <map>
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
constexpr std::string_view kExpressionSchemaV2 = "memix.view.expr.v2";
constexpr std::string_view kIrSchema = "memix.view.ir.v1";
constexpr std::string_view kIrSchemaV2 = "memix.view.ir.v2";
constexpr std::string_view kDerivationSchema = "memix.view.derivation.v1";
constexpr std::string_view kDerivationSchemaV2 = "memix.view.derivation.v2";
constexpr std::string_view kEnvironmentSchema = "memix.view.environment.v1";
constexpr std::string_view kEnvironmentSchemaV2 = "memix.view.environment.v2";
constexpr std::string_view kViewsDir = "views";
constexpr std::string_view kDerivationsDir = "derivations";
constexpr std::string_view kEnvironmentsDir = "environments";
constexpr std::string_view kQuestsDir = "quests";
constexpr std::string_view kManifestFilename = "manifest.json";
constexpr std::string_view kTreeDir = "tree";
constexpr std::string_view kJsonSuffix = ".json";
constexpr std::string_view kQuestSchema = "memix.quest.v1";
constexpr std::string_view kQuestIrFilename = "ir.json";
constexpr std::string_view kQuestManifestFilename = "quest.json";
constexpr std::string_view kQuestHistoryFilename = "history.jsonl";
constexpr std::string_view kQuestReceiptsFilename = "receipts.jsonl";
constexpr std::string_view kLatestVersion = "latest";
constexpr std::string_view kFileTreeOperatorVersion = "file-tree-v1";
constexpr std::string_view kFileTreeOperatorVersionV2 = "file-tree-v2";
constexpr std::string_view kProfilesDir = "profiles";
constexpr std::string_view kGenerationsDir = "generations";
constexpr std::string_view kCurrentLinkName = "current";
constexpr std::string_view kProfileLockName = "lock";
constexpr std::string_view kGenerationSchema = "memix.view.activation.v1";
constexpr std::string_view kMarkerSchema = "memix.view.activation.marker.v1";
constexpr std::string_view kActivationMarkerFilename = ".memix-activation.json";
constexpr std::string_view kWorkspaceLockPrefix = ".memix-activation-lock-";
constexpr std::string_view kWorkspaceProfilePrefix = "ws-";
constexpr std::size_t kWorkspaceProfileHashLength = 16;
constexpr std::size_t kMaxProfileNameLength = 64;
constexpr std::size_t kTimestampBufferBytes = 32;

template <typename Value>
using ViewResult = memix::ViewResult<Value>;

using ViewVoid = memix::ViewVoid;
using memix::detail::JsonObjectReader;

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

struct TypedIrInputJson {
  memix::ViewInputKind kind = memix::ViewInputKind::Context;
  IrInputJson context;
  std::string environment;
  std::string output;
  std::string mount;
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

struct ResolvedEnvironmentInputJson {
  std::string environment;
  std::string derivation;
  std::string artifact;
  std::string output;
  std::string mount;
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

struct ClosureV2Json {
  std::reference_wrapper<const std::vector<std::string>> environments;
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
                const TypedIrInputJson &input) {
  builder.start_object();
  builder.template append_key_value<"kind">(memix::to_string(input.kind));
  builder.append_comma();
  if (input.kind == memix::ViewInputKind::Context) {
    builder.template append_key_value<"locator">(input.context.locator);
    builder.append_comma();
    builder.template append_key_value<"type">(input.context.type);
    builder.append_comma();
    builder.template append_key_value<"path">(input.context.path);
    builder.append_comma();
    builder.template append_key_value<"version">(input.context.version);
    builder.append_comma();
    builder.template append_key_value<"selector">(input.context.selector);
  } else {
    builder.template append_key_value<"environment">(input.environment);
    builder.append_comma();
    builder.template append_key_value<"output">(input.output);
    builder.append_comma();
    builder.template append_key_value<"mount">(input.mount);
  }
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
                const ResolvedEnvironmentInputJson &input) {
  builder.start_object();
  builder.template append_key_value<"environment">(input.environment);
  builder.append_comma();
  builder.template append_key_value<"derivation">(input.derivation);
  builder.append_comma();
  builder.template append_key_value<"artifact">(input.artifact);
  builder.append_comma();
  builder.template append_key_value<"output">(input.output);
  builder.append_comma();
  builder.template append_key_value<"mount">(input.mount);
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

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ClosureV2Json &closure) {
  builder.start_object();
  builder.template append_key_value<"environments">(closure.environments.get());
  builder.append_comma();
  builder.template append_key_value<"derivations">(closure.derivations.get());
  builder.append_comma();
  builder.template append_key_value<"artifacts">(closure.artifacts.get());
  builder.append_comma();
  builder.template append_key_value<"raw_objects">(closure.raw_objects.get());
  builder.end_object();
}

std::unexpected<memix::ViewError>
view_error(memix::ErrorKind kind, std::string message) {
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

std::unexpected<memix::ViewError>
view_error(memix::detail::CommonError error, memix::ErrorKind kind) {
  error.kind = kind;
  return view_error(std::move(error));
}

ViewResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder) {
  memix::detail::CommonResult<std::string> json =
      memix::detail::json_from_builder(builder);
  if (!json) {
    return view_error(memix::detail::with_error_context(std::move(json.error()),
                                                        "view JSON"));
  }
  return ViewResult<std::string>{std::in_place, std::move(*json)};
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

ViewResult<std::optional<std::string>>
optional_string(const JsonObjectReader &fields, std::string_view key) {
  memix::detail::CommonResult<std::optional<std::string>> value =
      fields.optional_string(key);
  if (!value) {
    return view_error(std::move(value.error()), memix::ErrorKind::InvalidData);
  }
  return ViewResult<std::optional<std::string>>{std::in_place,
                                                std::move(*value)};
}

ViewResult<simdjson::dom::array>
optional_sources_array(const JsonObjectReader &fields, bool &has_sources) {
  memix::detail::CommonResult<std::optional<simdjson::dom::array>> sources =
      fields.optional_array("sources");
  if (!sources) {
    return view_error(std::move(sources.error()),
                      memix::ErrorKind::InvalidData);
  }
  if (!sources->has_value()) {
    has_sources = false;
    return simdjson::dom::array{};
  }
  has_sources = true;
  return **sources;
}

constexpr bool valid_logical_component(std::string_view component) {
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

ViewResult<fs::path>
validate_mount_path(std::string_view value, std::string_view context) {
  if (value.empty()) {
    return fs::path{};
  }
  return validate_view_path({
      .value = value,
      .locator = context,
  });
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

std::string
version_ref_name(memix::MemoryObjectType type, std::string_view version) {
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

std::string
canonical_locator(memix::MemoryObjectType type, const fs::path &path,
                  const memix::ViewVersionSelector &version, bool recursive) {
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
      .kind = memix::ViewInputKind::Context,
      .environment_id = {},
      .output_role = "file_tree",
      .mount = {},
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

ViewResult<std::string>
read_string_field(const JsonObjectReader &fields, std::string_view key);

ViewResult<bool>
apply_expression_metadata(const JsonObjectReader &fields,
                          memix::AuthoredViewExpression &expression) {
  ViewResult<std::optional<std::string>> schema =
      optional_string(fields, "schema");
  if (!schema) {
    return std::unexpected(schema.error());
  }
  const bool schema_explicit = schema->has_value();
  if (schema_explicit) {
    if (**schema != kExpressionSchema && **schema != kExpressionSchemaV2) {
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
  return schema_explicit;
}

ViewResult<memix::AuthoredViewInput>
parse_authored_environment_input(const JsonObjectReader &fields) {
  ViewResult<std::string> environment =
      read_string_field(fields, "environment");
  ViewResult<std::optional<std::string>> output =
      optional_string(fields, "output");
  ViewResult<std::optional<std::string>> mount =
      optional_string(fields, "mount");
  if (!environment) {
    return std::unexpected(environment.error());
  }
  if (!output) {
    return std::unexpected(output.error());
  }
  if (!mount) {
    return std::unexpected(mount.error());
  }
  return memix::AuthoredViewInput{
      .kind = memix::ViewInputKind::Environment,
      .locator = {},
      .environment_id = std::move(*environment),
      .output_role = output->value_or("file_tree"),
      .mount = fs::path(mount->value_or("")),
  };
}

ViewResult<memix::AuthoredViewInput>
parse_authored_input(simdjson::dom::element input_element) {
  memix::detail::CommonResult<simdjson::dom::object> fields_result =
      memix::detail::json_object_from_element(input_element);
  if (!fields_result) {
    return view_error(memix::detail::with_error_context(
        std::move(fields_result.error()), "view expression input",
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields = JsonObjectReader(*fields_result);
  ViewResult<std::string> kind_name = read_string_field(fields, "kind");
  if (!kind_name) {
    return std::unexpected(kind_name.error());
  }
  const std::optional<memix::ViewInputKind> kind =
      memix::parse_view_input_kind(*kind_name);
  if (!kind.has_value()) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported view input kind `{}`", *kind_name));
  }
  if (*kind == memix::ViewInputKind::Environment) {
    return parse_authored_environment_input(fields);
  }
  ViewResult<std::string> locator = read_string_field(fields, "locator");
  if (!locator) {
    return std::unexpected(locator.error());
  }
  return memix::AuthoredViewInput{
      .kind = memix::ViewInputKind::Context,
      .locator = std::move(*locator),
      .environment_id = {},
      .output_role = "file_tree",
      .mount = {},
  };
}

ViewResult<std::vector<memix::AuthoredViewInput>>
parse_authored_inputs(simdjson::dom::array input_elements) {
  std::vector<memix::AuthoredViewInput> inputs;
  for (const simdjson::dom::element input_element : input_elements) {
    ViewResult<memix::AuthoredViewInput> input =
        parse_authored_input(input_element);
    if (!input) {
      return std::unexpected(input.error());
    }
    inputs.push_back(std::move(*input));
  }
  return inputs;
}

ViewResult<std::vector<std::string>>
parse_authored_sources(simdjson::dom::array source_elements) {
  std::vector<std::string> sources;
  for (const simdjson::dom::element source_element : source_elements) {
    memix::detail::CommonResult<std::string_view> source =
        memix::detail::json_value_as<std::string_view>(source_element);
    if (!source) {
      return view_error(memix::detail::with_error_context(
          std::move(source.error()), "view expression sources",
          memix::ErrorKind::InvalidData));
    }
    sources.emplace_back(*source);
  }
  return sources;
}

ViewResult<memix::AuthoredViewExpression>
parse_expression_json(std::string_view text) {
  memix::detail::CommonResult<memix::detail::JsonDocument> document =
      memix::detail::parse_json_object(text);
  if (!document) {
    return view_error(memix::detail::with_error_context(
        std::move(document.error()), "invalid view expression JSON",
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields = JsonObjectReader(document->root_object());

  memix::AuthoredViewExpression expression;
  ViewResult<bool> schema_explicit =
      apply_expression_metadata(fields, expression);
  if (!schema_explicit) {
    return std::unexpected(schema_explicit.error());
  }
  bool has_sources = false;
  ViewResult<simdjson::dom::array> sources =
      optional_sources_array(fields, has_sources);
  if (!sources) {
    return std::unexpected(sources.error());
  }
  memix::detail::CommonResult<std::optional<simdjson::dom::array>>
      authored_inputs = fields.optional_array("inputs");
  if (!authored_inputs) {
    return view_error(std::move(authored_inputs.error()),
                      memix::ErrorKind::InvalidData);
  }
  if (has_sources && authored_inputs->has_value()) {
    return view_error(memix::ErrorKind::InvalidData,
                      "view expression cannot contain both sources and inputs");
  }
  if (authored_inputs->has_value()) {
    if (*schema_explicit && expression.schema != kExpressionSchemaV2) {
      return view_error(memix::ErrorKind::InvalidData,
                        "typed view inputs require memix.view.expr.v2");
    }
    expression.schema = std::string(kExpressionSchemaV2);
    ViewResult<std::vector<memix::AuthoredViewInput>> inputs =
        parse_authored_inputs(**authored_inputs);
    if (!inputs) {
      return std::unexpected(inputs.error());
    }
    expression.inputs = std::move(*inputs);
    return expression;
  }
  if (has_sources) {
    if (expression.schema == kExpressionSchemaV2) {
      return view_error(memix::ErrorKind::InvalidData,
                        "memix.view.expr.v2 requires typed inputs");
    }
    ViewResult<std::vector<std::string>> parsed_sources =
        parse_authored_sources(*sources);
    if (!parsed_sources) {
      return std::unexpected(parsed_sources.error());
    }
    expression.sources = std::move(*parsed_sources);
  }

  return expression;
}

ViewResult<bool>
read_bool_field(const JsonObjectReader &fields, std::string_view key) {
  memix::detail::CommonResult<simdjson::dom::element> element =
      fields.require_field(key);
  if (!element) {
    return view_error(std::move(element.error()),
                      memix::ErrorKind::InvalidData);
  }
  bool value = false;
  const simdjson::error_code error = element->get(value);
  if (error != simdjson::SUCCESS) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("JSON field `{}` must be a boolean: {}", key,
                                  memix::detail::simdjson_error_text(error)));
  }
  return value;
}

ViewResult<memix::ViewVersionSelector>
ir_version_from_json(const JsonObjectReader &input_fields) {
  memix::detail::CommonResult<simdjson::dom::object> version_result =
      input_fields.require_object("version");
  if (!version_result) {
    return view_error(std::move(version_result.error()),
                      memix::ErrorKind::InvalidData);
  }
  const JsonObjectReader version_fields(*version_result);
  ViewResult<std::string> kind_name = read_string_field(version_fields, "kind");
  ViewResult<std::string> value = read_string_field(version_fields, "value");
  if (!kind_name) {
    return std::unexpected(kind_name.error());
  }
  if (!value) {
    return std::unexpected(value.error());
  }
  memix::ViewVersionKind kind = memix::ViewVersionKind::Ref;
  if (*kind_name == "generation") {
    kind = memix::ViewVersionKind::Generation;
  } else if (*kind_name != "ref") {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported view version kind `{}`", *kind_name));
  }
  return memix::ViewVersionSelector{
      .kind = kind,
      .value = std::move(*value),
  };
}

ViewResult<memix::ViewIrInput>
context_ir_input_from_json(const JsonObjectReader &fields) {
  ViewResult<std::string> locator = read_string_field(fields, "locator");
  ViewResult<std::string> type_name = read_string_field(fields, "type");
  ViewResult<std::string> path = read_string_field(fields, "path");
  ViewResult<memix::ViewVersionSelector> version = ir_version_from_json(fields);
  memix::detail::CommonResult<simdjson::dom::object> selector_result =
      fields.require_object("selector");
  if (!locator) {
    return std::unexpected(locator.error());
  }
  if (!type_name) {
    return std::unexpected(type_name.error());
  }
  if (!path) {
    return std::unexpected(path.error());
  }
  if (!version) {
    return std::unexpected(version.error());
  }
  if (!selector_result) {
    return view_error(std::move(selector_result.error()),
                      memix::ErrorKind::InvalidData);
  }
  const std::optional<memix::MemoryObjectType> type =
      memix::parse_memory_object_type(*type_name);
  if (!type.has_value()) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported memory object type `{}`", *type_name));
  }
  ViewResult<fs::path> valid_path =
      validate_view_path({.value = *path, .locator = *locator});
  if (!valid_path) {
    return std::unexpected(valid_path.error());
  }
  const JsonObjectReader selector_fields(*selector_result);
  ViewResult<bool> recursive = read_bool_field(selector_fields, "recursive");
  ViewResult<bool> required = read_bool_field(selector_fields, "required");
  if (!recursive) {
    return std::unexpected(recursive.error());
  }
  if (!required) {
    return std::unexpected(required.error());
  }
  return memix::ViewIrInput{
      .locator = std::move(*locator),
      .type = *type,
      .path = std::move(*valid_path),
      .version = std::move(*version),
      .recursive = *recursive,
      .required = *required,
      .kind = memix::ViewInputKind::Context,
      .environment_id = {},
      .output_role = "file_tree",
      .mount = {},
  };
}

ViewResult<memix::ViewIrInput>
environment_ir_input_from_json(const JsonObjectReader &fields) {
  ViewResult<std::string> environment =
      read_string_field(fields, "environment");
  ViewResult<std::string> output = read_string_field(fields, "output");
  ViewResult<std::string> mount = read_string_field(fields, "mount");
  if (!environment) {
    return std::unexpected(environment.error());
  }
  if (!output) {
    return std::unexpected(output.error());
  }
  if (!mount) {
    return std::unexpected(mount.error());
  }
  ViewResult<fs::path> valid_mount = validate_mount_path(*mount, *environment);
  if (!valid_mount) {
    return std::unexpected(valid_mount.error());
  }
  return memix::ViewIrInput{
      .locator = {},
      .type = memix::MemoryObjectType::Rules,
      .path = {},
      .version = {},
      .recursive = false,
      .required = true,
      .kind = memix::ViewInputKind::Environment,
      .environment_id = std::move(*environment),
      .output_role = std::move(*output),
      .mount = std::move(*valid_mount),
  };
}

ViewResult<memix::ViewIrInput>
ir_input_from_json(simdjson::dom::element input_element, bool typed) {
  memix::detail::CommonResult<simdjson::dom::object> fields_result =
      memix::detail::json_object_from_element(input_element);
  if (!fields_result) {
    return view_error(memix::detail::with_error_context(
        std::move(fields_result.error()), "view IR input",
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields(*fields_result);
  if (!typed) {
    return context_ir_input_from_json(fields);
  }
  ViewResult<std::string> kind_name = read_string_field(fields, "kind");
  if (!kind_name) {
    return std::unexpected(kind_name.error());
  }
  const std::optional<memix::ViewInputKind> kind =
      memix::parse_view_input_kind(*kind_name);
  if (!kind.has_value()) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported view input kind `{}`", *kind_name));
  }
  if (*kind == memix::ViewInputKind::Environment) {
    return environment_ir_input_from_json(fields);
  }
  return context_ir_input_from_json(fields);
}

ViewResult<memix::ViewIrTarget>
ir_target_from_json(const JsonObjectReader &fields) {
  memix::detail::CommonResult<simdjson::dom::object> target_result =
      fields.require_object("target");
  if (!target_result) {
    return view_error(std::move(target_result.error()),
                      memix::ErrorKind::InvalidData);
  }
  const JsonObjectReader target_fields(*target_result);
  ViewResult<std::string> op = read_string_field(target_fields, "op");
  memix::detail::CommonResult<simdjson::dom::object> attrs_result =
      target_fields.require_object("attrs");
  if (!op) {
    return std::unexpected(op.error());
  }
  if (!attrs_result) {
    return view_error(std::move(attrs_result.error()),
                      memix::ErrorKind::InvalidData);
  }
  ViewResult<memix::ViewTargetKind> kind = parse_required_target(*op);
  if (!kind) {
    return std::unexpected(kind.error());
  }
  const JsonObjectReader attrs_fields(*attrs_result);
  memix::detail::CommonResult<simdjson::dom::element> format_element =
      attrs_fields.require_field("format");
  if (!format_element) {
    return view_error(std::move(format_element.error()),
                      memix::ErrorKind::InvalidData);
  }
  std::string format;
  if (!format_element->is_null()) {
    memix::detail::CommonResult<std::string_view> format_value =
        memix::detail::json_value_as<std::string_view>(*format_element);
    if (!format_value) {
      return view_error(std::move(format_value.error()),
                        memix::ErrorKind::InvalidData);
    }
    format = *format_value;
  }
  return memix::ViewIrTarget{
      .kind = *kind,
      .format = std::move(format),
  };
}

struct IrJsonParseRequest {
  std::string_view text;
  std::string_view context;
};

ViewResult<memix::ViewIr> ir_from_json(const IrJsonParseRequest &request) {
  memix::detail::CommonResult<memix::detail::JsonDocument> document =
      memix::detail::parse_json_object(request.text);
  if (!document) {
    return view_error(memix::detail::with_error_context(
        std::move(document.error()), std::string(request.context),
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields(document->root_object());
  ViewResult<std::string> schema = read_string_field(fields, "schema");
  if (!schema) {
    return std::unexpected(schema.error());
  }
  const bool typed = *schema == kIrSchemaV2;
  if (*schema != kIrSchema && !typed) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("unsupported view IR schema `{}`", *schema));
  }
  memix::detail::CommonResult<simdjson::dom::array> inputs_result =
      fields.require_array("inputs");
  if (!inputs_result) {
    return view_error(std::move(inputs_result.error()),
                      memix::ErrorKind::InvalidData);
  }
  std::vector<memix::ViewIrInput> inputs;
  for (const simdjson::dom::element input_element : *inputs_result) {
    ViewResult<memix::ViewIrInput> input =
        ir_input_from_json(input_element, typed);
    if (!input) {
      return std::unexpected(input.error());
    }
    inputs.push_back(std::move(*input));
  }
  ViewResult<memix::ViewIrTarget> target = ir_target_from_json(fields);
  if (!target) {
    return std::unexpected(target.error());
  }
  return memix::ViewIr{
      .schema = std::move(*schema),
      .inputs = std::move(inputs),
      .target = std::move(*target),
  };
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

TypedIrInputJson typed_ir_input_json_for(const memix::ViewIrInput &input) {
  return TypedIrInputJson{
      .kind = input.kind,
      .context = ir_input_json_for(input),
      .environment = input.environment_id,
      .output = input.output_role,
      .mount = input.mount.generic_string(),
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

ViewResult<std::string> ir_basis_json_for_v2(
    std::string_view ir_hash, const std::vector<ResolvedInputJson> &inputs,
    const std::vector<ResolvedEnvironmentInputJson> &environment_inputs,
    memix::ViewTargetKind target) {
  const std::vector<PassJson> passes{
      PassJson{.name = "resolve-refs", .version = "v1"},
      PassJson{.name = "expand-dirs", .version = "v1"},
      PassJson{.name = "compose-views", .version = "v1"},
      PassJson{.name = "pack-file-tree", .version = "v2"},
  };

  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">("memix.view.derivation.basis.v2");
  builder.append_comma();
  builder.append_key_value<"ir_hash">(ir_hash);
  builder.append_comma();
  builder.append_key_value<"target">(memix::to_string(target));
  builder.append_comma();
  builder.append_key_value<"passes">(passes);
  builder.append_comma();
  builder.append_key_value<"resolved_inputs">(inputs);
  builder.append_comma();
  builder.append_key_value<"resolved_view_inputs">(environment_inputs);
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

std::vector<ResolvedEnvironmentInputJson> resolved_environment_input_json(
    const std::vector<memix::ResolvedViewEnvironmentInput> &inputs) {
  return inputs
       | std::views::transform(
             [](const memix::ResolvedViewEnvironmentInput &input) {
               return ResolvedEnvironmentInputJson{
                   .environment = input.environment_id,
                   .derivation = input.derivation_id,
                   .artifact = input.artifact_id,
                   .output = input.output_role,
                   .mount = input.mount.generic_string(),
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

std::string sha256_text(std::string_view text) {
  return memix::detail::sha256_text(text);
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
  for (fs::recursive_directory_iterator iterator{
           root, std::filesystem::directory_options::none, iterator_error};
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

ViewVoid apply_writable_permissions(const fs::path &path,
                                    const fs::file_status &status) {
  fs::perms permissions = fs::perms::owner_read | fs::perms::owner_write;
  if (std::filesystem::is_directory(status)) {
    permissions |= fs::perms::owner_exec;
  }
  const memix::FsVoid applied =
      fs::permissions(path, permissions, fs::perm_options::add);
  if (!applied) {
    return view_error(applied.error());
  }
  return {};
}

template <typename Visitor>
ViewVoid visit_tree_without_following_symlinks(const fs::path &root,
                                               const Visitor &visitor) {
  memix::FsResult<fs::file_status> root_status = fs::symlink_status(root);
  if (!root_status) {
    if (root_status.error().code == std::errc::no_such_file_or_directory) {
      return {};
    }
    return view_error(root_status.error());
  }
  if (!std::filesystem::exists(*root_status)
      || std::filesystem::is_symlink(*root_status)) {
    return {};
  }
  const ViewVoid root_visited = visitor(root, *root_status);
  if (!root_visited) {
    return std::unexpected(root_visited.error());
  }
  if (!std::filesystem::is_directory(*root_status)) {
    return {};
  }

  std::error_code iterator_error;
  const fs::recursive_directory_iterator end;
  for (fs::recursive_directory_iterator iterator{root, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    const fs::file_status status = entry.symlink_status(entry_error);
    if (entry_error) {
      return view_error(
          fs::make_fs_error("failed to inspect", entry_error, entry.path()));
    }
    if (std::filesystem::is_symlink(status)) {
      iterator.disable_recursion_pending();
      continue;
    }
    const ViewVoid visited = visitor(entry.path(), status);
    if (!visited) {
      return std::unexpected(visited.error());
    }
  }
  if (iterator_error) {
    return view_error(
        fs::make_fs_error("failed to list directory", iterator_error, root));
  }
  return {};
}

ViewVoid validate_removal_tree_hard_links(const fs::path &root) {
  return visit_tree_without_following_symlinks(
      root,
      [](const fs::path &path, const fs::file_status &status) -> ViewVoid {
        if (!std::filesystem::is_regular_file(status)) {
          return {};
        }
        std::error_code error;
        const std::uintmax_t count =
            std::filesystem::hard_link_count(path, error);
        if (error) {
          return view_error(fs::make_fs_error(
              "failed to inspect hard link count for", error, path));
        }
        if (count > 1) {
          return view_error(
              memix::ErrorKind::Conflict,
              std::format("refusing to change permissions for multiply linked "
                          "activation file `{}`",
                          path.string()));
        }
        return {};
      });
}

ViewVoid make_tree_writable_for_removal(const fs::path &root) {
  const ViewVoid links_checked = validate_removal_tree_hard_links(root);
  if (!links_checked) {
    return std::unexpected(links_checked.error());
  }
  return visit_tree_without_following_symlinks(
      root, [](const fs::path &path, const fs::file_status &status) {
        return apply_writable_permissions(path, status);
      });
}

void remove_tree_best_effort(const fs::path &path) {
  std::ignore = make_tree_writable_for_removal(path);
  std::ignore = fs::remove_all(path);
}

ViewResult<std::uintmax_t> remove_writable_tree(const fs::path &path) {
  const ViewVoid writable = make_tree_writable_for_removal(path);
  if (!writable) {
    return std::unexpected(writable.error());
  }
  memix::FsResult<std::uintmax_t> removed = fs::remove_all(path);
  if (!removed) {
    return view_error(removed.error());
  }
  return *removed;
}

ViewVoid finalize_published_text_file(const fs::path &temp_dir,
                                      const fs::path &temp_file,
                                      const fs::path &published_file) {
  const memix::FsVoid writable =
      fs::permissions(temp_file, fs::perms::owner_read | fs::perms::owner_write,
                      fs::perm_options::add);
  if (!writable) {
    // Publication is already complete and remains read-only. Leaving the
    // temporary hard link is safer than changing the published file's mode.
    return {};
  }

  const memix::FsResult<bool> temp_removed = fs::remove(temp_file);
  if (!temp_removed || !*temp_removed) {
    const memix::FsVoid frozen =
        fs::apply_read_only_file_permissions(published_file);
    if (!frozen) {
      return view_error(frozen.error());
    }
    return {};
  }

  const memix::FsVoid frozen =
      fs::apply_read_only_file_permissions(published_file);
  if (!frozen) {
    auto freeze_error =
        memix::detail::translate_error<memix::ViewError>(frozen.error());
    const memix::FsResult<bool> rolled_back = fs::remove(published_file);
    if (!rolled_back || !*rolled_back) {
      const std::string rollback_message =
          rolled_back ? "published file was already missing"
                      : rolled_back.error().message;
      freeze_error.message = std::format(
          "{}; additionally failed to roll back published file `{}`: {}",
          freeze_error.message, published_file.string(), rollback_message);
    }
    return std::unexpected(std::move(freeze_error));
  }
  std::ignore = fs::remove(temp_dir);
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

fs::path quests_dir_for(const fs::path &root) {
  return views_dir_for(root) / std::string(kQuestsDir);
}

fs::path quest_dir_for(const fs::path &root, std::string_view ir_hash) {
  return quests_dir_for(root) / std::string(ir_hash);
}

fs::path
derivation_dir_for(const fs::path &root, std::string_view derivation_id) {
  return derivations_dir_for(root) / std::string(derivation_id);
}

fs::path
environment_path_for(const fs::path &root, std::string_view environment_id) {
  return environments_dir_for(root)
       / std::format("{}{}", environment_id, kJsonSuffix);
}

ViewResult<fs::path>
unique_temp_dir(const fs::path &parent, std::string_view prefix) {
  memix::FsResult<fs::path> temp_dir = fs::unique_temp_dir(
      parent,
      fs::TempDirOptions{
          .prefix = prefix,
          .failure_message = "failed to allocate a temporary view directory",
      });
  if (!temp_dir) {
    return view_error(std::move(temp_dir.error()));
  }
  return ViewResult<fs::path>{std::in_place, std::move(*temp_dir)};
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

ViewResult<bool>
write_text_file_if_absent(const fs::path &path, std::string_view contents) {
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
  const memix::scope_exit cleanup(
      [temp_path] { std::ignore = fs::remove_all(temp_path); });

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

  const ViewVoid finalized =
      finalize_published_text_file(temp_path, temp_file, path);
  if (!finalized) {
    return std::unexpected(finalized.error());
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
resolve_input(const memix::Store &store, const memix::ViewIrInput &input,
              std::string_view generation) {
  std::vector<memix::ResolvedViewFile> files;
  if (!input.recursive) {
    memix::StoreResult<memix::StoredObject> blob =
        store.read_file(input.type, input.path, generation);
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
        .source_generation = std::string(generation),
        .output_path = output_path_for(input.type, input.path),
        .blob = std::move(*blob),
    });
    return files;
  }

  memix::StoreResult<std::vector<memix::StoreFile>> source_files =
      store.list_files(input.type, generation);
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
        .source_generation = std::string(generation),
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

struct ResolvedBuildInputs {
  std::vector<memix::ResolvedViewFile> files;
  std::vector<memix::ResolvedViewEnvironmentInput> environment_inputs;
};

ViewVoid resolve_context_input(const memix::Store &store,
                               const memix::ViewIrInput &input,
                               ResolvedBuildInputs &resolved) {
  ViewResult<std::string> generation =
      resolve_source_generation(store, input);
  if (!generation) {
    return std::unexpected(generation.error());
  }
  ViewResult<std::vector<memix::ResolvedViewFile>> input_files =
      resolve_input(store, input, *generation);
  if (!input_files) {
    return std::unexpected(input_files.error());
  }
  std::ranges::move(*input_files, std::back_inserter(resolved.files));
  return {};
}

ViewResult<ResolvedBuildInputs>
resolve_inputs(const memix::Store &store, const memix::ViewIr &ir) {
  ResolvedBuildInputs resolved;
  for (const memix::ViewIrInput &input : ir.inputs) {
    if (input.kind == memix::ViewInputKind::Environment) {
      ViewResult<memix::ViewEnvironment> environment =
          memix::ViewService(store).resolve_environment(input.environment_id);
      if (!environment) {
        return std::unexpected(environment.error());
      }
      if (std::ranges::contains(environment->closure.environments,
                                environment->id)) {
        return view_error(
            memix::ErrorKind::InvalidData,
            std::format("view environment `{}` contains a dependency cycle",
                        environment->id));
      }
      const auto output =
          std::ranges::find(environment->derivation.outputs, input.output_role,
                            &memix::ViewDerivationOutput::role);
      if (output == environment->derivation.outputs.end()) {
        return view_error(
            memix::ErrorKind::InvalidData,
            std::format("view environment `{}` has no `{}` output",
                        environment->id, input.output_role));
      }
      resolved.environment_inputs.push_back(memix::ResolvedViewEnvironmentInput{
          .environment_id = environment->id,
          .derivation_id = environment->derivation.id,
          .artifact_id = output->artifact,
          .output_role = input.output_role,
          .mount = input.mount,
          .closure = environment->closure,
      });
      for (const memix::ResolvedViewFile &upstream_file :
           environment->derivation.resolved_files) {
        memix::ResolvedViewFile file = upstream_file;
        file.output_path = input.mount / upstream_file.output_path;
        resolved.files.push_back(std::move(file));
      }
      continue;
    }
    const ViewVoid context_resolved =
        resolve_context_input(store, input, resolved);
    if (!context_resolved) {
      return std::unexpected(context_resolved.error());
    }
  }

  std::ranges::sort(resolved.files, [](const memix::ResolvedViewFile &lhs,
                                       const memix::ResolvedViewFile &rhs) {
    const auto lhs_key =
        std::tuple{lhs.output_path.generic_string(), lhs.blob.id, lhs.locator};
    const auto rhs_key =
        std::tuple{rhs.output_path.generic_string(), rhs.blob.id, rhs.locator};
    return lhs_key < rhs_key;
  });

  std::vector<memix::ResolvedViewFile> unique_files;
  for (memix::ResolvedViewFile &file : resolved.files) {
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
  resolved.files = std::move(unique_files);
  return resolved;
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

std::string
artifact_id_for(std::string_view derivation_id, std::string_view role) {
  return sha256_text(std::format("{}:{}", role, derivation_id));
}

ViewResult<std::vector<memix::ViewDerivationOutput>>
outputs_for_derivation(std::string_view derivation_id,
                       memix::ViewTargetKind target) {
  if (target != memix::ViewTargetKind::FileTree) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      "only file_tree target builds are supported in v1");
  }

  std::string tree_artifact = artifact_id_for(derivation_id, "file_tree");
  std::string manifest_artifact =
      artifact_id_for(derivation_id, "provenance_manifest");
  return std::vector<memix::ViewDerivationOutput>{
      memix::ViewDerivationOutput{
          .role = "file_tree",
          .artifact = std::move(tree_artifact),
          .path = fs::path(kTreeDir),
      },
      memix::ViewDerivationOutput{
          .role = "provenance_manifest",
          .artifact = std::move(manifest_artifact),
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

std::string derivation_manifest_without_created_at(std::string_view manifest) {
  std::string normalized(manifest);
  constexpr std::string_view key = R"(,"created_at":")";
  const std::size_t begin = normalized.find(key);
  if (begin == std::string::npos) {
    return normalized;
  }
  const std::size_t value_begin = begin + key.size();
  const std::size_t value_end = normalized.find('"', value_begin);
  if (value_end != std::string::npos) {
    normalized.erase(begin, value_end - begin + 1);
  }
  return normalized;
}

ViewVoid
validate_existing_derivation_manifest(const fs::path &path,
                                      std::string_view expected_manifest) {
  const memix::FsResult<std::string> existing =
      fs::read_text_file(path, "view derivation manifest");
  if (!existing) {
    return view_error(existing.error());
  }
  if (derivation_manifest_without_created_at(*existing)
      != derivation_manifest_without_created_at(expected_manifest)) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("immutable view derivation manifest differs `{}`",
                    path.string()));
  }
  return {};
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

ViewResult<bool>
handle_derivation_rename_failure(const fs::path &temp_dir,
                                 const memix::ViewDerivation &derivation,
                                 std::string_view manifest_json,
                                 const memix::FsError &rename_error) {
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

  const ViewVoid validated = validate_existing_derivation_manifest(
      derivation.manifest_path, manifest_json);
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
    const ViewVoid validated = validate_existing_derivation_manifest(
        derivation.manifest_path, manifest_json);
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
  memix::scope_exit cleanup(
      [temp_path] { std::ignore = fs::remove_all(temp_path); });
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
  builder.append_key_value<"schema">(derivation.environment_inputs.empty()
                                         ? "memix.view.environment.basis.v1"
                                         : "memix.view.environment.basis.v2");
  builder.append_comma();
  builder.append_key_value<"bindings">(bindings);
  builder.append_comma();
  builder.append_key_value<"derivation">(derivation.id);
  builder.end_object();
  return json_from_builder(builder);
}

memix::ViewEnvironmentClosure
closure_for(const memix::ViewDerivation &derivation) {
  const auto sort_unique = [](std::vector<std::string> &values) {
    std::ranges::sort(values);
    values.erase(std::ranges::unique(values).begin(), values.end());
  };
  std::vector<std::string> environments;
  std::vector<std::string> derivations{derivation.id};
  std::vector<std::string> artifacts;
  artifacts.reserve(derivation.outputs.size());
  for (const memix::ViewDerivationOutput &output : derivation.outputs) {
    artifacts.push_back(output.artifact);
  }
  std::vector<std::string> raw_objects =
      unique_blob_ids(derivation.resolved_files);
  for (const memix::ResolvedViewEnvironmentInput &input :
       derivation.environment_inputs) {
    environments.push_back(input.environment_id);
    environments.insert(environments.end(), input.closure.environments.begin(),
                        input.closure.environments.end());
    derivations.insert(derivations.end(), input.closure.derivations.begin(),
                       input.closure.derivations.end());
    artifacts.insert(artifacts.end(), input.closure.artifacts.begin(),
                     input.closure.artifacts.end());
    raw_objects.insert(raw_objects.end(), input.closure.raw_objects.begin(),
                       input.closure.raw_objects.end());
  }
  sort_unique(environments);
  sort_unique(derivations);
  sort_unique(artifacts);
  sort_unique(raw_objects);
  return memix::ViewEnvironmentClosure{
      .derivations = std::move(derivations),
      .artifacts = std::move(artifacts),
      .raw_objects = std::move(raw_objects),
      .environments = std::move(environments),
  };
}

ViewResult<memix::ViewEnvironment>
environment_for_derivation(const memix::Store &store,
                           memix::ViewDerivation derivation) {
  ViewResult<std::string> basis = environment_basis_json_for(derivation);
  if (!basis) {
    return std::unexpected(basis.error());
  }
  memix::ViewEnvironment environment;
  environment.id = sha256_text(*basis);
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
  if (std::ranges::contains(environment.closure.environments, environment.id)) {
    return view_error(memix::ErrorKind::Conflict,
                      "view environment composition would create a cycle");
  }
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
read_string_array(const JsonObjectReader &fields, std::string_view key) {
  memix::detail::CommonResult<std::vector<std::string>> values =
      fields.string_array(key);
  if (!values) {
    return view_error(std::move(values.error()), memix::ErrorKind::InvalidData);
  }
  return ViewResult<std::vector<std::string>>{std::in_place,
                                              std::move(*values)};
}

ViewResult<std::string>
read_string_field(const JsonObjectReader &fields, std::string_view key) {
  memix::detail::CommonResult<std::string> value = fields.require_string(key);
  if (!value) {
    return view_error(std::move(value.error()), memix::ErrorKind::InvalidData);
  }
  return ViewResult<std::string>{std::in_place, std::move(*value)};
}

ViewResult<memix::MemoryObjectType>
read_memory_object_type(const JsonObjectReader &fields, std::string_view key) {
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
  memix::detail::CommonResult<simdjson::dom::object> fields_result =
      memix::detail::json_object_from_element(output_element);
  if (!fields_result) {
    return view_error(memix::detail::with_error_context(
        std::move(fields_result.error()), "derivation output",
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields = JsonObjectReader(*fields_result);
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
derivation_outputs_from_json(const JsonObjectReader &fields) {
  memix::detail::CommonResult<simdjson::dom::array> outputs_result =
      fields.require_array("outputs");
  if (!outputs_result) {
    return view_error(std::move(outputs_result.error()),
                      memix::ErrorKind::InvalidData);
  }
  std::vector<memix::ViewDerivationOutput> parsed;
  for (const simdjson::dom::element output_element : *outputs_result) {
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
  memix::detail::CommonResult<simdjson::dom::object> fields_result =
      memix::detail::json_object_from_element(input_element);
  if (!fields_result) {
    return view_error(memix::detail::with_error_context(
        std::move(fields_result.error()), "resolved input",
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields = JsonObjectReader(*fields_result);
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
                         const JsonObjectReader &fields) {
  memix::detail::CommonResult<simdjson::dom::array> inputs_result =
      fields.require_array("resolved_inputs");
  if (!inputs_result) {
    return view_error(std::move(inputs_result.error()),
                      memix::ErrorKind::InvalidData);
  }
  std::vector<memix::ResolvedViewFile> files;
  for (const simdjson::dom::element input_element : *inputs_result) {
    ViewResult<memix::ResolvedViewFile> file =
        resolved_file_from_json(store, input_element);
    if (!file) {
      return std::unexpected(file.error());
    }
    files.push_back(std::move(*file));
  }
  return files;
}

ViewResult<memix::ViewEnvironment>
environment_from_json(std::string_view text, const fs::path &manifest_path);

ViewResult<std::vector<memix::ResolvedViewEnvironmentInput>>
resolved_environment_inputs_from_json(const memix::Store &store,
                                      const JsonObjectReader &fields,
                                      bool required) {
  memix::detail::CommonResult<std::optional<simdjson::dom::array>>
      inputs_result = fields.optional_array("resolved_view_inputs");
  if (!inputs_result) {
    return view_error(std::move(inputs_result.error()),
                      memix::ErrorKind::InvalidData);
  }
  if (!inputs_result->has_value()) {
    if (required) {
      return view_error(memix::ErrorKind::InvalidData,
                        "v2 derivation is missing resolved_view_inputs");
    }
    return std::vector<memix::ResolvedViewEnvironmentInput>{};
  }

  std::vector<memix::ResolvedViewEnvironmentInput> inputs;
  for (const simdjson::dom::element input_element : **inputs_result) {
    memix::detail::CommonResult<simdjson::dom::object> fields_result =
        memix::detail::json_object_from_element(input_element);
    if (!fields_result) {
      return view_error(memix::detail::with_error_context(
          std::move(fields_result.error()), "resolved view input",
          memix::ErrorKind::InvalidData));
    }
    const JsonObjectReader input_fields = JsonObjectReader(*fields_result);
    ViewResult<std::string> environment =
        read_string_field(input_fields, "environment");
    ViewResult<std::string> derivation =
        read_string_field(input_fields, "derivation");
    ViewResult<std::string> artifact =
        read_string_field(input_fields, "artifact");
    ViewResult<std::string> output = read_string_field(input_fields, "output");
    ViewResult<std::string> mount = read_string_field(input_fields, "mount");
    if (!environment) {
      return std::unexpected(environment.error());
    }
    if (!derivation) {
      return std::unexpected(derivation.error());
    }
    if (!artifact) {
      return std::unexpected(artifact.error());
    }
    if (!output) {
      return std::unexpected(output.error());
    }
    if (!mount) {
      return std::unexpected(mount.error());
    }
    const fs::path environment_path =
        environment_path_for(store.root(), *environment);
    memix::FsResult<std::string> environment_text =
        fs::read_text_file(environment_path, "view environment");
    if (!environment_text) {
      return view_error(environment_text.error());
    }
    ViewResult<memix::ViewEnvironment> parsed_environment =
        environment_from_json(*environment_text, environment_path);
    if (!parsed_environment) {
      return std::unexpected(parsed_environment.error());
    }
    const bool has_binding =
        std::ranges::any_of(parsed_environment->bindings,
                            [&](const memix::ViewEnvironmentBinding &binding) {
                              return binding.derivation_id == *derivation
                                  && binding.output_role == *output;
                            });
    if (!has_binding
        || !std::ranges::contains(parsed_environment->closure.artifacts,
                                  *artifact)) {
      return view_error(
          memix::ErrorKind::InvalidData,
          std::format("resolved view input `{}` has inconsistent provenance",
                      *environment));
    }
    inputs.push_back(memix::ResolvedViewEnvironmentInput{
        .environment_id = std::move(*environment),
        .derivation_id = std::move(*derivation),
        .artifact_id = std::move(*artifact),
        .output_role = std::move(*output),
        .mount = fs::path(std::move(*mount)),
        .closure = std::move(parsed_environment->closure),
    });
  }
  return inputs;
}

ViewResult<memix::ViewDerivation>
derivation_from_json(const memix::Store &store, std::string_view text,
                     const fs::path &manifest_path) {
  const std::string json_context =
      std::format("invalid derivation JSON `{}`", manifest_path.string());
  memix::detail::CommonResult<memix::detail::JsonDocument> document =
      memix::detail::parse_json_object(text);
  if (!document) {
    return view_error(memix::detail::with_error_context(
        std::move(document.error()), json_context,
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields = JsonObjectReader(document->root_object());

  ViewResult<std::string> schema = read_string_field(fields, "schema");
  ViewResult<std::string> derivation_id =
      read_string_field(fields, "derivation_id");
  ViewResult<std::string> ir_hash = read_string_field(fields, "ir_hash");
  ViewResult<std::string> target_name = read_string_field(fields, "target");
  ViewResult<std::optional<std::string>> created_at =
      optional_string(fields, "created_at");
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
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  const bool v2 = *schema == kDerivationSchemaV2;
  if (*schema != kDerivationSchema && !v2) {
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
  ViewResult<std::vector<memix::ResolvedViewEnvironmentInput>>
      environment_inputs =
          resolved_environment_inputs_from_json(store, fields, v2);
  if (!environment_inputs) {
    return std::unexpected(environment_inputs.error());
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
      .environment_inputs = std::move(*environment_inputs),
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
  const std::string json_context =
      std::format("invalid environment JSON `{}`", manifest_path.string());
  memix::detail::CommonResult<memix::detail::JsonDocument> document =
      memix::detail::parse_json_object(text);
  if (!document) {
    return view_error(memix::detail::with_error_context(
        std::move(document.error()), json_context,
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields = JsonObjectReader(document->root_object());
  ViewResult<std::string> schema = read_string_field(fields, "schema");
  if (!schema) {
    return std::unexpected(schema.error());
  }
  const bool v2 = *schema == kEnvironmentSchemaV2;
  if (*schema != kEnvironmentSchema && !v2) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported environment schema `{}`", *schema));
  }
  ViewResult<std::string> environment_id =
      read_string_field(fields, "environment_id");
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

  memix::detail::CommonResult<simdjson::dom::array> bindings =
      fields.require_array("bindings");
  if (!bindings) {
    return view_error(std::move(bindings.error()),
                      memix::ErrorKind::InvalidData);
  }
  for (const simdjson::dom::element binding_element : *bindings) {
    memix::detail::CommonResult<simdjson::dom::object> binding_fields_result =
        memix::detail::json_object_from_element(binding_element);
    if (!binding_fields_result) {
      return view_error(memix::detail::with_error_context(
          std::move(binding_fields_result.error()), "environment binding",
          memix::ErrorKind::InvalidData));
    }
    const JsonObjectReader binding_fields =
        JsonObjectReader(*binding_fields_result);
    ViewResult<std::string> mount = read_string_field(binding_fields, "mount");
    ViewResult<std::string> derivation =
        read_string_field(binding_fields, "derivation");
    ViewResult<std::string> output =
        read_string_field(binding_fields, "output");
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

  memix::detail::CommonResult<simdjson::dom::object> closure_fields =
      fields.require_object("closure");
  if (!closure_fields) {
    return view_error(std::move(closure_fields.error()),
                      memix::ErrorKind::InvalidData);
  }
  const JsonObjectReader closure = JsonObjectReader(*closure_fields);
  std::vector<std::string> environment_dependencies;
  if (v2) {
    ViewResult<std::vector<std::string>> environments =
        read_string_array(closure, "environments");
    if (!environments) {
      return std::unexpected(environments.error());
    }
    environment_dependencies = std::move(*environments);
  }
  ViewResult<std::vector<std::string>> derivations =
      read_string_array(closure, "derivations");
  ViewResult<std::vector<std::string>> artifacts =
      read_string_array(closure, "artifacts");
  ViewResult<std::vector<std::string>> raw_objects =
      read_string_array(closure, "raw_objects");
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
      .environments = std::move(environment_dependencies),
  };
  return environment;
}

// ---------------------------------------------------------------------------
// Activation (Nix-profile-style generations over built environments)
// ---------------------------------------------------------------------------

fs::path profiles_dir_for(const fs::path &root) {
  return views_dir_for(root) / std::string(kProfilesDir);
}

fs::path profile_dir_for(const fs::path &root, std::string_view profile) {
  return profiles_dir_for(root) / std::string(profile);
}

fs::path generations_dir_for(const fs::path &profile_dir) {
  return profile_dir / std::string(kGenerationsDir);
}

fs::path current_link_for(const fs::path &profile_dir) {
  return profile_dir / std::string(kCurrentLinkName);
}

fs::path
generation_link_for(const fs::path &profile_dir, std::uint64_t number) {
  return generations_dir_for(profile_dir) / std::format("{}", number);
}

fs::path
generation_metadata_for(const fs::path &profile_dir, std::uint64_t number) {
  return generations_dir_for(profile_dir) / std::format("{}.json", number);
}

ViewVoid check_profile_name(std::string_view profile) {
  const auto allowed = [](char character, bool first) {
    if (character >= 'a' && character <= 'z') {
      return true;
    }
    if (character >= '0' && character <= '9') {
      return true;
    }
    return !first && (character == '.' || character == '_' || character == '-');
  };
  bool valid = !profile.empty() && profile.size() <= kMaxProfileNameLength;
  for (std::size_t index = 0; valid && index < profile.size(); ++index) {
    valid = allowed(profile[index], index == 0);
  }
  if (!valid) {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("profile name `{}` must match "
                                  "[a-z0-9][a-z0-9._-]*",
                                  profile));
  }
  return {};
}

ViewResult<std::string> utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &time) != 0) {
    return view_error("failed to compute UTC timestamp");
  }
#else
  if (gmtime_r(&time, &utc) == nullptr) {
    return view_error("failed to compute UTC timestamp");
  }
#endif
  std::array<char, kTimestampBufferBytes> buffer{};
  if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc)
      == 0U) {
    return view_error("failed to format timestamp");
  }
  return {buffer.data()};
}

struct QuestRecord {
  std::string ir_hash;
  std::string intent;
  std::string name;
  std::string created_at;
};

struct QuestHistoryEntry {
  std::string resolved_at;
  std::string derivation_id;
  std::string environment_id;
};

struct QuestReceipt {
  std::string at;
  std::string operation;
  std::string environment_id;
  std::string derivation_id;
  std::optional<std::string> path;
  std::optional<std::string> profile;
  std::optional<std::uint64_t> generation;
  std::optional<std::string> mode;
  std::optional<std::string> via;
};

struct QuestReceiptSummary {
  std::size_t activation_count = 0;
  std::string first_activated_at;
  std::string last_activated_at;
  std::map<std::string, std::size_t> activations_by_profile;
  std::size_t read_count = 0;
  std::string first_read_at;
  std::string last_read_at;
  std::size_t cat_count = 0;
  std::size_t tree_count = 0;
  std::map<std::string, std::size_t> reads_by_environment;
};

void summarize_quest_receipt(const QuestReceipt &receipt,
                             QuestReceiptSummary &summary) {
  if (receipt.operation == "activate") {
    if (summary.activation_count == 0) {
      summary.first_activated_at = receipt.at;
    }
    ++summary.activation_count;
    summary.last_activated_at = receipt.at;
    if (receipt.profile.has_value()) {
      ++summary.activations_by_profile[*receipt.profile];
    }
    return;
  }
  if (receipt.operation != "cat" && receipt.operation != "tree") {
    return;
  }
  if (summary.read_count == 0) {
    summary.first_read_at = receipt.at;
  }
  ++summary.read_count;
  summary.last_read_at = receipt.at;
  if (receipt.operation == "cat") {
    ++summary.cat_count;
  } else {
    ++summary.tree_count;
  }
  ++summary.reads_by_environment[receipt.environment_id];
}

ViewResult<std::string> quest_json_for(const QuestRecord &quest) {
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">(kQuestSchema);
  builder.append_comma();
  builder.append_key_value<"ir_hash">(quest.ir_hash);
  builder.append_comma();
  builder.append_key_value<"intent">(quest.intent);
  builder.append_comma();
  builder.append_key_value<"name">(quest.name);
  builder.append_comma();
  builder.append_key_value<"created_at">(quest.created_at);
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<QuestRecord>
quest_from_json(std::string_view text, const fs::path &path) {
  memix::detail::CommonResult<memix::detail::JsonDocument> document =
      memix::detail::parse_json_object(text);
  if (!document) {
    return view_error(memix::detail::with_error_context(
        std::move(document.error()),
        std::format("invalid quest JSON `{}`", path.string()),
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields(document->root_object());
  ViewResult<std::string> schema = read_string_field(fields, "schema");
  ViewResult<std::string> ir_hash = read_string_field(fields, "ir_hash");
  ViewResult<std::string> intent = read_string_field(fields, "intent");
  ViewResult<std::string> name = read_string_field(fields, "name");
  ViewResult<std::string> created_at = read_string_field(fields, "created_at");
  if (!schema) {
    return std::unexpected(schema.error());
  }
  if (!ir_hash) {
    return std::unexpected(ir_hash.error());
  }
  if (!intent) {
    return std::unexpected(intent.error());
  }
  if (!name) {
    return std::unexpected(name.error());
  }
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  if (*schema != kQuestSchema) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("unsupported quest schema `{}`", *schema));
  }
  if (!memix::detail::is_lower_hex_sha256(*ir_hash)) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("quest ir_hash is not a lowercase sha256 digest `{}`",
                    *ir_hash));
  }
  return QuestRecord{
      .ir_hash = std::move(*ir_hash),
      .intent = std::move(*intent),
      .name = std::move(*name),
      .created_at = std::move(*created_at),
  };
}

ViewResult<QuestRecord>
read_quest_record(const fs::path &root, std::string_view ir_hash) {
  const fs::path path =
      quest_dir_for(root, ir_hash) / std::string(kQuestManifestFilename);
  memix::FsResult<std::string> text = fs::read_text_file(path, "quest");
  if (!text) {
    return view_error(text.error());
  }
  ViewResult<QuestRecord> quest = quest_from_json(*text, path);
  if (!quest) {
    return std::unexpected(quest.error());
  }
  if (quest->ir_hash != ir_hash) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("quest manifest ir_hash `{}` does not match directory `{}`",
                    quest->ir_hash, ir_hash));
  }
  return quest;
}

ViewVoid
write_mutable_quest_record(const fs::path &root, const QuestRecord &quest) {
  ViewResult<std::string> json = quest_json_for(quest);
  if (!json) {
    return std::unexpected(json.error());
  }
  const fs::path path =
      quest_dir_for(root, quest.ir_hash) / std::string(kQuestManifestFilename);
  const memix::FsVoid written = fs::write_text_file(path, *json);
  if (!written) {
    return view_error(written.error());
  }
  return {};
}

struct QuestMetadataUpdate {
  std::optional<std::string_view> intent;
  std::optional<std::string_view> name;
};

ViewResult<memix::QuestDefinition>
upsert_quest( // NOLINT(readability-function-cognitive-complexity)
    const fs::path &root, const memix::ViewIr &ir,
    const QuestMetadataUpdate &metadata) {
  ViewResult<std::string> ir_json = memix::view_ir_json(ir);
  if (!ir_json) {
    return std::unexpected(ir_json.error());
  }
  const std::string ir_hash = sha256_text(*ir_json);
  const fs::path quests_dir = quests_dir_for(root);
  const ViewVoid quests_created = ensure_directory(quests_dir);
  if (!quests_created) {
    return std::unexpected(quests_created.error());
  }
  const fs::path quest_dir = quest_dir_for(root, ir_hash);
  const memix::FsResult<bool> exists = fs::exists(quest_dir);
  if (!exists) {
    return view_error(exists.error());
  }
  if (!*exists) {
    ViewResult<std::string> created_at = utc_timestamp();
    if (!created_at) {
      return std::unexpected(created_at.error());
    }
    std::string quest_name;
    if (metadata.name.has_value()) {
      quest_name = *metadata.name;
    } else if (!ir.inputs.empty()) {
      quest_name = ir.inputs.front().locator;
    }
    const QuestRecord quest{
        .ir_hash = ir_hash,
        .intent = metadata.intent.has_value() ? std::string(*metadata.intent)
                                              : std::string{},
        .name = std::move(quest_name),
        .created_at = std::move(*created_at),
    };
    ViewResult<std::string> quest_json = quest_json_for(quest);
    if (!quest_json) {
      return std::unexpected(quest_json.error());
    }
    ViewResult<fs::path> temp_dir = unique_temp_dir(quests_dir, ir_hash);
    if (!temp_dir) {
      return std::unexpected(temp_dir.error());
    }
    const fs::path &temp_path = *temp_dir;
    const memix::FsResult<bool> created = fs::create_directory(temp_path);
    if (!created) {
      return view_error(created.error());
    }
    if (!*created) {
      return view_error(std::format("failed to create temporary quest `{}`",
                                    temp_path.string()));
    }
    memix::scope_exit cleanup(
        [temp_path] { std::ignore = fs::remove_all(temp_path); });
    const fs::path ir_path = temp_path / std::string(kQuestIrFilename);
    const memix::FsVoid ir_written = fs::write_text_file(ir_path, *ir_json);
    if (!ir_written) {
      return view_error(ir_written.error());
    }
    const memix::FsVoid ir_frozen =
        fs::apply_read_only_file_permissions(ir_path);
    if (!ir_frozen) {
      return view_error(ir_frozen.error());
    }
    const memix::FsVoid quest_written = fs::write_text_file(
        temp_path / std::string(kQuestManifestFilename), *quest_json);
    if (!quest_written) {
      return view_error(quest_written.error());
    }
    const memix::FsVoid renamed = fs::rename(temp_path, quest_dir);
    if (!renamed) {
      const memix::FsResult<bool> raced_exists = fs::exists(quest_dir);
      if (!raced_exists) {
        return view_error(raced_exists.error());
      }
      if (!*raced_exists) {
        return view_error(renamed.error());
      }
      memix::FsResult<std::string> raced_ir = fs::read_text_file(
          quest_dir / std::string(kQuestIrFilename), "quest IR");
      if (!raced_ir) {
        return view_error(raced_ir.error());
      }
      if (*raced_ir != *ir_json || sha256_text(*raced_ir) != ir_hash) {
        return view_error(
            memix::ErrorKind::InvalidData,
            std::format("persisted quest IR differs `{}`", quest_dir.string()));
      }
    } else {
      cleanup.release();
      return memix::QuestDefinition{
          .ir_hash = ir_hash,
          .quest_dir = quest_dir,
          .existed = false,
      };
    }
  }

  const fs::path ir_path = quest_dir / std::string(kQuestIrFilename);
  memix::FsResult<std::string> existing_ir =
      fs::read_text_file(ir_path, "quest IR");
  if (!existing_ir) {
    return view_error(existing_ir.error());
  }
  if (*existing_ir != *ir_json || sha256_text(*existing_ir) != ir_hash) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("persisted quest IR differs `{}`", ir_path.string()));
  }
  if (!metadata.intent.has_value() && !metadata.name.has_value()) {
    return memix::QuestDefinition{
        .ir_hash = ir_hash,
        .quest_dir = quest_dir,
        .existed = true,
    };
  }
  ViewResult<QuestRecord> quest = read_quest_record(root, ir_hash);
  if (!quest) {
    return std::unexpected(quest.error());
  }
  if (metadata.intent.has_value()) {
    quest->intent = *metadata.intent;
  }
  if (metadata.name.has_value()) {
    quest->name = *metadata.name;
  }
  const ViewVoid written = write_mutable_quest_record(root, *quest);
  if (!written) {
    return std::unexpected(written.error());
  }
  return memix::QuestDefinition{
      .ir_hash = ir_hash,
      .quest_dir = quest_dir,
      .existed = true,
  };
}

ViewResult<std::string> quest_history_json_for(const QuestHistoryEntry &entry) {
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"resolved_at">(entry.resolved_at);
  builder.append_comma();
  builder.append_key_value<"derivation_id">(entry.derivation_id);
  builder.append_comma();
  builder.append_key_value<"environment_id">(entry.environment_id);
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<QuestHistoryEntry>
quest_history_entry_from_json(std::string_view text, const fs::path &path) {
  memix::detail::CommonResult<memix::detail::JsonDocument> document =
      memix::detail::parse_json_object(text);
  if (!document) {
    return view_error(memix::detail::with_error_context(
        std::move(document.error()),
        std::format("invalid quest history `{}`", path.string()),
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields(document->root_object());
  ViewResult<std::string> resolved_at =
      read_string_field(fields, "resolved_at");
  ViewResult<std::string> derivation_id =
      read_string_field(fields, "derivation_id");
  ViewResult<std::string> environment_id =
      read_string_field(fields, "environment_id");
  if (!resolved_at) {
    return std::unexpected(resolved_at.error());
  }
  if (!derivation_id) {
    return std::unexpected(derivation_id.error());
  }
  if (!environment_id) {
    return std::unexpected(environment_id.error());
  }
  if (!memix::detail::is_lower_hex_sha256(*derivation_id)
      || !memix::detail::is_lower_hex_sha256(*environment_id)) {
    return view_error(memix::ErrorKind::InvalidData,
                      "quest history contains an invalid object id");
  }
  return QuestHistoryEntry{
      .resolved_at = std::move(*resolved_at),
      .derivation_id = std::move(*derivation_id),
      .environment_id = std::move(*environment_id),
  };
}

ViewResult<std::vector<QuestHistoryEntry>>
read_quest_history(const fs::path &root, std::string_view ir_hash) {
  const fs::path path =
      quest_dir_for(root, ir_hash) / std::string(kQuestHistoryFilename);
  const memix::FsResult<bool> exists = fs::exists(path);
  if (!exists) {
    return view_error(exists.error());
  }
  if (!*exists) {
    return std::vector<QuestHistoryEntry>{};
  }
  memix::FsResult<std::string> text = fs::read_text_file(path, "quest history");
  if (!text) {
    return view_error(text.error());
  }
  std::vector<QuestHistoryEntry> history;
  std::string_view remaining = *text;
  while (!remaining.empty()) {
    const std::size_t end = remaining.find('\n');
    const std::string_view line = remaining.substr(0, end);
    if (!line.empty()) {
      ViewResult<QuestHistoryEntry> entry =
          quest_history_entry_from_json(line, path);
      if (!entry) {
        return std::unexpected(entry.error());
      }
      history.push_back(std::move(*entry));
    }
    if (end == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(end + 1);
  }
  return history;
}

ViewVoid
record_resolution(const fs::path &root, const memix::ViewDerivation &derivation,
                  const memix::ViewEnvironment &environment) {
  ViewResult<std::string> resolved_at = utc_timestamp();
  if (!resolved_at) {
    return std::unexpected(resolved_at.error());
  }
  const QuestHistoryEntry entry{
      .resolved_at = std::move(*resolved_at),
      .derivation_id = derivation.id,
      .environment_id = environment.id,
  };
  ViewResult<std::string> json = quest_history_json_for(entry);
  if (!json) {
    return std::unexpected(json.error());
  }
  const fs::path path = quest_dir_for(root, derivation.ir_hash)
                      / std::string(kQuestHistoryFilename);
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    return view_error(
        std::format("failed to open quest history `{}`", path.string()));
  }
  std::string line = std::move(*json);
  line.push_back('\n');
  if (line.size()
      > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    return view_error(
        std::format("quest history entry is too large `{}`", path.string()));
  }
  output.write(line.data(), static_cast<std::streamsize>(line.size()));
  if (!output) {
    return view_error(
        std::format("failed to append quest history `{}`", path.string()));
  }
  return {};
}

ViewResult<std::string> quest_receipt_json_for(const QuestReceipt &receipt) {
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"at">(receipt.at);
  builder.append_comma();
  builder.append_key_value<"operation">(receipt.operation);
  builder.append_comma();
  builder.append_key_value<"environment_id">(receipt.environment_id);
  builder.append_comma();
  builder.append_key_value<"derivation_id">(receipt.derivation_id);
  if (receipt.path.has_value()) {
    builder.append_comma();
    builder.append_key_value<"path">(*receipt.path);
  }
  if (receipt.profile.has_value()) {
    builder.append_comma();
    builder.append_key_value<"profile">(*receipt.profile);
  }
  if (receipt.generation.has_value()) {
    builder.append_comma();
    builder.append_key_value<"generation">(*receipt.generation);
  }
  if (receipt.mode.has_value()) {
    builder.append_comma();
    builder.append_key_value<"mode">(*receipt.mode);
  }
  if (receipt.via.has_value()) {
    builder.append_comma();
    builder.append_key_value<"via">(*receipt.via);
  }
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<QuestReceipt>
quest_receipt_from_json(std::string_view text, const fs::path &path) {
  memix::detail::CommonResult<memix::detail::JsonDocument> document =
      memix::detail::parse_json_object(text);
  if (!document) {
    return view_error(memix::detail::with_error_context(
        std::move(document.error()),
        std::format("invalid quest receipt `{}`", path.string()),
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields(document->root_object());
  ViewResult<std::string> at = read_string_field(fields, "at");
  ViewResult<std::string> operation = read_string_field(fields, "operation");
  ViewResult<std::string> environment_id =
      read_string_field(fields, "environment_id");
  ViewResult<std::string> derivation_id =
      read_string_field(fields, "derivation_id");
  ViewResult<std::optional<std::string>> profile =
      optional_string(fields, "profile");
  if (!at) {
    return std::unexpected(at.error());
  }
  if (!operation) {
    return std::unexpected(operation.error());
  }
  if (!environment_id) {
    return std::unexpected(environment_id.error());
  }
  if (!derivation_id) {
    return std::unexpected(derivation_id.error());
  }
  if (!profile) {
    return std::unexpected(profile.error());
  }
  if (!memix::detail::is_lower_hex_sha256(*environment_id)
      || !memix::detail::is_lower_hex_sha256(*derivation_id)) {
    return view_error(memix::ErrorKind::InvalidData,
                      "quest receipt contains an invalid object id");
  }
  return QuestReceipt{
      .at = std::move(*at),
      .operation = std::move(*operation),
      .environment_id = std::move(*environment_id),
      .derivation_id = std::move(*derivation_id),
      .path = std::nullopt,
      .profile = std::move(*profile),
      .generation = std::nullopt,
      .mode = std::nullopt,
      .via = std::nullopt,
  };
}

ViewResult<QuestReceiptSummary>
read_quest_receipt_summary(const fs::path &root, std::string_view ir_hash) {
  const fs::path path =
      quest_dir_for(root, ir_hash) / std::string(kQuestReceiptsFilename);
  const memix::FsResult<bool> exists = fs::exists(path);
  if (!exists) {
    return view_error(exists.error());
  }
  if (!*exists) {
    return QuestReceiptSummary{};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return view_error(
        std::format("failed to open quest receipts `{}`", path.string()));
  }
  QuestReceiptSummary summary;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    ViewResult<QuestReceipt> receipt = quest_receipt_from_json(line, path);
    if (!receipt) {
      return std::unexpected(receipt.error());
    }
    summarize_quest_receipt(*receipt, summary);
  }
  if (!input.eof()) {
    return view_error(
        std::format("failed to read quest receipts `{}`", path.string()));
  }
  return summary;
}

void record_quest_receipt_best_effort(const fs::path &root,
                                      const memix::ViewEnvironment &environment,
                                      QuestReceipt receipt) {
  const fs::path quest_dir =
      quest_dir_for(root, environment.derivation.ir_hash);
  const memix::FsResult<bool> exists = fs::exists(quest_dir);
  if (!exists || !*exists) {
    return;
  }
  ViewResult<std::string> at = utc_timestamp();
  if (!at) {
    return;
  }
  receipt.at = std::move(*at);
  receipt.environment_id = environment.id;
  receipt.derivation_id = environment.derivation.id;
  ViewResult<std::string> json = quest_receipt_json_for(receipt);
  if (!json) {
    return;
  }
  const fs::path path = quest_dir / std::string(kQuestReceiptsFilename);
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    return;
  }
  std::string line = std::move(*json);
  line.push_back('\n');
  if (line.size()
      > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    return;
  }
  output.write(line.data(), static_cast<std::streamsize>(line.size()));
}

ViewResult<std::string>
resolve_unique_quest_id(const fs::path &root, std::string_view prefix) {
  if (!memix::detail::is_lower_hex_prefix(prefix)) {
    return view_error(
        memix::ErrorKind::InvalidArgument,
        std::format("quest id prefix must be lowercase hex `{}`", prefix));
  }
  const fs::path quests_dir = quests_dir_for(root);
  const memix::FsResult<bool> exists = fs::exists(quests_dir);
  if (!exists) {
    return view_error(exists.error());
  }
  if (!*exists) {
    return view_error(memix::ErrorKind::NotFound,
                      std::format("no quest matches `{}`", prefix));
  }
  std::vector<std::string> matches;
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{quests_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    if (!entry.is_directory(entry_error)) {
      if (entry_error) {
        return view_error(
            fs::make_fs_error("failed to inspect", entry_error, entry.path()));
      }
      continue;
    }
    std::string name = entry.path().filename().string();
    if (memix::detail::is_lower_hex_sha256(name) && name.starts_with(prefix)) {
      matches.push_back(std::move(name));
    }
  }
  if (iterator_error) {
    return view_error(fs::make_fs_error("failed to list directory",
                                        iterator_error, quests_dir));
  }
  if (matches.empty()) {
    return view_error(memix::ErrorKind::NotFound,
                      std::format("no quest matches `{}`", prefix));
  }
  if (matches.size() > 1) {
    return view_error(memix::ErrorKind::Conflict,
                      std::format("quest prefix `{}` is ambiguous", prefix));
  }
  return matches.front();
}

ViewResult<memix::ViewIr>
load_quest_ir(const fs::path &root, std::string_view ir_hash) {
  const fs::path path =
      quest_dir_for(root, ir_hash) / std::string(kQuestIrFilename);
  memix::FsResult<std::string> text = fs::read_text_file(path, "quest IR");
  if (!text) {
    return view_error(text.error());
  }
  if (sha256_text(*text) != ir_hash) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("quest IR hash does not match directory `{}`",
                                  path.string()));
  }
  const std::string context =
      std::format("invalid quest IR `{}`", path.string());
  return ir_from_json({
      .text = *text,
      .context = context,
  });
}

ViewResult<std::optional<fs::file_status>> lstat(const fs::path &path) {
  memix::FsResult<fs::file_status> status = fs::symlink_status(path);
  if (!status) {
    if (status.error().code == std::errc::no_such_file_or_directory) {
      return std::optional<fs::file_status>{};
    }
    return view_error(std::move(status.error()));
  }
  return std::optional<fs::file_status>{*status};
}

struct GenerationMetadata {
  std::string profile;
  std::uint64_t number = 0;
  std::string environment_id;
  std::string derivation_id;
  std::string activated_at;
  memix::ActivationMode mode = memix::ActivationMode::Link;
  std::string workspace;
};

bool same_activation_identity(const GenerationMetadata &left,
                              const GenerationMetadata &right) {
  return left.profile == right.profile
      && left.number == right.number
      && left.environment_id == right.environment_id
      && left.derivation_id == right.derivation_id
      && left.activated_at == right.activated_at
      && left.mode == right.mode
      && left.workspace == right.workspace;
}

ViewResult<fs::path> workspace_attach_path(const fs::path &workspace) {
  fs::path parent = workspace.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  memix::FsResult<fs::path> canonical_parent = fs::weakly_canonical(parent);
  if (!canonical_parent) {
    return view_error(canonical_parent.error());
  }
  return (*canonical_parent / workspace.filename()).lexically_normal();
}

ViewResult<fs::path> acquire_workspace_lock(const fs::path &workspace) {
  ViewResult<fs::path> attach_path = workspace_attach_path(workspace);
  if (!attach_path) {
    return std::unexpected(attach_path.error());
  }
  const fs::path parent = attach_path->parent_path();
  const ViewVoid parent_created = ensure_directory(parent);
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }
  fs::path lock = parent
                / std::format("{}{}", kWorkspaceLockPrefix,
                              sha256_text(attach_path->generic_string()));
  const memix::FsResult<bool> acquired = fs::create_directory(lock);
  if (!acquired) {
    return view_error(acquired.error());
  }
  if (!*acquired) {
    return view_error(
        memix::ErrorKind::Locked,
        std::format("workspace `{}` is being modified by another memix "
                    "process; remove `{}` if the lock is stale",
                    workspace.string(), lock.string()));
  }
  return lock;
}

ViewVoid validate_copy_marker_location(const GenerationMetadata &metadata,
                                       const fs::path &into) {
  if (metadata.mode != memix::ActivationMode::Copy) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("activation marker at `{}` records mode `{}`",
                                  into.string(),
                                  memix::to_string(metadata.mode)));
  }
  ViewResult<fs::path> expected_workspace = workspace_attach_path(into);
  if (!expected_workspace) {
    return std::unexpected(expected_workspace.error());
  }
  const fs::path recorded_workspace{metadata.workspace};
  if (!recorded_workspace.is_absolute()
      || recorded_workspace != *expected_workspace) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("activation marker at `{}` records workspace `{}`",
                    into.string(), metadata.workspace));
  }
  return {};
}

ViewResult<std::string>
generation_metadata_json(const GenerationMetadata &metadata,
                         std::string_view schema) {
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">(schema);
  builder.append_comma();
  builder.append_key_value<"profile">(metadata.profile);
  builder.append_comma();
  builder.append_key_value<"generation">(metadata.number);
  builder.append_comma();
  builder.append_key_value<"environment">(metadata.environment_id);
  builder.append_comma();
  builder.append_key_value<"derivation">(metadata.derivation_id);
  builder.append_comma();
  builder.append_key_value<"activated_at">(metadata.activated_at);
  builder.append_comma();
  builder.append_key_value<"mode">(memix::to_string(metadata.mode));
  builder.append_comma();
  builder.append_key_value<"workspace">(metadata.workspace);
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<std::uint64_t>
read_uint64_field(const JsonObjectReader &fields, std::string_view key) {
  memix::detail::CommonResult<std::uint64_t> value = fields.require_uint64(key);
  if (!value) {
    return view_error(std::move(value.error()), memix::ErrorKind::InvalidData);
  }
  return *value;
}

ViewResult<GenerationMetadata>
generation_metadata_from_json(std::string_view text, const fs::path &path,
                              std::string_view expected_schema) {
  const std::string json_context =
      std::format("invalid activation metadata `{}`", path.string());
  memix::detail::CommonResult<memix::detail::JsonDocument> document =
      memix::detail::parse_json_object(text);
  if (!document) {
    return view_error(memix::detail::with_error_context(
        std::move(document.error()), json_context,
        memix::ErrorKind::InvalidData));
  }
  const JsonObjectReader fields = JsonObjectReader(document->root_object());
  ViewResult<std::string> schema = read_string_field(fields, "schema");
  if (!schema) {
    return std::unexpected(schema.error());
  }
  if (*schema != expected_schema) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported activation metadata schema `{}`", *schema));
  }
  ViewResult<std::string> profile = read_string_field(fields, "profile");
  ViewResult<std::uint64_t> number = read_uint64_field(fields, "generation");
  ViewResult<std::string> environment =
      read_string_field(fields, "environment");
  ViewResult<std::string> derivation = read_string_field(fields, "derivation");
  ViewResult<std::string> activated_at =
      read_string_field(fields, "activated_at");
  ViewResult<std::string> mode_name = read_string_field(fields, "mode");
  if (!profile) {
    return std::unexpected(profile.error());
  }
  const ViewVoid profile_checked = check_profile_name(*profile);
  if (!profile_checked) {
    return view_error(memix::ErrorKind::InvalidData,
                      std::format("invalid activation profile `{}`", *profile));
  }
  if (!number) {
    return std::unexpected(number.error());
  }
  if (*number == 0) {
    return view_error(memix::ErrorKind::InvalidData,
                      "activation generation must be greater than zero");
  }
  if (!environment) {
    return std::unexpected(environment.error());
  }
  if (!derivation) {
    return std::unexpected(derivation.error());
  }
  if (!activated_at) {
    return std::unexpected(activated_at.error());
  }
  if (!mode_name) {
    return std::unexpected(mode_name.error());
  }
  const std::optional<memix::ActivationMode> mode =
      memix::parse_activation_mode(*mode_name);
  if (!mode.has_value()) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("unsupported activation mode `{}`", *mode_name));
  }
  ViewResult<std::optional<std::string>> workspace =
      optional_string(fields, "workspace");
  if (!workspace) {
    return std::unexpected(workspace.error());
  }
  return GenerationMetadata{
      .profile = std::move(*profile),
      .number = *number,
      .environment_id = std::move(*environment),
      .derivation_id = std::move(*derivation),
      .activated_at = std::move(*activated_at),
      .mode = *mode,
      .workspace = workspace->value_or(""),
  };
}

ViewResult<GenerationMetadata>
read_generation_metadata(const fs::path &profile_dir, std::uint64_t number) {
  const fs::path path = generation_metadata_for(profile_dir, number);
  memix::FsResult<std::string> text =
      fs::read_text_file(path, "activation generation");
  if (!text) {
    return view_error(text.error());
  }
  ViewResult<GenerationMetadata> metadata =
      generation_metadata_from_json(*text, path, kGenerationSchema);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  const std::string expected_profile = profile_dir.filename().string();
  if (metadata->profile != expected_profile || metadata->number != number) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("activation generation metadata `{}` does not match "
                    "profile `{}` generation {}",
                    path.string(), expected_profile, number));
  }
  return metadata;
}

bool parse_generation_number(std::string_view name, std::uint64_t &number) {
  constexpr std::size_t kMaxDigits = 20;
  constexpr std::uint64_t kDecimalBase = 10;
  if (name.empty() || name.size() > kMaxDigits) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char character : name) {
    if (character < '0' || character > '9') {
      return false;
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value
        > (std::numeric_limits<std::uint64_t>::max() - digit) / kDecimalBase) {
      return false;
    }
    value = (value * kDecimalBase) + digit;
  }
  if (value == 0) {
    return false;
  }
  number = value;
  return true;
}

ViewResult<std::vector<std::uint64_t>>
scan_generations(const fs::path &profile_dir) {
  const fs::path generations_dir = generations_dir_for(profile_dir);
  const memix::FsResult<bool> exists = fs::exists(generations_dir);
  if (!exists) {
    return view_error(exists.error());
  }
  if (!*exists) {
    return std::vector<std::uint64_t>{};
  }
  std::vector<std::uint64_t> numbers;
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{generations_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    if (!entry.is_symlink(entry_error)) {
      if (entry_error) {
        return view_error(
            fs::make_fs_error("failed to inspect", entry_error, entry.path()));
      }
      continue;
    }
    std::uint64_t number = 0;
    if (parse_generation_number(entry.path().filename().string(), number)) {
      numbers.push_back(number);
    }
  }
  if (iterator_error) {
    return view_error(fs::make_fs_error("failed to list directory",
                                        iterator_error, generations_dir));
  }
  std::ranges::sort(numbers);
  return numbers;
}

ViewResult<std::optional<std::uint64_t>>
read_current_generation(const fs::path &profile_dir) {
  const fs::path current = current_link_for(profile_dir);
  ViewResult<std::optional<fs::file_status>> status = lstat(current);
  if (!status) {
    return std::unexpected(status.error());
  }
  if (!status->has_value()) {
    return std::optional<std::uint64_t>{};
  }
  if (!std::filesystem::is_symlink(**status)) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("profile current is not a symlink `{}`", current.string()));
  }
  memix::FsResult<fs::path> target = fs::read_symlink(current);
  if (!target) {
    return view_error(target.error());
  }
  std::uint64_t number = 0;
  if (target->is_absolute()
      || !parse_generation_number(target->filename().string(), number)
      || *target
             != fs::path(std::string(kGenerationsDir))
                    / std::format("{}", number)) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("profile current points at `{}`", target->string()));
  }
  return std::optional<std::uint64_t>{number};
}

ViewVoid acquire_profile_lock(const fs::path &profile_dir) {
  const ViewVoid created = ensure_directory(profile_dir);
  if (!created) {
    return std::unexpected(created.error());
  }
  const fs::path lock = profile_dir / std::string(kProfileLockName);
  const memix::FsResult<bool> acquired = fs::create_directory(lock);
  if (!acquired) {
    return view_error(acquired.error());
  }
  if (!*acquired) {
    return view_error(
        memix::ErrorKind::Locked,
        std::format("profile `{}` is locked by another memix process; "
                    "remove `{}` if the lock is stale",
                    profile_dir.filename().string(), lock.string()));
  }
  return {};
}

ViewVoid acquire_existing_profile_lock(const fs::path &profile_dir) {
  const fs::path lock = profile_dir / std::string(kProfileLockName);
  const memix::FsResult<bool> acquired = fs::create_directory(lock);
  if (!acquired) {
    if (acquired.error().code == std::errc::no_such_file_or_directory) {
      return view_error(memix::ErrorKind::NotFound,
                        std::format("profile `{}` does not exist",
                                    profile_dir.filename().string()));
    }
    return view_error(acquired.error());
  }
  if (!*acquired) {
    return view_error(
        memix::ErrorKind::Locked,
        std::format("profile `{}` is locked by another memix process; "
                    "remove `{}` if the lock is stale",
                    profile_dir.filename().string(), lock.string()));
  }
  return {};
}

ViewVoid swap_current(const fs::path &profile_dir, std::uint64_t number) {
  ViewResult<fs::path> temp =
      unique_temp_dir(profile_dir, std::string(kCurrentLinkName));
  if (!temp) {
    return std::unexpected(temp.error());
  }
  const std::string target = std::format("{}/{}", kGenerationsDir, number);
  const memix::FsVoid linked = fs::create_directory_symlink(target, *temp);
  if (!linked) {
    return view_error(linked.error());
  }
  const memix::FsVoid installed =
      fs::replace_directory_symlink(*temp, current_link_for(profile_dir));
  if (!installed) {
    std::ignore = fs::remove(*temp);
    return view_error(installed.error());
  }
  return {};
}

ViewVoid restore_current(const fs::path &profile_dir,
                         std::optional<std::uint64_t> previous) {
  if (previous.has_value()) {
    return swap_current(profile_dir, *previous);
  }
  const memix::FsResult<bool> removed =
      fs::remove(current_link_for(profile_dir));
  if (!removed) {
    return view_error(removed.error());
  }
  return {};
}

memix::ViewError
restore_current_after_failure(const fs::path &profile_dir,
                              std::optional<std::uint64_t> previous,
                              memix::ViewError original_error) {
  const ViewVoid restored = restore_current(profile_dir, previous);
  if (!restored) {
    original_error.message =
        std::format("{}; additionally failed to restore profile current: {}",
                    original_error.message, restored.error().message);
  }
  return original_error;
}

ViewVoid discard_generation(const fs::path &profile_dir, std::uint64_t number) {
  std::optional<memix::ViewError> removal_error;
  const memix::FsResult<bool> link_removed =
      fs::remove(generation_link_for(profile_dir, number));
  if (!link_removed) {
    removal_error = view_error_from_fs(link_removed.error());
  }
  const fs::path metadata_path = generation_metadata_for(profile_dir, number);
  const ViewVoid metadata_writable =
      make_tree_writable_for_removal(metadata_path);
  if (!metadata_writable) {
    if (removal_error.has_value()) {
      removal_error->message = std::format(
          "{}; additionally failed to make generation metadata writable: {}",
          removal_error->message, metadata_writable.error().message);
    } else {
      removal_error = metadata_writable.error();
    }
  }
  const memix::FsResult<bool> metadata_removed = fs::remove(metadata_path);
  if (!metadata_removed) {
    memix::ViewError metadata_error =
        view_error_from_fs(metadata_removed.error());
    if (removal_error.has_value()) {
      removal_error->message =
          std::format("{}; additionally failed to remove generation metadata: "
                      "{}",
                      removal_error->message, metadata_error.message);
    } else {
      removal_error = std::move(metadata_error);
    }
  }
  if (removal_error.has_value()) {
    return std::unexpected(std::move(*removal_error));
  }
  return {};
}

memix::ViewError
discard_generation_after_failure(const fs::path &profile_dir,
                                 std::uint64_t number,
                                 memix::ViewError original_error) {
  const ViewVoid discarded = discard_generation(profile_dir, number);
  if (!discarded) {
    original_error.message =
        std::format("{}; additionally failed to discard generation {}: {}",
                    original_error.message, number, discarded.error().message);
  }
  return original_error;
}

memix::ViewError
restore_activation_after_failure(const fs::path &profile_dir,
                                 std::optional<std::uint64_t> previous,
                                 std::uint64_t failed_generation,
                                 memix::ViewError original_error) {
  const ViewVoid restored = restore_current(profile_dir, previous);
  if (!restored) {
    original_error.message =
        std::format("{}; additionally failed to restore profile current: {}",
                    original_error.message, restored.error().message);
    return original_error;
  }
  const ViewVoid discarded = discard_generation(profile_dir, failed_generation);
  if (!discarded) {
    original_error.message = std::format(
        "{}; additionally failed to discard generation {}: {}",
        original_error.message, failed_generation, discarded.error().message);
  }
  return original_error;
}

ViewVoid write_generation(const fs::path &profile_dir,
                          const GenerationMetadata &metadata,
                          const fs::path &tree_path) {
  const fs::path generations_dir = generations_dir_for(profile_dir);
  const ViewVoid created = ensure_directory(generations_dir);
  if (!created) {
    return std::unexpected(created.error());
  }
  memix::FsResult<fs::path> relative_target =
      fs::relative(tree_path, generations_dir);
  if (!relative_target) {
    return view_error(relative_target.error());
  }
  const memix::FsVoid linked = fs::create_directory_symlink(
      *relative_target, generation_link_for(profile_dir, metadata.number));
  if (!linked) {
    return view_error(linked.error());
  }
  ViewResult<std::string> json =
      generation_metadata_json(metadata, kGenerationSchema);
  if (!json) {
    return std::unexpected(discard_generation_after_failure(
        profile_dir, metadata.number, std::move(json.error())));
  }
  ViewResult<bool> written = write_text_file_if_absent(
      generation_metadata_for(profile_dir, metadata.number), *json);
  if (!written) {
    return std::unexpected(discard_generation_after_failure(
        profile_dir, metadata.number, std::move(written.error())));
  }
  ViewVoid swapped = swap_current(profile_dir, metadata.number);
  if (!swapped) {
    return std::unexpected(discard_generation_after_failure(
        profile_dir, metadata.number, std::move(swapped.error())));
  }
  return {};
}

struct WorkspaceLinkRequest {
  fs::path into;
  fs::path target;
  bool force = false;
};

ViewVoid preflight_workspace_link(const WorkspaceLinkRequest &request) {
  ViewResult<std::optional<fs::file_status>> status = lstat(request.into);
  if (!status) {
    return std::unexpected(status.error());
  }
  if (!status->has_value()) {
    return {};
  }
  if (!std::filesystem::is_symlink(**status)) {
    return view_error(
        memix::ErrorKind::AlreadyExists,
        std::format("workspace path `{}` already exists and is not a "
                    "symlink; refusing to replace it",
                    request.into.string()));
  }
  memix::FsResult<fs::path> existing = fs::read_symlink(request.into);
  if (!existing) {
    return view_error(existing.error());
  }
  if (*existing == request.target || request.force) {
    return {};
  }
  return view_error(
      memix::ErrorKind::AlreadyExists,
      std::format("workspace path `{}` already links to `{}`; pass --force "
                  "to replace it",
                  request.into.string(), existing->string()));
}

ViewResult<bool> ensure_workspace_link(const WorkspaceLinkRequest &request) {
  ViewResult<std::optional<fs::file_status>> status = lstat(request.into);
  if (!status) {
    return std::unexpected(status.error());
  }
  if (status->has_value()) {
    if (!std::filesystem::is_symlink(**status)) {
      return view_error(
          memix::ErrorKind::AlreadyExists,
          std::format("workspace path `{}` already exists and is not a "
                      "symlink; refusing to replace it",
                      request.into.string()));
    }
    memix::FsResult<fs::path> existing = fs::read_symlink(request.into);
    if (!existing) {
      return view_error(existing.error());
    }
    if (*existing == request.target) {
      return false;
    }
    if (!request.force) {
      return view_error(
          memix::ErrorKind::AlreadyExists,
          std::format("workspace path `{}` already links to `{}`; pass "
                      "--force to replace it",
                      request.into.string(), existing->string()));
    }
  }
  fs::path parent = request.into.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const ViewVoid parent_created = ensure_directory(parent);
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }
  ViewResult<fs::path> temp =
      unique_temp_dir(parent, request.into.filename().string());
  if (!temp) {
    return std::unexpected(temp.error());
  }
  const memix::FsVoid linked =
      fs::create_directory_symlink(request.target, *temp);
  if (!linked) {
    return view_error(linked.error());
  }
  memix::scope_exit cleanup([temp = *temp] { std::ignore = fs::remove(temp); });
  const memix::FsVoid installed =
      fs::replace_directory_symlink(*temp, request.into);
  if (!installed) {
    return view_error(installed.error());
  }
  cleanup.release();
  return true;
}

ViewVoid copy_tree_contents(const fs::path &source_dir,
                            const fs::path &destination_dir) {
  std::error_code iterator_error;
  const fs::recursive_directory_iterator end;
  for (fs::recursive_directory_iterator iterator{source_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    const fs::path &source = entry.path();
    const fs::path destination =
        destination_dir / source.lexically_relative(source_dir);
    std::error_code entry_error;
    if (entry.is_symlink(entry_error) || entry.is_regular_file(entry_error)) {
      if (entry_error) {
        return view_error(
            fs::make_fs_error("failed to inspect", entry_error, source));
      }
      const memix::FsResult<bool> copied = fs::copy_file(source, destination);
      if (!copied) {
        return view_error(copied.error());
      }
      const memix::FsVoid read_only =
          fs::apply_read_only_file_permissions(destination);
      if (!read_only) {
        return view_error(read_only.error());
      }
      continue;
    }
    if (entry_error) {
      return view_error(
          fs::make_fs_error("failed to inspect", entry_error, source));
    }
    if (entry.is_directory(entry_error)) {
      const ViewVoid created = ensure_directory(destination);
      if (!created) {
        return std::unexpected(created.error());
      }
    }
  }
  if (iterator_error) {
    return view_error(fs::make_fs_error("failed to list directory",
                                        iterator_error, source_dir));
  }
  return {};
}

ViewResult<std::optional<GenerationMetadata>>
read_copy_marker(const fs::path &into) {
  const fs::path marker = into / std::string(kActivationMarkerFilename);
  ViewResult<std::optional<fs::file_status>> status = lstat(marker);
  if (!status) {
    return std::unexpected(status.error());
  }
  if (!status->has_value()) {
    return std::optional<GenerationMetadata>{};
  }
  memix::FsResult<std::string> text =
      fs::read_text_file(marker, "activation marker");
  if (!text) {
    return view_error(text.error());
  }
  ViewResult<GenerationMetadata> metadata =
      generation_metadata_from_json(*text, marker, kMarkerSchema);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  return std::optional<GenerationMetadata>{std::move(*metadata)};
}

struct CopyMaterializationRequest {
  memix::Store store;
  fs::path into;
  fs::path tree_path;
  GenerationMetadata metadata;
  bool force = false;
};

ViewResult<GenerationMetadata>
validated_recorded_copy_marker(const memix::Store &store,
                               const fs::path &into) {
  ViewResult<std::optional<GenerationMetadata>> marker = read_copy_marker(into);
  if (!marker) {
    return std::unexpected(marker.error());
  }
  if (!marker->has_value()) {
    return view_error(
        memix::ErrorKind::AlreadyExists,
        std::format("workspace path `{}` is a directory without a memix "
                    "activation marker; refusing to replace it",
                    into.string()));
  }
  const ViewVoid marker_checked = validate_copy_marker_location(**marker, into);
  if (!marker_checked) {
    return std::unexpected(marker_checked.error());
  }
  const fs::path profile_dir =
      profile_dir_for(store.root(), (*marker)->profile);
  const memix::FsResult<bool> profile_exists = fs::exists(profile_dir);
  if (!profile_exists) {
    return view_error(profile_exists.error());
  }
  if (!*profile_exists) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("copy activation at `{}` has no recorded profile `{}`",
                    into.string(), (*marker)->profile));
  }
  ViewResult<GenerationMetadata> recorded =
      read_generation_metadata(profile_dir, (*marker)->number);
  if (!recorded) {
    return std::unexpected(recorded.error());
  }
  if (!same_activation_identity(*recorded, **marker)) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("copy activation at `{}` does not match profile `{}` "
                    "generation {}",
                    into.string(), (*marker)->profile, (*marker)->number));
  }
  return std::move(**marker);
}

ViewVoid preflight_copy_workspace(const memix::Store &store,
                                  const fs::path &into, bool force) {
  ViewResult<std::optional<fs::file_status>> status = lstat(into);
  if (!status) {
    return std::unexpected(status.error());
  }
  if (!status->has_value()) {
    return {};
  }
  if (std::filesystem::is_symlink(**status)) {
    if (force) {
      return {};
    }
    return view_error(
        memix::ErrorKind::AlreadyExists,
        std::format("workspace path `{}` is a symlink; pass --force to "
                    "replace it with a copied activation",
                    into.string()));
  }
  if (std::filesystem::is_directory(**status)) {
    ViewResult<GenerationMetadata> marker =
        validated_recorded_copy_marker(store, into);
    if (!marker) {
      return std::unexpected(marker.error());
    }
    return {};
  }
  return view_error(
      memix::ErrorKind::AlreadyExists,
      std::format("workspace path `{}` already exists and is not a "
                  "directory; refusing to replace it",
                  into.string()));
}

ViewResult<fs::path>
preflight_activation_workspace(const memix::Store &store,
                               const memix::ActivationOptions &options) {
  if (options.mode == memix::ActivationMode::Copy) {
    const ViewVoid checked =
        preflight_copy_workspace(store, options.into, options.force);
    if (!checked) {
      return std::unexpected(checked.error());
    }
    return fs::path{};
  }
  memix::FsResult<fs::path> canonical_root = fs::weakly_canonical(store.root());
  if (!canonical_root) {
    return view_error(canonical_root.error());
  }
  fs::path target =
      current_link_for(profile_dir_for(*canonical_root, options.profile));
  const ViewVoid checked = preflight_workspace_link({
      .into = options.into,
      .target = target,
      .force = options.force,
  });
  if (!checked) {
    return std::unexpected(checked.error());
  }
  return target;
}

ViewResult<bool>
replace_existing_with_copy_tree(const fs::path &temp, const fs::path &into,
                                bool existing_is_directory) {
  if (existing_is_directory) {
    const ViewVoid links_checked = validate_removal_tree_hard_links(into);
    if (!links_checked) {
      return std::unexpected(links_checked.error());
    }
  }
  fs::path parent = into.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  ViewResult<fs::path> aside =
      unique_temp_dir(parent, std::format("old-{}", into.filename().string()));
  if (!aside) {
    return std::unexpected(aside.error());
  }
  const memix::FsVoid moved_aside = fs::rename(into, *aside);
  if (!moved_aside) {
    return view_error(moved_aside.error());
  }
  const memix::FsVoid swapped = fs::rename(temp, into);
  if (!swapped) {
    memix::ViewError swap_error = view_error_from_fs(swapped.error());
    const memix::FsVoid restored = fs::rename(*aside, into);
    if (!restored) {
      swap_error.message = std::format(
          "{}; additionally failed to restore the previous workspace: {}; "
          "recover it from `{}`",
          swap_error.message, restored.error().message, aside->string());
    }
    return std::unexpected(std::move(swap_error));
  }
  if (existing_is_directory) {
    remove_tree_best_effort(*aside);
  } else {
    std::ignore = fs::remove(*aside);
  }
  return true;
}

ViewResult<bool>
install_copy_tree(const fs::path &temp, const fs::path &into,
                  const std::optional<GenerationMetadata> &existing, bool force,
                  const memix::Store &store) {
  ViewResult<std::optional<fs::file_status>> status = lstat(into);
  if (!status) {
    return std::unexpected(status.error());
  }
  if (status->has_value() && std::filesystem::is_symlink(**status)) {
    if (!force) {
      return view_error(
          memix::ErrorKind::AlreadyExists,
          std::format("workspace path `{}` is a symlink; pass --force to "
                      "replace it with a copied activation",
                      into.string()));
    }
    return replace_existing_with_copy_tree(temp, into, false);
  }
  if (status->has_value() && std::filesystem::is_directory(**status)) {
    if (!existing.has_value()) {
      return view_error(
          memix::ErrorKind::AlreadyExists,
          std::format("workspace path `{}` is a directory without a memix "
                      "activation marker; refusing to replace it",
                      into.string()));
    }
    ViewResult<GenerationMetadata> revalidated =
        validated_recorded_copy_marker(store, into);
    if (!revalidated) {
      return std::unexpected(revalidated.error());
    }
    if (!same_activation_identity(*revalidated, *existing)) {
      return view_error(
          memix::ErrorKind::Conflict,
          std::format("copy activation at `{}` changed before replacement",
                      into.string()));
    }
    return replace_existing_with_copy_tree(temp, into, true);
  }
  if (status->has_value()) {
    return view_error(
        memix::ErrorKind::AlreadyExists,
        std::format("workspace path `{}` already exists and is not a "
                    "directory; refusing to replace it",
                    into.string()));
  }
  const memix::FsVoid renamed = fs::rename(temp, into);
  if (!renamed) {
    return view_error(renamed.error());
  }
  return true;
}

ViewResult<bool>
sync_copy_activation(const CopyMaterializationRequest &request) {
  std::optional<GenerationMetadata> existing;
  ViewResult<std::optional<fs::file_status>> status = lstat(request.into);
  if (!status) {
    return std::unexpected(status.error());
  }
  if (status->has_value() && std::filesystem::is_directory(**status)) {
    ViewResult<GenerationMetadata> marker =
        validated_recorded_copy_marker(request.store, request.into);
    if (!marker) {
      return std::unexpected(marker.error());
    }
    existing = std::move(*marker);
  }
  fs::path parent = request.into.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const ViewVoid parent_created = ensure_directory(parent);
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }

  ViewResult<fs::path> temp =
      unique_temp_dir(parent, request.into.filename().string());
  if (!temp) {
    return std::unexpected(temp.error());
  }
  const memix::FsResult<bool> temp_created = fs::create_directory(*temp);
  if (!temp_created) {
    return view_error(temp_created.error());
  }
  memix::scope_exit cleanup([temp = *temp] { remove_tree_best_effort(temp); });
  const ViewVoid copied = copy_tree_contents(request.tree_path, *temp);
  if (!copied) {
    return std::unexpected(copied.error());
  }
  ViewResult<std::string> marker_json =
      generation_metadata_json(request.metadata, kMarkerSchema);
  if (!marker_json) {
    return std::unexpected(marker_json.error());
  }
  const memix::FsVoid marker_written = fs::write_text_file(
      *temp / std::string(kActivationMarkerFilename), *marker_json);
  if (!marker_written) {
    return view_error(marker_written.error());
  }

  ViewResult<bool> installed = install_copy_tree(*temp, request.into, existing,
                                                 request.force, request.store);
  if (!installed) {
    return std::unexpected(installed.error());
  }
  cleanup.release();
  return *installed;
}

struct ResolvedWorkspaceActivation {
  memix::ActivationMode mode = memix::ActivationMode::Link;
  std::string profile;
  std::uint64_t generation = 0;
  GenerationMetadata metadata;
  bool stale = false;
};

ViewResult<ResolvedWorkspaceActivation>
resolve_link_activation(const memix::Store &store, const fs::path &into) {
  memix::FsResult<fs::path> target = fs::read_symlink(into);
  if (!target) {
    return view_error(target.error());
  }
  const fs::path absolute_target =
      target->is_absolute() ? *target : into.parent_path() / *target;
  // Canonicalize the parent only: the final component (`current`) is the
  // swap-point symlink and must not be resolved.
  memix::FsResult<fs::path> canonical_parent =
      fs::weakly_canonical(absolute_target.parent_path());
  if (!canonical_parent) {
    return view_error(canonical_parent.error());
  }
  const fs::path canonical_target =
      *canonical_parent / absolute_target.filename();
  memix::FsResult<fs::path> canonical_root = fs::weakly_canonical(store.root());
  if (!canonical_root) {
    return view_error(canonical_root.error());
  }
  const fs::path canonical_profiles = profiles_dir_for(*canonical_root);
  if (canonical_target.filename() != kCurrentLinkName
      || canonical_target.parent_path().parent_path() != canonical_profiles) {
    return view_error(
        memix::ErrorKind::NotFound,
        std::format("workspace path `{}` is a symlink but not a memix "
                    "activation",
                    into.string()));
  }
  const std::string profile =
      canonical_target.parent_path().filename().string();
  const fs::path profile_dir = canonical_target.parent_path();
  ViewResult<std::optional<std::uint64_t>> current =
      read_current_generation(profile_dir);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (!current->has_value()) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("profile `{}` has no current generation", profile));
  }
  ViewResult<GenerationMetadata> metadata =
      read_generation_metadata(profile_dir, **current);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  return ResolvedWorkspaceActivation{
      .mode = memix::ActivationMode::Link,
      .profile = profile,
      .generation = **current,
      .metadata = std::move(*metadata),
      .stale = false,
  };
}

ViewResult<ResolvedWorkspaceActivation>
resolve_copy_activation(const memix::Store &store, const fs::path &into) {
  ViewResult<std::optional<GenerationMetadata>> marker = read_copy_marker(into);
  if (!marker) {
    return std::unexpected(marker.error());
  }
  if (!marker->has_value()) {
    return view_error(
        memix::ErrorKind::NotFound,
        std::format("workspace path `{}` is a directory without a memix "
                    "activation marker",
                    into.string()));
  }
  const ViewVoid marker_checked = validate_copy_marker_location(**marker, into);
  if (!marker_checked) {
    return std::unexpected(marker_checked.error());
  }
  const fs::path profile_dir =
      profile_dir_for(store.root(), (*marker)->profile);
  ViewResult<std::optional<std::uint64_t>> current =
      read_current_generation(profile_dir);
  if (!current) {
    return std::unexpected(current.error());
  }
  bool stale = true;
  if (current->has_value()) {
    ViewResult<GenerationMetadata> current_metadata =
        read_generation_metadata(profile_dir, **current);
    if (!current_metadata) {
      return std::unexpected(current_metadata.error());
    }
    stale = !same_activation_identity(**marker, *current_metadata);
  }
  return ResolvedWorkspaceActivation{
      .mode = memix::ActivationMode::Copy,
      .profile = (*marker)->profile,
      .generation = (*marker)->number,
      .metadata = std::move(**marker),
      .stale = stale,
  };
}

ViewResult<std::optional<ResolvedWorkspaceActivation>>
resolve_workspace_activation(const memix::Store &store, const fs::path &into) {
  ViewResult<std::optional<fs::file_status>> status = lstat(into);
  if (!status) {
    return std::unexpected(status.error());
  }
  if (!status->has_value()) {
    return std::optional<ResolvedWorkspaceActivation>{};
  }
  if (std::filesystem::is_symlink(**status)) {
    ViewResult<ResolvedWorkspaceActivation> activation =
        resolve_link_activation(store, into);
    if (!activation) {
      return std::unexpected(activation.error());
    }
    return std::optional<ResolvedWorkspaceActivation>{std::move(*activation)};
  }
  if (std::filesystem::is_directory(**status)) {
    ViewResult<ResolvedWorkspaceActivation> activation =
        resolve_copy_activation(store, into);
    if (!activation) {
      return std::unexpected(activation.error());
    }
    return std::optional<ResolvedWorkspaceActivation>{std::move(*activation)};
  }
  return view_error(memix::ErrorKind::NotFound,
                    std::format("workspace path `{}` is not a memix activation",
                                into.string()));
}

struct CurrentGeneration {
  std::uint64_t number = 0;
  GenerationMetadata metadata;
  std::optional<std::uint64_t> previous;
  bool switched = false;
};

ViewResult<CurrentGeneration>
ensure_current_generation(const fs::path &profile_dir,
                          const memix::ViewEnvironment &environment,
                          memix::ActivationMode mode,
                          const fs::path &workspace) {
  ViewResult<fs::path> attach_path = workspace_attach_path(workspace);
  if (!attach_path) {
    return std::unexpected(attach_path.error());
  }
  const std::string workspace_identity = attach_path->generic_string();
  ViewResult<std::optional<std::uint64_t>> current =
      read_current_generation(profile_dir);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (current->has_value()) {
    ViewResult<GenerationMetadata> existing =
        read_generation_metadata(profile_dir, **current);
    if (!existing) {
      return std::unexpected(existing.error());
    }
    // Link attachments follow the profile's `current` pointer, so the
    // recorded workspace is informational and re-activating the same
    // environment from another workspace must stay a no-op. Copy
    // generations are workspace-specific: rollback re-materializes the
    // recorded workspace, so a copy activated elsewhere needs its own
    // generation.
    const bool same_workspace = mode == memix::ActivationMode::Link
                             || existing->workspace == workspace_identity;
    if (existing->environment_id == environment.id
        && existing->derivation_id == environment.derivation.id
        && existing->mode == mode
        && same_workspace) {
      return CurrentGeneration{
          .number = **current,
          .metadata = std::move(*existing),
          .previous = *current,
          .switched = false,
      };
    }
  }
  ViewResult<std::vector<std::uint64_t>> existing_generations =
      scan_generations(profile_dir);
  if (!existing_generations) {
    return std::unexpected(existing_generations.error());
  }
  if (!existing_generations->empty()
      && existing_generations->back()
             == std::numeric_limits<std::uint64_t>::max()) {
    return view_error(
        memix::ErrorKind::Conflict,
        std::format("profile `{}` has exhausted generation numbers",
                    profile_dir.filename().string()));
  }
  const std::uint64_t number =
      existing_generations->empty() ? 1 : existing_generations->back() + 1;
  ViewResult<std::string> activated_at = utc_timestamp();
  if (!activated_at) {
    return std::unexpected(activated_at.error());
  }
  const GenerationMetadata metadata{
      .profile = profile_dir.filename().string(),
      .number = number,
      .environment_id = environment.id,
      .derivation_id = environment.derivation.id,
      .activated_at = std::move(*activated_at),
      .mode = mode,
      .workspace = workspace_identity,
  };
  const ViewVoid written =
      write_generation(profile_dir, metadata, environment.derivation.tree_path);
  if (!written) {
    return std::unexpected(written.error());
  }
  return CurrentGeneration{
      .number = number,
      .metadata = metadata,
      .previous = *current,
      .switched = true,
  };
}

ViewResult<std::uint64_t>
select_rollback_target(const std::vector<std::uint64_t> &existing,
                       std::uint64_t current,
                       std::optional<std::uint64_t> generation,
                       std::string_view profile) {
  if (generation.has_value()) {
    if (!std::ranges::contains(existing, *generation)) {
      return view_error(
          memix::ErrorKind::NotFound,
          std::format("generation {} does not exist in profile `{}`",
                      *generation, profile));
    }
    return *generation;
  }
  for (const std::uint64_t candidate : existing | std::views::reverse) {
    if (candidate < current) {
      return candidate;
    }
  }
  return view_error(memix::ErrorKind::NotFound,
                    std::format("profile `{}` has no generation older than {}",
                                profile, current));
}

ViewResult<bool>
rematerialize_copy_workspace(const memix::Store &store,
                             const GenerationMetadata &metadata) {
  if (metadata.mode != memix::ActivationMode::Copy
      || metadata.workspace.empty()) {
    return false;
  }
  ViewResult<std::optional<GenerationMetadata>> marker =
      read_copy_marker(metadata.workspace);
  if (!marker) {
    return std::unexpected(marker.error());
  }
  if (!marker->has_value()) {
    return false;
  }
  const ViewVoid marker_checked =
      validate_copy_marker_location(**marker, metadata.workspace);
  if (!marker_checked) {
    return std::unexpected(marker_checked.error());
  }
  if ((*marker)->profile != metadata.profile) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("copy workspace `{}` belongs to profile `{}`, not `{}`",
                    metadata.workspace, (*marker)->profile, metadata.profile));
  }
  const fs::path tree_path =
      derivation_dir_for(store.root(), metadata.derivation_id)
      / std::string(kTreeDir);
  ViewResult<bool> synced = sync_copy_activation({
      .store = store,
      .into = fs::path(metadata.workspace),
      .tree_path = tree_path,
      .metadata = metadata,
      .force = true,
  });
  if (!synced) {
    return std::unexpected(synced.error());
  }
  return *synced;
}

ViewResult<ResolvedWorkspaceActivation> revalidate_workspace_activation(
    const memix::Store &store, const fs::path &into,
    const ResolvedWorkspaceActivation &expected_activation) {
  ViewResult<std::optional<ResolvedWorkspaceActivation>> resolved =
      resolve_workspace_activation(store, into);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  if (!resolved->has_value()) {
    return view_error(
        memix::ErrorKind::Conflict,
        std::format("activation at `{}` changed while waiting for a lock",
                    into.string()));
  }
  ResolvedWorkspaceActivation activation = std::move(**resolved);
  if (activation.mode != expected_activation.mode
      || !same_activation_identity(activation.metadata,
                                   expected_activation.metadata)) {
    return view_error(
        memix::ErrorKind::Conflict,
        std::format("activation at `{}` changed while waiting for a lock",
                    into.string()));
  }
  return activation;
}

struct ProfileDeactivationLockRequest {
  std::reference_wrapper<const memix::Store> store;
  std::reference_wrapper<const fs::path> into;
  std::reference_wrapper<const fs::path> profile_dir;
  std::reference_wrapper<const ResolvedWorkspaceActivation> expected_activation;
  std::reference_wrapper<bool> lock_held;
};

ViewResult<ResolvedWorkspaceActivation>
lock_profile_for_deactivation(const ProfileDeactivationLockRequest &request) {
  const fs::path &profile_dir = request.profile_dir.get();
  const fs::path &into = request.into.get();
  const ResolvedWorkspaceActivation &expected_activation =
      request.expected_activation.get();
  const memix::FsResult<bool> profile_exists = fs::exists(profile_dir);
  if (!profile_exists) {
    return view_error(profile_exists.error());
  }
  if (!*profile_exists) {
    return view_error(
        memix::ErrorKind::NotFound,
        std::format("activation at `{}` has no recorded profile `{}`",
                    into.string(), expected_activation.profile));
  }
  const ViewVoid locked = acquire_existing_profile_lock(profile_dir);
  if (!locked) {
    return std::unexpected(locked.error());
  }
  request.lock_held.get() = true;

  ViewResult<ResolvedWorkspaceActivation> activation =
      revalidate_workspace_activation(request.store.get(), into,
                                      expected_activation);
  if (!activation) {
    return std::unexpected(activation.error());
  }
  if (activation->mode != memix::ActivationMode::Copy) {
    return activation;
  }
  ViewResult<GenerationMetadata> recorded =
      read_generation_metadata(profile_dir, activation->metadata.number);
  if (!recorded) {
    return std::unexpected(recorded.error());
  }
  if (!same_activation_identity(*recorded, activation->metadata)) {
    return view_error(
        memix::ErrorKind::InvalidData,
        std::format("copy activation at `{}` does not match profile `{}` "
                    "generation {}",
                    into.string(), activation->profile,
                    activation->metadata.number));
  }
  return activation;
}

ViewVoid
remove_workspace_activation(const fs::path &into, memix::ActivationMode mode) {
  if (mode == memix::ActivationMode::Link) {
    const memix::FsResult<bool> removed = fs::remove(into);
    if (!removed) {
      return view_error(removed.error());
    }
    return {};
  }
  ViewResult<std::uintmax_t> removed = remove_writable_tree(into);
  if (!removed) {
    return std::unexpected(removed.error());
  }
  return {};
}

ViewVoid wipe_locked_profile(const fs::path &profile_dir, bool &lock_held) {
  fs::path parent = profile_dir.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  ViewResult<fs::path> tombstone = unique_temp_dir(
      parent, std::format("deleted-{}", profile_dir.filename().string()));
  if (!tombstone) {
    return std::unexpected(tombstone.error());
  }
  const memix::FsVoid moved = fs::rename(profile_dir, *tombstone);
  if (!moved) {
    return view_error(moved.error());
  }

  // The lock moved with the profile. Disarm cleanup before exposing the
  // original profile path so it cannot remove a new owner's lock there.
  lock_held = false;
  ViewResult<std::uintmax_t> removed = remove_writable_tree(*tombstone);
  if (!removed) {
    memix::ViewError removal_error = removed.error();
    removal_error.message =
        std::format("{}; recover or remove the profile tombstone at `{}`",
                    removal_error.message, tombstone->string());
    return std::unexpected(std::move(removal_error));
  }
  return {};
}

ViewResult<memix::ViewIrInput>
lower_authored_input(const memix::AuthoredViewInput &authored) {
  if (authored.kind == memix::ViewInputKind::Context) {
    ViewResult<memix::ViewIrInput> input = parse_locator(authored.locator);
    if (!input) {
      return std::unexpected(input.error());
    }
    input->kind = memix::ViewInputKind::Context;
    return input;
  }
  if (!memix::detail::is_lower_hex_prefix(authored.environment_id)) {
    return view_error(
        memix::ErrorKind::InvalidArgument,
        std::format("environment id prefix must be lowercase hex `{}`",
                    authored.environment_id));
  }
  if (authored.output_role != "file_tree") {
    return view_error(memix::ErrorKind::InvalidArgument,
                      std::format("unsupported environment output `{}`",
                                  authored.output_role));
  }
  const std::string mount_text = authored.mount.generic_string();
  ViewResult<fs::path> mount =
      validate_mount_path(mount_text, authored.environment_id);
  if (!mount) {
    return std::unexpected(mount.error());
  }
  return memix::ViewIrInput{
      .locator = {},
      .type = memix::MemoryObjectType::Rules,
      .path = {},
      .version = {},
      .recursive = false,
      .required = true,
      .kind = memix::ViewInputKind::Environment,
      .environment_id = authored.environment_id,
      .output_role = authored.output_role,
      .mount = std::move(*mount),
  };
}

ViewResult<std::vector<memix::ViewIrInput>>
lower_authored_inputs(const std::vector<memix::AuthoredViewInput> &authored) {
  std::vector<memix::ViewIrInput> inputs;
  inputs.reserve(authored.size());
  for (const memix::AuthoredViewInput &authored_input : authored) {
    ViewResult<memix::ViewIrInput> input = lower_authored_input(authored_input);
    if (!input) {
      return std::unexpected(input.error());
    }
    inputs.push_back(std::move(*input));
  }
  return inputs;
}

ViewResult<std::vector<memix::ViewIrInput>>
lower_source_locators(const std::vector<std::string> &sources) {
  std::vector<memix::ViewIrInput> inputs;
  inputs.reserve(sources.size());
  for (const std::string &source : sources) {
    ViewResult<memix::ViewIrInput> input = parse_locator(source);
    if (!input) {
      return std::unexpected(input.error());
    }
    inputs.push_back(std::move(*input));
  }
  return inputs;
}

ViewVoid
validate_composed_environment_closure(const memix::ViewEnvironment &environment,
                                      const memix::ViewDerivation &derivation) {
  for (const memix::ResolvedViewEnvironmentInput &input :
       derivation.environment_inputs) {
    const std::string mount = input.mount.generic_string();
    const ViewResult<fs::path> valid_mount =
        validate_mount_path(mount, input.environment_id);
    if (!memix::detail::is_lower_hex_sha256(input.environment_id)
        || !memix::detail::is_lower_hex_sha256(input.derivation_id)
        || !memix::detail::is_lower_hex_sha256(input.artifact_id)
        || input.output_role != "file_tree"
        || !valid_mount) {
      return view_error(memix::ErrorKind::InvalidData,
                        "composed derivation contains an invalid view input");
    }
    if (!std::ranges::contains(environment.closure.environments,
                               input.environment_id)
        || !std::ranges::contains(environment.closure.derivations,
                                  input.derivation_id)
        || !std::ranges::contains(environment.closure.artifacts,
                                  input.artifact_id)) {
      return view_error(
          memix::ErrorKind::InvalidData,
          std::format("environment `{}` omits direct upstream provenance",
                      environment.id));
    }
  }
  return {};
}

} // namespace

namespace memix {

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
      .inputs = {},
  };
}

ViewResult<AuthoredViewExpression>
ViewService::expression_from_inputs(std::vector<AuthoredViewInput> inputs,
                                    ViewTargetKind target) const {
  const Store &unused_store = store_;
  (void)unused_store;
  return AuthoredViewExpression{
      .schema = std::string(kExpressionSchemaV2),
      .profile = {},
      .sources = {},
      .target = target,
      .inputs = std::move(inputs),
  };
}

ViewResult<ViewIr>
ViewService::lower_to_ir(const AuthoredViewExpression &expression) const {
  const Store &unused_store = store_;
  (void)unused_store;
  if (!expression.schema.empty()
      && expression.schema != kExpressionSchema
      && expression.schema != kExpressionSchemaV2) {
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
  const bool typed =
      expression.schema == kExpressionSchemaV2 || !expression.inputs.empty();
  if (typed) {
    if (!expression.sources.empty()) {
      return view_error(ErrorKind::InvalidData,
                        "typed view expressions must not contain sources");
    }
    ir.schema = std::string(kIrSchemaV2);
    ViewResult<std::vector<ViewIrInput>> inputs =
        lower_authored_inputs(expression.inputs);
    if (!inputs) {
      return std::unexpected(inputs.error());
    }
    ir.inputs = std::move(*inputs);
    return ir;
  }
  ViewResult<std::vector<ViewIrInput>> inputs =
      lower_source_locators(expression.sources);
  if (!inputs) {
    return std::unexpected(inputs.error());
  }
  ir.inputs = std::move(*inputs);
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
  std::string ir_hash = sha256_text(*ir_json);
  ViewResult<ResolvedBuildInputs> resolved = resolve_inputs(store_, ir);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  const std::vector<ResolvedInputJson> inputs =
      resolved_input_json(resolved->files);
  const std::vector<ResolvedEnvironmentInputJson> environment_inputs =
      resolved_environment_input_json(resolved->environment_inputs);
  ViewResult<std::string> basis =
      environment_inputs.empty()
          ? ir_basis_json_for(ir_hash, inputs, ir.target.kind)
          : ir_basis_json_for_v2(ir_hash, inputs, environment_inputs,
                                 ir.target.kind);
  if (!basis) {
    return std::unexpected(basis.error());
  }
  std::string derivation_id = sha256_text(*basis);
  ViewResult<std::vector<ViewDerivationOutput>> outputs =
      outputs_for_derivation(derivation_id, ir.target.kind);
  if (!outputs) {
    return std::unexpected(outputs.error());
  }

  const fs::path derivation_dir =
      derivation_dir_for(store_.root(), derivation_id);
  ViewDerivation derivation{
      .id = std::move(derivation_id),
      .ir_hash = std::move(ir_hash),
      .target = ir.target.kind,
      .resolved_files = std::move(resolved->files),
      .outputs = std::move(*outputs),
      .manifest_path = derivation_dir / std::string(kManifestFilename),
      .tree_path = derivation_dir / std::string(kTreeDir),
      .inserted = false,
      .environment_inputs = std::move(resolved->environment_inputs),
  };
  const memix::FsResult<bool> derivation_exists = fs::exists(derivation_dir);
  if (!derivation_exists) {
    return view_error(derivation_exists.error());
  }
  if (*derivation_exists) {
    ViewResult<ViewDerivation> existing =
        resolve_derivation(store_, derivation.id);
    if (!existing) {
      return std::unexpected(existing.error());
    }
    if (existing->ir_hash != derivation.ir_hash
        || existing->target != derivation.target) {
      return view_error(ErrorKind::InvalidData,
                        std::format("existing derivation `{}` does not match "
                                    "the requested IR",
                                    derivation.id));
    }
    ViewResult<QuestDefinition> quest = upsert_quest(store_.root(), ir, {});
    if (!quest) {
      return std::unexpected(quest.error());
    }
    return existing;
  }
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
  ViewResult<QuestDefinition> quest = upsert_quest(store_.root(), ir, {});
  if (!quest) {
    return std::unexpected(quest.error());
  }
  return derivation;
}

ViewResult<ViewEnvironment>
ViewService::build(const AuthoredViewExpression &expression) const {
  return build(expression, BuildOptions{});
}

ViewResult<ViewEnvironment>
ViewService::build(const AuthoredViewExpression &expression,
                   const BuildOptions &options) const {
  ViewResult<ViewIr> ir = lower_to_ir(expression);
  if (!ir) {
    return std::unexpected(ir.error());
  }
  ViewResult<ViewDerivation> derivation = build_ir(*ir);
  if (!derivation) {
    return std::unexpected(derivation.error());
  }
  if (!options.intent.empty() || !options.name.empty()) {
    const QuestMetadataUpdate metadata{
        .intent = options.intent.empty()
                    ? std::nullopt
                    : std::optional<std::string_view>{options.intent},
        .name = options.name.empty()
                  ? std::nullopt
                  : std::optional<std::string_view>{options.name},
    };
    ViewResult<QuestDefinition> quest =
        upsert_quest(store_.root(), *ir, metadata);
    if (!quest) {
      return std::unexpected(quest.error());
    }
  }
  ViewResult<ViewEnvironment> environment =
      environment_for_derivation(store_, std::move(*derivation));
  if (!environment) {
    return std::unexpected(environment.error());
  }
  const ViewVoid recorded =
      record_resolution(store_.root(), environment->derivation, *environment);
  if (!recorded) {
    return std::unexpected(recorded.error());
  }
  return environment;
}

ViewResult<QuestDefinition>
ViewService::define_quest(const AuthoredViewExpression &expression,
                          std::string_view intent,
                          std::string_view name) const {
  const QuestDefinitionOptions options{
      .intent =
          intent.empty() ? std::nullopt : std::optional<std::string>{intent},
      .name = name.empty() ? std::nullopt : std::optional<std::string>{name},
  };
  return define_quest(expression, options);
}

ViewResult<QuestDefinition>
ViewService::define_quest(const AuthoredViewExpression &expression,
                          const QuestDefinitionOptions &options) const {
  ViewResult<ViewIr> ir = lower_to_ir(expression);
  if (!ir) {
    return std::unexpected(ir.error());
  }
  return upsert_quest(
      store_.root(), *ir,
      QuestMetadataUpdate{
          .intent = options.intent.has_value()
                      ? std::optional<std::string_view>{*options.intent}
                      : std::nullopt,
          .name = options.name.has_value()
                    ? std::optional<std::string_view>{*options.name}
                    : std::nullopt,
      });
}

ViewResult<std::vector<QuestInfo>> ViewService::list_quests() const {
  const fs::path quests_dir = quests_dir_for(store_.root());
  const memix::FsResult<bool> exists = fs::exists(quests_dir);
  if (!exists) {
    return view_error(exists.error());
  }
  if (!*exists) {
    return std::vector<QuestInfo>{};
  }

  std::vector<QuestInfo> quests;
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{quests_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    if (!entry.is_directory(entry_error)) {
      if (entry_error) {
        return view_error(
            fs::make_fs_error("failed to inspect", entry_error, entry.path()));
      }
      continue;
    }
    const std::string ir_hash = entry.path().filename().string();
    if (!detail::is_lower_hex_sha256(ir_hash)) {
      continue;
    }
    ViewResult<QuestRecord> quest =
        read_quest_record(store_.root(), ir_hash);
    if (!quest) {
      return std::unexpected(quest.error());
    }
    QuestInfo info{
        .ir_hash = quest->ir_hash,
        .intent = quest->intent,
        .name = quest->name,
        .created_at = quest->created_at,
        .resolution_count = 0,
        .last_resolved_at = {},
        .latest_derivation_id = {},
        .latest_environment_id = {},
        .read_count = 0,
        .last_read_at = {},
        .activation_count = 0,
        .last_activated_at = {},
    };
    ViewResult<std::vector<QuestHistoryEntry>> history =
        read_quest_history(store_.root(), ir_hash);
    if (history) {
      info.resolution_count = history->size();
      if (!history->empty()) {
        const QuestHistoryEntry &latest = history->back();
        info.last_resolved_at = latest.resolved_at;
        info.latest_derivation_id = latest.derivation_id;
        info.latest_environment_id = latest.environment_id;
      }
    }
    ViewResult<QuestReceiptSummary> receipts =
        read_quest_receipt_summary(store_.root(), ir_hash);
    if (receipts) {
      info.read_count = receipts->read_count;
      info.last_read_at = receipts->last_read_at;
      info.activation_count = receipts->activation_count;
      info.last_activated_at = receipts->last_activated_at;
    }
    quests.push_back(std::move(info));
  }
  if (iterator_error) {
    return view_error(fs::make_fs_error("failed to list directory",
                                        iterator_error, quests_dir));
  }
  std::ranges::sort(
      quests, [](const QuestInfo &left, const QuestInfo &right) {
        return std::tie(left.created_at, left.ir_hash)
             < std::tie(right.created_at, right.ir_hash);
      });
  return quests;
}

ViewResult<std::vector<QuestInfo>>
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ViewService::find_quests(std::vector<std::string> terms,
                         std::optional<fs::path> covers) const {
  if (terms.empty() && !covers.has_value()) {
    return view_error(ErrorKind::InvalidArgument,
                      "find requires at least one term or --covers");
  }
  const auto lower_ascii = [](std::string text) {
    for (char &character : text) {
      if (character >= 'A' && character <= 'Z') {
        character = static_cast<char>(character - 'A' + 'a');
      }
    }
    return text;
  };
  for (std::string &term : terms) {
    term = lower_ascii(std::move(term));
  }

  std::optional<MemoryObjectType> cover_type;
  fs::path cover_path;
  if (covers.has_value()) {
    std::string cover_text = covers->generic_string();
    const std::size_t separator = cover_text.find('/');
    if (separator != std::string::npos) {
      const std::optional<MemoryObjectType> parsed_type =
          parse_memory_object_type(
              std::string_view(cover_text).substr(0, separator));
      if (parsed_type.has_value()) {
        cover_type = parsed_type;
        cover_text.erase(0, separator + 1);
      }
    }
    ViewResult<fs::path> valid_cover = validate_view_path(
        {.value = cover_text, .locator = covers->generic_string()});
    if (!valid_cover) {
      return std::unexpected(valid_cover.error());
    }
    cover_path = std::move(*valid_cover);
  }

  ViewResult<std::vector<QuestInfo>> quests = list_quests();
  if (!quests) {
    return std::unexpected(quests.error());
  }
  std::vector<QuestInfo> matches;
  for (QuestInfo &quest : *quests) {
    const std::string searchable =
        lower_ascii(std::format("{} {}", quest.intent, quest.name));
    const bool terms_match =
        std::ranges::all_of(terms, [&searchable](const std::string &term) {
          return searchable.contains(term);
        });
    if (!terms_match) {
      continue;
    }
    if (covers.has_value()) {
      ViewResult<ViewIr> ir = load_quest_ir(store_.root(), quest.ir_hash);
      if (!ir) {
        return std::unexpected(ir.error());
      }
      const bool covered =
          std::ranges::any_of(ir->inputs, [&](const ViewIrInput &input) {
            if (input.kind != ViewInputKind::Context
                || (cover_type.has_value() && input.type != *cover_type)) {
              return false;
            }
            return input.path == cover_path
                || (input.recursive && path_is_under(cover_path, input.path))
                || path_is_under(input.path, cover_path);
          });
      if (!covered) {
        continue;
      }
    }
    matches.push_back(std::move(quest));
  }
  return matches;
}

ViewResult<ViewEnvironment>
ViewService::resolve_quest(std::string_view quest_prefix) const {
  ViewResult<std::string> ir_hash =
      resolve_unique_quest_id(store_.root(), quest_prefix);
  if (!ir_hash) {
    return std::unexpected(ir_hash.error());
  }
  ViewResult<ViewIr> ir = load_quest_ir(store_.root(), *ir_hash);
  if (!ir) {
    return std::unexpected(ir.error());
  }
  ViewResult<ViewDerivation> derivation = build_ir(*ir);
  if (!derivation) {
    return std::unexpected(derivation.error());
  }
  ViewResult<ViewEnvironment> environment =
      environment_for_derivation(store_, std::move(*derivation));
  if (!environment) {
    return std::unexpected(environment.error());
  }
  const ViewVoid recorded =
      record_resolution(store_.root(), environment->derivation, *environment);
  if (!recorded) {
    return std::unexpected(recorded.error());
  }
  return environment;
}

ViewResult<QuestFreshness>
ViewService::quest_freshness(std::string_view quest_prefix) const {
  ViewResult<std::string> ir_hash =
      resolve_unique_quest_id(store_.root(), quest_prefix);
  if (!ir_hash) {
    return std::unexpected(ir_hash.error());
  }
  ViewResult<std::vector<QuestHistoryEntry>> history =
      read_quest_history(store_.root(), *ir_hash);
  if (!history) {
    return std::unexpected(history.error());
  }
  if (history->empty()) {
    return view_error(ErrorKind::NotFound,
                      std::format("quest `{}` has no resolutions", *ir_hash));
  }
  const QuestHistoryEntry &latest = history->back();
  ViewResult<ViewDerivation> derivation =
      resolve_derivation(store_, latest.derivation_id);
  if (!derivation) {
    return std::unexpected(derivation.error());
  }
  if (derivation->ir_hash != *ir_hash) {
    return view_error(ErrorKind::InvalidData,
                      "quest history derivation has a different ir_hash");
  }
  ViewResult<ViewIr> ir = load_quest_ir(store_.root(), *ir_hash);
  if (!ir) {
    return std::unexpected(ir.error());
  }
  QuestFreshness freshness{
      .ir_hash = *ir_hash,
      .derivation_id = latest.derivation_id,
      .resolved_at = latest.resolved_at,
      .inputs = {},
      .stale = false,
  };
  for (const ViewIrInput &input : ir->inputs) {
    if (input.kind != ViewInputKind::Context) {
      continue;
    }
    const auto resolution = std::ranges::find(
        derivation->resolved_files, input.locator,
        &ResolvedViewFile::locator);
    if (resolution == derivation->resolved_files.end()) {
      continue;
    }
    QuestFreshnessInput status{
        .locator = input.locator,
        .recorded_generation = resolution->source_generation,
        .current_generation = std::nullopt,
        .pinned = input.version.kind == ViewVersionKind::Generation
               || input.version.value.starts_with("generation/"),
        .fresh = true,
    };
    if (!status.pinned) {
      StoreResult<std::optional<std::string>> current =
          store_.read_ref(version_ref_name(input.type, input.version.value));
      if (!current) {
        return view_error(current.error());
      }
      if (current->has_value()) {
        status.current_generation = **current;
        status.fresh = **current == status.recorded_generation;
      } else {
        status.fresh = false;
      }
    }
    freshness.stale = freshness.stale || !status.fresh;
    freshness.inputs.push_back(std::move(status));
  }
  return freshness;
}

ViewResult<QuestStats>
ViewService::quest_stats(std::string_view quest_prefix) const {
  ViewResult<std::string> ir_hash =
      resolve_unique_quest_id(store_.root(), quest_prefix);
  if (!ir_hash) {
    return std::unexpected(ir_hash.error());
  }
  const ViewResult<QuestRecord> quest =
      read_quest_record(store_.root(), *ir_hash);
  if (!quest) {
    return std::unexpected(quest.error());
  }
  QuestStats stats{
      .ir_hash = *ir_hash,
      .resolution_count = 0,
      .last_resolved_at = {},
      .activation_count = 0,
      .first_activated_at = {},
      .last_activated_at = {},
      .activations_by_profile = {},
      .read_count = 0,
      .first_read_at = {},
      .last_read_at = {},
      .cat_count = 0,
      .tree_count = 0,
      .reads_by_environment = {},
  };
  ViewResult<std::vector<QuestHistoryEntry>> history =
      read_quest_history(store_.root(), *ir_hash);
  if (history) {
    stats.resolution_count = history->size();
    if (!history->empty()) {
      stats.last_resolved_at = history->back().resolved_at;
    }
  }
  ViewResult<QuestReceiptSummary> receipts =
      read_quest_receipt_summary(store_.root(), *ir_hash);
  if (!receipts) {
    return stats;
  }
  stats.activation_count = receipts->activation_count;
  stats.first_activated_at = receipts->first_activated_at;
  stats.last_activated_at = receipts->last_activated_at;
  stats.activations_by_profile.reserve(
      receipts->activations_by_profile.size());
  for (const auto &[profile, activation_count] :
       receipts->activations_by_profile) {
    stats.activations_by_profile.push_back(QuestProfileActivations{
        .profile = profile,
        .activation_count = activation_count,
    });
  }
  std::ranges::sort(stats.activations_by_profile,
                    [](const QuestProfileActivations &left,
                       const QuestProfileActivations &right) {
                      if (left.activation_count != right.activation_count) {
                        return left.activation_count > right.activation_count;
                      }
                      return left.profile < right.profile;
                    });
  stats.read_count = receipts->read_count;
  stats.first_read_at = receipts->first_read_at;
  stats.last_read_at = receipts->last_read_at;
  stats.cat_count = receipts->cat_count;
  stats.tree_count = receipts->tree_count;
  stats.reads_by_environment.reserve(receipts->reads_by_environment.size());
  for (const auto &[environment_id, read_count] :
       receipts->reads_by_environment) {
    stats.reads_by_environment.push_back(QuestEnvironmentReads{
        .environment_id = environment_id,
        .read_count = read_count,
    });
  }
  std::ranges::sort(stats.reads_by_environment,
                    [](const QuestEnvironmentReads &left,
                       const QuestEnvironmentReads &right) {
                      if (left.read_count != right.read_count) {
                        return left.read_count > right.read_count;
                      }
                      return left.environment_id < right.environment_id;
                    });
  return stats;
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
  for (const std::string &dependency : environment->closure.environments) {
    if (!detail::is_lower_hex_sha256(dependency)) {
      return view_error(
          ErrorKind::InvalidData,
          std::format("environment `{}` has an invalid upstream id `{}`",
                      *environment_id, dependency));
    }
    if (dependency == *environment_id) {
      return view_error(
          ErrorKind::InvalidData,
          std::format("environment `{}` contains a dependency cycle",
                      *environment_id));
    }
  }
  ViewResult<ViewDerivation> derivation =
      resolve_derivation(store_, binding.derivation_id);
  if (!derivation) {
    return std::unexpected(derivation.error());
  }
  const ViewVoid closure_valid =
      validate_composed_environment_closure(*environment, *derivation);
  if (!closure_valid) {
    return std::unexpected(closure_valid.error());
  }
  environment->derivation = std::move(*derivation);
  return environment;
}

ViewResult<std::vector<ViewEnvironmentInfo>>
ViewService::list_environments() const {
  const fs::path environments_dir = environments_dir_for(store_.root());
  const memix::FsResult<bool> exists = fs::exists(environments_dir);
  if (!exists) {
    return view_error(exists.error());
  }
  if (!*exists) {
    return std::vector<ViewEnvironmentInfo>{};
  }

  std::vector<std::string> environment_ids;
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
    std::string id = entry.path().filename().string();
    if (!id.ends_with(kJsonSuffix)) {
      continue;
    }
    id.resize(id.size() - kJsonSuffix.size());
    if (detail::is_lower_hex_sha256(id)) {
      environment_ids.push_back(std::move(id));
    }
  }
  if (iterator_error) {
    return view_error(fs::make_fs_error("failed to list directory",
                                        iterator_error, environments_dir));
  }
  std::ranges::sort(environment_ids);

  std::vector<ViewEnvironmentInfo> environments;
  environments.reserve(environment_ids.size());
  for (const std::string &id : environment_ids) {
    ViewResult<ViewEnvironment> environment = resolve_environment(id);
    if (!environment) {
      return std::unexpected(environment.error());
    }
    std::uintmax_t total_size_bytes = 0;
    for (const ResolvedViewFile &file :
         environment->derivation.resolved_files) {
      total_size_bytes += file.blob.size_bytes;
    }
    environments.push_back(ViewEnvironmentInfo{
        .id = environment->id,
        .derivation_id = environment->derivation.id,
        .file_count = environment->derivation.resolved_files.size(),
        .total_size_bytes = total_size_bytes,
        .upstream_environment_count = environment->closure.environments.size(),
    });
  }
  return environments;
}

ViewResult<std::vector<ViewCatalogEntry>>
ViewService::environment_tree(std::string_view environment_prefix,
                              const ViewCatalogOptions &options) const {
  fs::path selected_path;
  if (!options.path.empty()) {
    ViewResult<fs::path> validated = validate_view_path({
        .value = options.path.generic_string(),
        .locator = options.path.generic_string(),
    });
    if (!validated) {
      return std::unexpected(validated.error());
    }
    selected_path = std::move(*validated);
  }

  ViewResult<ViewEnvironment> environment =
      resolve_environment(environment_prefix);
  if (!environment) {
    return std::unexpected(environment.error());
  }

  const auto components = [](const fs::path &path) {
    return path | std::ranges::to<std::vector<fs::path>>();
  };
  const auto is_same_or_under = [&components](const fs::path &candidate,
                                              const fs::path &ancestor) {
    if (ancestor.empty()) {
      return true;
    }
    const std::vector<fs::path> candidate_components = components(candidate);
    const std::vector<fs::path> ancestor_components = components(ancestor);
    return ancestor_components.size() <= candidate_components.size()
        && std::ranges::equal(
               ancestor_components,
               candidate_components
                   | std::views::take(ancestor_components.size()));
  };
  const auto relative_depth = [&components](const fs::path &path,
                                            const fs::path &base) {
    return components(path).size() - components(base).size();
  };

  struct DirectoryAggregate {
    std::size_t file_count = 0;
    std::uintmax_t total_size_bytes = 0;
  };
  std::map<fs::path, DirectoryAggregate> directories;
  std::vector<ViewCatalogEntry> entries;
  bool matched = false;
  for (const ResolvedViewFile &file : environment->derivation.resolved_files) {
    if (!is_same_or_under(file.output_path, selected_path)) {
      continue;
    }
    matched = true;
    fs::path directory = file.output_path.parent_path();
    while (is_same_or_under(directory, selected_path)) {
      DirectoryAggregate &aggregate = directories[directory];
      ++aggregate.file_count;
      aggregate.total_size_bytes += file.blob.size_bytes;
      if (directory == selected_path) {
        break;
      }
      directory = directory.parent_path();
    }
    if (!options.depth.has_value()
        || relative_depth(file.output_path, selected_path) <= *options.depth) {
      entries.push_back(ViewCatalogEntry{
          .path = file.output_path,
          .directory = false,
          .file_count = 1,
          .total_size_bytes = file.blob.size_bytes,
          .blob_id = file.blob.id,
      });
    }
  }
  for (const auto &[path, aggregate] : directories) {
    if (options.depth.has_value()
        && relative_depth(path, selected_path) > *options.depth) {
      continue;
    }
    entries.push_back(ViewCatalogEntry{
        .path = path,
        .directory = true,
        .file_count = aggregate.file_count,
        .total_size_bytes = aggregate.total_size_bytes,
        .blob_id = {},
    });
  }
  if (!matched && !selected_path.empty()) {
    return view_error(ErrorKind::ContextPathNotFound,
                      std::format("view path does not exist `{}`",
                                  selected_path.generic_string()));
  }
  std::ranges::sort(
      entries, [](const ViewCatalogEntry &left, const ViewCatalogEntry &right) {
        return std::tuple{left.path.generic_string(), !left.directory}
             < std::tuple{right.path.generic_string(), !right.directory};
      });
  record_quest_receipt_best_effort(store_.root(), *environment,
                                   QuestReceipt{
                                       .at = {},
                                       .operation = "tree",
                                       .environment_id = {},
                                       .derivation_id = {},
                                       .path = selected_path.empty()
                                                 ? "."
                                                 : selected_path.generic_string(),
                                       .profile = std::nullopt,
                                       .generation = std::nullopt,
                                       .mode = std::nullopt,
                                       .via = std::nullopt,
                                   });
  return entries;
}

ViewResult<ResolvedViewFile>
ViewService::read_environment_file(std::string_view environment_prefix,
                                   const fs::path &path) const {
  ViewResult<fs::path> validated = validate_view_path({
      .value = path.generic_string(),
      .locator = path.generic_string(),
  });
  if (!validated) {
    return std::unexpected(validated.error());
  }
  ViewResult<ViewEnvironment> environment =
      resolve_environment(environment_prefix);
  if (!environment) {
    return std::unexpected(environment.error());
  }
  const auto file =
      std::ranges::find(environment->derivation.resolved_files, *validated,
                        &ResolvedViewFile::output_path);
  if (file == environment->derivation.resolved_files.end()) {
    return view_error(ErrorKind::ContextPathNotFound,
                      std::format("view file does not exist `{}`",
                                  validated->generic_string()));
  }
  record_quest_receipt_best_effort(store_.root(), *environment,
                                   QuestReceipt{
                                       .at = {},
                                       .operation = "cat",
                                       .environment_id = {},
                                       .derivation_id = {},
                                       .path = validated->generic_string(),
                                       .profile = std::nullopt,
                                       .generation = std::nullopt,
                                       .mode = std::nullopt,
                                       .via = std::nullopt,
                                   });
  return *file;
}

void ViewService::record_activation(
    const ViewEnvironment &environment, const ViewActivation &activation,
    ActivationMode mode, std::optional<std::string_view> via) const {
  record_quest_receipt_best_effort(
      store_.root(), environment,
      QuestReceipt{
          .at = {},
          .operation = "activate",
          .environment_id = {},
          .derivation_id = {},
          .path = std::nullopt,
          .profile = activation.profile,
          .generation = activation.generation,
          .mode = std::string(to_string(mode)),
          .via = via.has_value()
                   ? std::optional<std::string>(std::in_place, *via)
                   : std::nullopt,
      });
}

ViewResult<ViewActivation>
ViewService::activate(std::string_view environment_prefix,
                      const ActivationOptions &options) const {
  if (options.into.empty()) {
    return view_error(ErrorKind::InvalidArgument,
                      "activation target path must not be empty");
  }
  const ViewVoid profile_checked = check_profile_name(options.profile);
  if (!profile_checked) {
    return std::unexpected(profile_checked.error());
  }
  ViewResult<ViewEnvironment> environment =
      resolve_environment(environment_prefix);
  if (!environment) {
    return std::unexpected(environment.error());
  }
  const fs::path profile_dir = profile_dir_for(store_.root(), options.profile);
  const fs::path lock = profile_dir / std::string(kProfileLockName);
  const ViewVoid locked = acquire_profile_lock(profile_dir);
  if (!locked) {
    return std::unexpected(locked.error());
  }
  const memix::scope_exit release_lock(
      [lock] { std::ignore = fs::remove(lock); });
  ViewResult<fs::path> workspace_lock = acquire_workspace_lock(options.into);
  if (!workspace_lock) {
    return std::unexpected(workspace_lock.error());
  }
  const memix::scope_exit release_workspace_lock(
      [lock = *workspace_lock] { std::ignore = fs::remove(lock); });
  ViewResult<fs::path> link_target =
      preflight_activation_workspace(store_, options);
  if (!link_target) {
    return std::unexpected(link_target.error());
  }

  ViewResult<CurrentGeneration> generation = ensure_current_generation(
      profile_dir, *environment, options.mode, options.into);
  if (!generation) {
    return std::unexpected(generation.error());
  }
  const auto fail_after_switch =
      [&](ViewError error) -> ViewResult<ViewActivation> {
    if (generation->switched) {
      error = restore_activation_after_failure(
          profile_dir, generation->previous, generation->number,
          std::move(error));
    }
    return std::unexpected(std::move(error));
  };

  bool materialized = false;
  if (options.mode == ActivationMode::Link) {
    ViewResult<bool> linked = ensure_workspace_link({
        .into = options.into,
        .target = *link_target,
        .force = options.force,
    });
    if (!linked) {
      return fail_after_switch(std::move(linked.error()));
    }
    materialized = *linked;
  } else {
    ViewResult<bool> synced = sync_copy_activation({
        .store = store_,
        .into = options.into,
        .tree_path = environment->derivation.tree_path,
        .metadata = generation->metadata,
        .force = options.force,
    });
    if (!synced) {
      return fail_after_switch(std::move(synced.error()));
    }
    materialized = *synced;
  }

  ViewActivation activation{
      .profile = options.profile,
      .generation = generation->number,
      .environment_id = environment->id,
      .derivation_id = environment->derivation.id,
      .workspace_path = options.into,
      .switched = generation->switched,
      .materialized = materialized,
  };
  record_activation(*environment, activation, options.mode);
  return activation;
}

ViewResult<ViewActivation>
ViewService::rollback(std::string_view profile,
                      std::optional<std::uint64_t> generation) const {
  const ViewVoid profile_checked = check_profile_name(profile);
  if (!profile_checked) {
    return std::unexpected(profile_checked.error());
  }
  const fs::path profile_dir = profile_dir_for(store_.root(), profile);
  const fs::path lock = profile_dir / std::string(kProfileLockName);
  const ViewVoid locked = acquire_existing_profile_lock(profile_dir);
  if (!locked) {
    return std::unexpected(locked.error());
  }
  const memix::scope_exit release_lock(
      [lock] { std::ignore = fs::remove(lock); });

  ViewResult<std::vector<std::uint64_t>> existing =
      scan_generations(profile_dir);
  if (!existing) {
    return std::unexpected(existing.error());
  }
  if (existing->empty()) {
    return view_error(ErrorKind::NotFound,
                      std::format("profile `{}` has no generations", profile));
  }
  ViewResult<std::optional<std::uint64_t>> current =
      read_current_generation(profile_dir);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (!current->has_value()) {
    return view_error(
        ErrorKind::InvalidData,
        std::format("profile `{}` has no current generation", profile));
  }

  ViewResult<std::uint64_t> target =
      select_rollback_target(*existing, **current, generation, profile);
  if (!target) {
    return std::unexpected(target.error());
  }

  ViewResult<GenerationMetadata> metadata =
      read_generation_metadata(profile_dir, *target);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  if (*target == **current) {
    ViewActivation activation{
        .profile = std::string(profile),
        .generation = *target,
        .environment_id = metadata->environment_id,
        .derivation_id = metadata->derivation_id,
        .workspace_path = fs::path(metadata->workspace),
        .switched = false,
        .materialized = false,
    };
    const ViewResult<ViewEnvironment> environment =
        resolve_environment(activation.environment_id);
    if (environment) {
      record_activation(*environment, activation, metadata->mode, "rollback");
    }
    return activation;
  }
  std::optional<fs::path> workspace_lock;
  if (metadata->mode == ActivationMode::Copy && !metadata->workspace.empty()) {
    ViewResult<fs::path> acquired = acquire_workspace_lock(metadata->workspace);
    if (!acquired) {
      return std::unexpected(acquired.error());
    }
    workspace_lock = std::move(*acquired);
  }
  const memix::scope_exit release_workspace_lock([&workspace_lock] {
    if (workspace_lock.has_value()) {
      std::ignore = fs::remove(*workspace_lock);
    }
  });
  const ViewVoid swapped = swap_current(profile_dir, *target);
  if (!swapped) {
    return std::unexpected(swapped.error());
  }

  ViewResult<bool> rematerialized =
      rematerialize_copy_workspace(store_, *metadata);
  if (!rematerialized) {
    return std::unexpected(restore_current_after_failure(
        profile_dir, *current, std::move(rematerialized.error())));
  }
  ViewActivation activation{
      .profile = std::string(profile),
      .generation = *target,
      .environment_id = metadata->environment_id,
      .derivation_id = metadata->derivation_id,
      .workspace_path = fs::path(metadata->workspace),
      .switched = true,
      .materialized = *rematerialized,
  };
  const ViewResult<ViewEnvironment> environment =
      resolve_environment(activation.environment_id);
  if (environment) {
    record_activation(*environment, activation, metadata->mode, "rollback");
  }
  return activation;
}

ViewResult<std::vector<ViewGenerationInfo>>
ViewService::generations(std::string_view profile) const {
  const ViewVoid profile_checked = check_profile_name(profile);
  if (!profile_checked) {
    return std::unexpected(profile_checked.error());
  }
  const fs::path profile_dir = profile_dir_for(store_.root(), profile);
  const memix::FsResult<bool> profile_exists = fs::exists(profile_dir);
  if (!profile_exists) {
    return view_error(profile_exists.error());
  }
  if (!*profile_exists) {
    return view_error(ErrorKind::NotFound,
                      std::format("profile `{}` does not exist", profile));
  }
  const fs::path lock = profile_dir / std::string(kProfileLockName);
  const ViewVoid locked = acquire_existing_profile_lock(profile_dir);
  if (!locked) {
    return std::unexpected(locked.error());
  }
  const memix::scope_exit release_lock(
      [lock] { std::ignore = fs::remove(lock); });
  ViewResult<std::vector<std::uint64_t>> existing =
      scan_generations(profile_dir);
  if (!existing) {
    return std::unexpected(existing.error());
  }
  ViewResult<std::optional<std::uint64_t>> current =
      read_current_generation(profile_dir);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (!existing->empty() && !current->has_value()) {
    return view_error(
        ErrorKind::InvalidData,
        std::format("profile `{}` has generations but no current generation",
                    profile));
  }
  if (current->has_value() && !std::ranges::contains(*existing, **current)) {
    return view_error(
        ErrorKind::InvalidData,
        std::format("profile `{}` current generation {} does not exist",
                    profile, **current));
  }
  std::vector<ViewGenerationInfo> infos;
  infos.reserve(existing->size());
  for (const std::uint64_t number : *existing) {
    ViewResult<GenerationMetadata> metadata =
        read_generation_metadata(profile_dir, number);
    if (!metadata) {
      return std::unexpected(metadata.error());
    }
    infos.push_back(ViewGenerationInfo{
        .number = number,
        .environment_id = metadata->environment_id,
        .derivation_id = metadata->derivation_id,
        .activated_at = metadata->activated_at,
        .mode = metadata->mode,
        .current = current->has_value() && **current == number,
    });
  }
  return infos;
}

ViewResult<ViewActivationStatus>
ViewService::activation_status(const fs::path &into) const {
  ViewResult<std::optional<ResolvedWorkspaceActivation>> resolved =
      resolve_workspace_activation(store_, into);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  if (!resolved->has_value()) {
    return ViewActivationStatus{
        .active = false,
        .mode = ActivationMode::Link,
        .profile = {},
        .generation = 0,
        .environment_id = {},
        .derivation_id = {},
        .stale = false,
        .workspace_path = into,
    };
  }
  const ResolvedWorkspaceActivation &activation = **resolved;
  return ViewActivationStatus{
      .active = true,
      .mode = activation.mode,
      .profile = activation.profile,
      .generation = activation.generation,
      .environment_id = activation.metadata.environment_id,
      .derivation_id = activation.metadata.derivation_id,
      .stale = activation.stale,
      .workspace_path = into,
  };
}

ViewResult<ViewDeactivation>
ViewService::deactivate(const fs::path &into, bool wipe_profile) const {
  ViewResult<std::optional<ResolvedWorkspaceActivation>> resolved =
      resolve_workspace_activation(store_, into);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  if (!resolved->has_value()) {
    return view_error(
        ErrorKind::NotFound,
        std::format("no memix activation at `{}`", into.string()));
  }
  ResolvedWorkspaceActivation activation = std::move(**resolved);
  const std::string profile = activation.profile;
  const fs::path profile_dir = profile_dir_for(store_.root(), profile);
  const fs::path lock = profile_dir / std::string(kProfileLockName);
  bool lock_held = false;
  const memix::scope_exit release_lock([&lock_held, lock] {
    if (lock_held) {
      std::ignore = fs::remove(lock);
    }
  });
  ViewResult<ResolvedWorkspaceActivation> locked_activation =
      lock_profile_for_deactivation({
          .store = std::cref(store_),
          .into = std::cref(into),
          .profile_dir = std::cref(profile_dir),
          .expected_activation = std::cref(activation),
          .lock_held = std::ref(lock_held),
      });
  if (!locked_activation) {
    return std::unexpected(locked_activation.error());
  }
  activation = std::move(*locked_activation);
  ViewResult<fs::path> workspace_lock = acquire_workspace_lock(into);
  if (!workspace_lock) {
    return std::unexpected(workspace_lock.error());
  }
  const memix::scope_exit release_workspace_lock(
      [lock = *workspace_lock] { std::ignore = fs::remove(lock); });
  ViewResult<ResolvedWorkspaceActivation> workspace_activation =
      revalidate_workspace_activation(store_, into, activation);
  if (!workspace_activation) {
    return std::unexpected(workspace_activation.error());
  }
  activation = std::move(*workspace_activation);
  const ViewVoid workspace_removed =
      remove_workspace_activation(into, activation.mode);
  if (!workspace_removed) {
    return std::unexpected(workspace_removed.error());
  }
  bool wiped = false;
  if (wipe_profile) {
    const ViewVoid profile_wiped = wipe_locked_profile(profile_dir, lock_held);
    if (!profile_wiped) {
      return std::unexpected(profile_wiped.error());
    }
    wiped = true;
  }
  return ViewDeactivation{
      .profile = profile,
      .workspace_path = into,
      .wiped_profile = wiped,
  };
}

std::string default_profile_name(const fs::path &workspace_dir) {
  const std::string hash = sha256_text(workspace_dir.generic_string());
  return std::format(
      "{}{}", kWorkspaceProfilePrefix,
      std::string_view(hash).substr(0, kWorkspaceProfileHashLength));
}

ViewResult<std::string> validated_profile_name(std::string_view profile) {
  const ViewVoid basic_checked = check_profile_name(profile);
  if (!basic_checked) {
    return std::unexpected(basic_checked.error());
  }
  if (profile.starts_with(kWorkspaceProfilePrefix)) {
    return view_error(
        ErrorKind::InvalidArgument,
        std::format("profile name `{}` uses the reserved `{}` prefix", profile,
                    kWorkspaceProfilePrefix));
  }
  return std::string(profile);
}

ViewResult<std::string> view_ir_json(const ViewIr &ir) {
  simdjson::builder::string_builder builder;
  builder.start_object();
  if (ir.schema == kIrSchemaV2) {
    const std::vector<TypedIrInputJson> inputs =
        ir.inputs
        | std::views::transform(typed_ir_input_json_for)
        | std::ranges::to<std::vector>();
    builder.append_key_value<"schema">(kIrSchemaV2);
    builder.append_comma();
    builder.append_key_value<"inputs">(inputs);
  } else {
    const std::vector<IrInputJson> inputs =
        ir.inputs
        | std::views::transform(ir_input_json_for)
        | std::ranges::to<std::vector>();
    builder.append_key_value<"schema">(kIrSchema);
    builder.append_comma();
    builder.append_key_value<"inputs">(inputs);
  }
  builder.append_comma();
  builder.append_key_value<"target">(ir_target_json_for(ir.target));
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<std::string> view_derivation_json(const ViewDerivation &derivation) {
  ViewResult<std::string> created_at = utc_timestamp();
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  if (derivation.environment_inputs.empty()) {
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
    builder.append_key_value<"created_at">(*created_at);
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

  const std::vector<PassJson> passes{
      PassJson{.name = "resolve-refs", .version = "v1"},
      PassJson{.name = "expand-dirs", .version = "v1"},
      PassJson{.name = "compose-views", .version = "v1"},
      PassJson{.name = "pack-file-tree", .version = "v2"},
  };
  const std::vector<ResolvedInputJson> inputs =
      resolved_input_json(derivation.resolved_files);
  const std::vector<ResolvedEnvironmentInputJson> environment_inputs =
      resolved_environment_input_json(derivation.environment_inputs);
  const std::vector<OutputJson> outputs = output_json(derivation.outputs);

  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">(kDerivationSchemaV2);
  builder.append_comma();
  builder.append_key_value<"derivation_id">(derivation.id);
  builder.append_comma();
  builder.append_key_value<"ir_hash">(derivation.ir_hash);
  builder.append_comma();
  builder.append_key_value<"created_at">(*created_at);
  builder.append_comma();
  builder.append_key_value<"target">(memix::to_string(derivation.target));
  builder.append_comma();
  builder.append_key_value<"operator_version">(kFileTreeOperatorVersionV2);
  builder.append_comma();
  builder.append_key_value<"passes">(passes);
  builder.append_comma();
  builder.append_key_value<"policy_snapshot">(std::optional<std::string>{});
  builder.append_comma();
  builder.append_key_value<"resolved_inputs">(inputs);
  builder.append_comma();
  builder.append_key_value<"resolved_view_inputs">(environment_inputs);
  builder.append_comma();
  builder.append_key_value<"outputs">(outputs);
  builder.end_object();
  return json_from_builder(builder);
}

ViewResult<std::string>
view_environment_json(const ViewEnvironment &environment) {
  const std::vector<BindingJson> bindings = binding_json(environment.bindings);
  if (environment.derivation.environment_inputs.empty()) {
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
  const ClosureV2Json closure{
      .environments = environment.closure.environments,
      .derivations = environment.closure.derivations,
      .artifacts = environment.closure.artifacts,
      .raw_objects = environment.closure.raw_objects,
  };
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema">(kEnvironmentSchemaV2);
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
