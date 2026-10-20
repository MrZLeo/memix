#include "memix/view.hpp"

#include "cli_common.hpp"
#include "commands.hpp"
#include "detail/error.hpp"
#include "detail/json.hpp"

#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <argum/data.h>
#include <argum/expected.h>
#include <argum/parser.h>
#include <simdjson.h>
#include <spdlog/logger.h>

namespace fs = memix::fs;

namespace {

enum class ViewCommand : std::uint8_t {
  None,
  List,
  Tree,
  Cat,
  Select,
  Compose,
  Ls,
  Define,
  Find,
  Resolve,
  Freshness,
  Quests,
  Stats,
  Show,
  Activate,
  Generations,
  Rollback,
  Deactivate,
  Status,
};

struct ViewCliOptions {
  std::optional<std::size_t> tree_depth;
  std::vector<std::string> sources;
  std::vector<std::string> mounts;
  std::vector<std::string> find_terms;
  std::optional<fs::path> covers;
  std::string intent;
  std::string name;
  std::string quest_id;
  std::string tree_reference;
  fs::path tree_path;
  std::string cat_reference;
  fs::path cat_path;
  std::string target_name = "file_tree";
  std::string quest_kind = "selection";
  std::string object_kind = "environment";
  std::string show_reference;
  std::string activate_environment_id;
  std::string into_path = ".memory";
  std::string profile_name;
  std::string mode_name = "link";
  memix::cli_detail::CommonOptions common;
  std::optional<std::string> rollback_to;
  ViewCommand command = ViewCommand::None;
  bool list_json = false;
  bool tree_json = false;
  bool materialization_json = false;
  bool define_json = false;
  bool find_json = false;
  bool resolve_json = false;
  bool freshness_json = false;
  bool quests_json = false;
  bool stats_json = false;
  bool intent_provided = false;
  bool name_provided = false;
  bool show_json = false;
  bool force = false;
  bool wipe_profile = false;
};

struct ViewEnvironmentInfoJson {
  std::string id;
  std::size_t binding_count = 0;
  std::size_t file_count = 0;
  std::uintmax_t size_bytes = 0;
  std::size_t upstream_environment_count = 0;
};

struct ViewCatalogEntryJson {
  std::string path;
  std::string kind;
  std::size_t file_count = 0;
  std::uintmax_t size_bytes = 0;
  std::string blob;
};

struct ViewReferenceJson {
  std::string kind;
  std::string id;
};

struct QuestInfoJson {
  std::string ir_hash;
  std::string result_kind;
  std::string intent;
  std::string name;
  std::string created_at;
  std::size_t resolution_count = 0;
  std::string last_resolved_at;
  std::optional<ViewReferenceJson> latest_result;
  std::size_t read_count = 0;
  std::string last_read_at;
  std::size_t activation_count = 0;
  std::string last_activated_at;
};

struct QuestDefinitionJson {
  std::string ir_hash;
  std::string result_kind;
  std::string intent;
  std::string name;
  std::string created_at;
  bool existed = false;
};

struct QuestFreshnessInputJson {
  std::string locator;
  std::string recorded_generation;
  std::optional<std::string> current_generation;
  bool pinned = false;
  bool fresh = true;
};

struct QuestFreshnessJson {
  std::string ir_hash;
  ViewReferenceJson result;
  std::string resolved_at;
  std::vector<QuestFreshnessInputJson> inputs;
  bool stale = false;
};

struct QuestEnvironmentReadsJson {
  std::string environment_id;
  std::size_t read_count = 0;
};

struct QuestDerivationReadsJson {
  std::string derivation_id;
  std::size_t read_count = 0;
};

struct QuestProfileActivationsJson {
  std::string profile;
  std::size_t activation_count = 0;
};

struct QuestStatsJson {
  std::string ir_hash;
  std::size_t resolution_count = 0;
  std::string last_resolved_at;
  std::size_t activation_count = 0;
  std::string first_activated_at;
  std::string last_activated_at;
  std::vector<QuestProfileActivationsJson> activations_by_profile;
  std::size_t read_count = 0;
  std::string first_read_at;
  std::string last_read_at;
  std::size_t cat_count = 0;
  std::size_t tree_count = 0;
  std::vector<QuestEnvironmentReadsJson> reads_by_environment;
  std::vector<QuestDerivationReadsJson> reads_by_derivation;
  std::size_t ls_count = 0;
};

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ViewReferenceJson &reference) {
  builder.start_object();
  builder.template append_key_value<"kind">(reference.kind);
  builder.append_comma();
  builder.template append_key_value<"id">(reference.id);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ViewEnvironmentInfoJson &environment) {
  builder.start_object();
  builder.template append_key_value<"id">(environment.id);
  builder.append_comma();
  builder.template append_key_value<"binding_count">(environment.binding_count);
  builder.append_comma();
  builder.template append_key_value<"file_count">(environment.file_count);
  builder.append_comma();
  builder.template append_key_value<"size_bytes">(environment.size_bytes);
  builder.append_comma();
  builder.template append_key_value<"upstream_environment_count">(
      environment.upstream_environment_count);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ViewCatalogEntryJson &entry) {
  builder.start_object();
  builder.template append_key_value<"path">(entry.path);
  builder.append_comma();
  builder.template append_key_value<"kind">(entry.kind);
  builder.append_comma();
  builder.template append_key_value<"file_count">(entry.file_count);
  builder.append_comma();
  builder.template append_key_value<"size_bytes">(entry.size_bytes);
  builder.append_comma();
  builder.template append_key_value<"blob">(entry.blob);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const QuestInfoJson &quest) {
  builder.start_object();
  builder.template append_key_value<"result_kind">(quest.result_kind);
  builder.append_comma();
  builder.template append_key_value<"ir_hash">(quest.ir_hash);
  builder.append_comma();
  builder.template append_key_value<"intent">(quest.intent);
  builder.append_comma();
  builder.template append_key_value<"name">(quest.name);
  builder.append_comma();
  builder.template append_key_value<"created_at">(quest.created_at);
  builder.append_comma();
  builder.template append_key_value<"resolution_count">(quest.resolution_count);
  builder.append_comma();
  builder.template append_key_value<"last_resolved_at">(quest.last_resolved_at);
  builder.append_comma();
  builder.template append_key_value<"latest_result">(quest.latest_result);
  builder.append_comma();
  builder.template append_key_value<"read_count">(quest.read_count);
  builder.append_comma();
  builder.template append_key_value<"last_read_at">(quest.last_read_at);
  builder.append_comma();
  builder.template append_key_value<"activation_count">(quest.activation_count);
  builder.append_comma();
  builder.template append_key_value<"last_activated_at">(
      quest.last_activated_at);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const QuestDefinitionJson &quest) {
  builder.start_object();
  builder.template append_key_value<"result_kind">(quest.result_kind);
  builder.append_comma();
  builder.template append_key_value<"ir_hash">(quest.ir_hash);
  builder.append_comma();
  builder.template append_key_value<"intent">(quest.intent);
  builder.append_comma();
  builder.template append_key_value<"name">(quest.name);
  builder.append_comma();
  builder.template append_key_value<"created_at">(quest.created_at);
  builder.append_comma();
  builder.template append_key_value<"existed">(quest.existed);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const QuestFreshnessInputJson &input) {
  builder.start_object();
  builder.template append_key_value<"locator">(input.locator);
  builder.append_comma();
  builder.template append_key_value<"recorded_generation">(
      input.recorded_generation);
  builder.append_comma();
  builder.template append_key_value<"current_generation">(
      input.current_generation);
  builder.append_comma();
  builder.template append_key_value<"pinned">(input.pinned);
  builder.append_comma();
  builder.template append_key_value<"fresh">(input.fresh);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const QuestFreshnessJson &freshness) {
  builder.start_object();
  builder.template append_key_value<"ir_hash">(freshness.ir_hash);
  builder.append_comma();
  builder.template append_key_value<"result">(freshness.result);
  builder.append_comma();
  builder.template append_key_value<"resolved_at">(freshness.resolved_at);
  builder.append_comma();
  builder.template append_key_value<"inputs">(freshness.inputs);
  builder.append_comma();
  builder.template append_key_value<"stale">(freshness.stale);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const QuestEnvironmentReadsJson &reads) {
  builder.start_object();
  builder.template append_key_value<"environment_id">(reads.environment_id);
  builder.append_comma();
  builder.template append_key_value<"read_count">(reads.read_count);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const QuestDerivationReadsJson &reads) {
  builder.start_object();
  builder.template append_key_value<"derivation_id">(reads.derivation_id);
  builder.append_comma();
  builder.template append_key_value<"read_count">(reads.read_count);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const QuestProfileActivationsJson &activations) {
  builder.start_object();
  builder.template append_key_value<"profile">(activations.profile);
  builder.append_comma();
  builder.template append_key_value<"activation_count">(
      activations.activation_count);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const QuestStatsJson &stats) {
  builder.start_object();
  builder.template append_key_value<"ir_hash">(stats.ir_hash);
  builder.append_comma();
  builder.template append_key_value<"resolution_count">(stats.resolution_count);
  builder.append_comma();
  builder.template append_key_value<"last_resolved_at">(stats.last_resolved_at);
  builder.append_comma();
  builder.template append_key_value<"activation_count">(stats.activation_count);
  builder.append_comma();
  builder.template append_key_value<"first_activated_at">(
      stats.first_activated_at);
  builder.append_comma();
  builder.template append_key_value<"last_activated_at">(
      stats.last_activated_at);
  builder.append_comma();
  builder.template append_key_value<"activations_by_profile">(
      stats.activations_by_profile);
  builder.append_comma();
  builder.template append_key_value<"read_count">(stats.read_count);
  builder.append_comma();
  builder.template append_key_value<"first_read_at">(stats.first_read_at);
  builder.append_comma();
  builder.template append_key_value<"last_read_at">(stats.last_read_at);
  builder.append_comma();
  builder.template append_key_value<"cat_count">(stats.cat_count);
  builder.append_comma();
  builder.template append_key_value<"tree_count">(stats.tree_count);
  builder.append_comma();
  builder.template append_key_value<"reads_by_environment">(
      stats.reads_by_environment);
  builder.append_comma();
  builder.template append_key_value<"reads_by_derivation">(
      stats.reads_by_derivation);
  builder.append_comma();
  builder.template append_key_value<"ls_count">(stats.ls_count);
  builder.end_object();
}

memix::cli_detail::Result<memix::ViewService>
make_service(const ViewCliOptions &options) {
  const memix::cli_detail::Result<fs::path> root =
      memix::cli_detail::resolve_store_root(options.common.store_root);
  if (!root) {
    return std::unexpected(root.error());
  }
  return memix::ViewService(memix::Store(*root));
}

memix::cli_detail::Result<memix::ViewTargetKind>
require_selection_target(std::string_view target_name) {
  const std::optional<memix::ViewTargetKind> target =
      memix::parse_view_target_kind(target_name);
  if (!target.has_value() || *target != memix::ViewTargetKind::FileTree) {
    return memix::cli_detail::unexpected_error(std::format(
        "unsupported build target `{}`; expected file_tree", target_name));
  }
  return *target;
}

template <typename Values>
memix::cli_detail::VoidResult print_json(const Values &values) {
  simdjson::builder::string_builder builder;
  builder.append(values);
  const memix::detail::CommonResult<std::string> json =
      memix::detail::json_from_builder(builder);
  if (!json) {
    return memix::cli_detail::unexpected_error(json.error().message);
  }
  std::println("{}", *json);
  return {};
}

QuestInfoJson quest_info_json_for(const memix::QuestInfo &info) {
  return QuestInfoJson{
      .ir_hash = info.ir_hash,
      .result_kind = std::string(memix::to_string(info.result_kind)),
      .intent = info.intent,
      .name = info.name,
      .created_at = info.created_at,
      .resolution_count = info.resolution_count,
      .last_resolved_at = info.last_resolved_at,
      .latest_result =
          info.latest_result.transform([](const memix::ViewReference &result) {
            return ViewReferenceJson{
                .kind = std::string(memix::to_string(result.kind)),
                .id = result.id,
            };
          }),
      .read_count = info.read_count,
      .last_read_at = info.last_read_at,
      .activation_count = info.activation_count,
      .last_activated_at = info.last_activated_at,
  };
}

memix::cli_detail::Result<memix::AuthoredViewExpression>
expression_from_cli(const memix::ViewService &service,
                    const ViewCliOptions &options,
                    memix::ViewTargetKind target) {
  if (!options.mounts.empty()) {
    return memix::cli_detail::unexpected_error(
        "--mount requires a composition, not a selection");
  }
  auto expression = service.expression_from_sources(options.sources, target);
  if (!expression)
    return memix::cli_detail::unexpected_error(expression.error());
  return std::move(*expression);
}

std::string truncate_table_cell(std::string_view value, std::size_t width) {
  constexpr std::string_view ellipsis = "...";
  constexpr unsigned char utf8_continuation_mask = 0xC0U;
  constexpr unsigned char utf8_continuation = 0x80U;
  if (value.size() <= width) {
    return std::string(value);
  }
  std::size_t prefix_size = width - ellipsis.size();
  while (prefix_size > 0
         && (static_cast<unsigned char>(value[prefix_size])
             & utf8_continuation_mask)
                == utf8_continuation) {
    --prefix_size;
  }
  return std::format("{}{}", value.substr(0, prefix_size), ellipsis);
}

void print_quest_table(const std::vector<memix::QuestInfo> &quests) {
  constexpr std::size_t intent_width = 28;
  constexpr std::size_t name_width = 24;
  std::println(
      "{:<66}  {:<28}  {:<24}  {:>12}  {:>8}  {:>8}  {:<22}  {:<22}  {:<15}",
      "QUEST", "INTENT", "NAME", "RESOLUTIONS", "READS", "ACTS",
      "LAST_RESOLVED", "LAST_READ", "RESULT");
  for (const memix::QuestInfo &quest : quests) {
    std::println(
        "{:<66}  {:<28}  {:<24}  {:>12}  {:>8}  {:>8}  {:<22}  {:<22}  {:<15}",
        quest.ir_hash, truncate_table_cell(quest.intent, intent_width),
        truncate_table_cell(quest.name, name_width), quest.resolution_count,
        quest.read_count, quest.activation_count, quest.last_resolved_at,
        quest.last_read_at,
        !quest.latest_result
            ? "-"
            : std::format(
                  "{}:{}",
                  quest.latest_result->kind == memix::ViewObjectKind::Derivation
                      ? "D"
                      : "E",
                  memix::cli_detail::short_id(quest.latest_result->id)));
  }
}

memix::cli_detail::VoidResult
print_quests(const std::vector<memix::QuestInfo> &quests, bool json) {
  if (json) {
    const std::vector<QuestInfoJson> values =
        quests
        | std::views::transform(quest_info_json_for)
        | std::ranges::to<std::vector>();
    return print_json(values);
  }
  print_quest_table(quests);
  return {};
}

memix::cli_detail::VoidResult
list_derivations(const memix::ViewService &service, bool json) {
  auto derivations = service.list_derivations();
  if (!derivations)
    return memix::cli_detail::unexpected_error(derivations.error());
  if (!json)
    std::println("{:<66}{:>8}{:>12}", "DERIVATION", "FILES", "SIZE_BYTES");
  simdjson::builder::string_builder builder;
  builder.start_array();
  bool first = true;
  for (const auto &derivation : *derivations) {
    std::uintmax_t bytes = 0;
    for (const auto &file : derivation.resolved_files)
      bytes += file.blob.size_bytes;
    if (!json)
      std::println("{:<66}{:>8}{:>12}", derivation.id,
                   derivation.resolved_files.size(), bytes);
    if (!first)
      builder.append_comma();
    first = false;
    builder.start_object();
    builder.append_key_value<"id">(derivation.id);
    builder.append_comma();
    builder.append_key_value<"kind">("derivation");
    builder.append_comma();
    builder.append_key_value<"quest">(derivation.ir_hash);
    builder.append_comma();
    builder.append_key_value<"file_count">(derivation.resolved_files.size());
    builder.append_comma();
    builder.append_key_value<"size_bytes">(bytes);
    builder.end_object();
  }
  builder.end_array();
  if (json) {
    auto text = memix::detail::json_from_builder(builder);
    if (!text)
      return memix::cli_detail::unexpected_error(text.error().message);
    std::println("{}", *text);
  }
  return {};
}

memix::cli_detail::VoidResult run_list(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  if (options.object_kind == "derivation")
    return list_derivations(*service, options.list_json);
  if (options.object_kind != "environment")
    return memix::cli_detail::unexpected_error(
        "--kind must be derivation or environment");
  const memix::ViewResult<std::vector<memix::ViewEnvironmentInfo>>
      environments = service->list_environments();
  if (!environments) {
    return memix::cli_detail::unexpected_error(environments.error());
  }
  if (options.list_json) {
    const std::vector<ViewEnvironmentInfoJson> json =
        *environments
        | std::views::transform([](const memix::ViewEnvironmentInfo &info) {
            return ViewEnvironmentInfoJson{
                .id = info.id,
                .binding_count = info.binding_count,
                .file_count = info.file_count,
                .size_bytes = info.total_size_bytes,
                .upstream_environment_count = info.upstream_environment_count,
            };
          })
        | std::ranges::to<std::vector>();
    return print_json(json);
  }
  std::println("{:<66}{:<66}{:>8}{:>12}{:>10}", "ENVIRONMENT", "BINDINGS",
               "FILES", "SIZE_BYTES", "UPSTREAMS");
  for (const memix::ViewEnvironmentInfo &environment : *environments) {
    std::println("{:<66}{:<66}{:>8}{:>12}{:>10}", environment.id,
                 environment.binding_count, environment.file_count,
                 environment.total_size_bytes,
                 environment.upstream_environment_count);
  }
  return {};
}

memix::cli_detail::Result<memix::ViewReference>
view_reference(std::string_view value) {
  auto kind = memix::ViewObjectKind::Environment;
  if (value.starts_with("derivation:")) {
    kind = memix::ViewObjectKind::Derivation;
    value.remove_prefix(std::string_view("derivation:").size());
  } else if (value.starts_with("environment:")) {
    value.remove_prefix(std::string_view("environment:").size());
  } else if (value.contains(':')) {
    return memix::cli_detail::unexpected_error(
        "expected derivation:ID or environment:ID");
  }
  if (value.empty())
    return memix::cli_detail::unexpected_error("view id is required");
  return memix::ViewReference{.kind = kind, .id = std::string(value)};
}

memix::cli_detail::Result<std::vector<memix::ViewEnvironmentBinding>>
bindings_from_cli(const ViewCliOptions &options) {
  if (!options.sources.empty())
    return memix::cli_detail::unexpected_error(
        "compose does not accept --source");
  std::vector<memix::ViewEnvironmentBinding> bindings;
  for (const auto &value : options.mounts) {
    const auto equal = value.find('=');
    const auto id = value.substr(0, equal);
    const auto mount =
        equal == std::string::npos ? "" : value.substr(equal + 1);
    if (id.empty() || mount.contains('=')) {
      return memix::cli_detail::unexpected_error(
          "--mount expects DERIVATION[=MOUNT]");
    }
    bindings.push_back({
        .mount = fs::path(mount),
        .derivation_id = id,
        .output_role = "file_tree",
    });
  }
  return bindings;
}

memix::cli_detail::VoidResult
print_resolution(const memix::ViewResolution &resolution, bool json) {
  if (!json) {
    std::println("{}: {}", memix::to_string(resolution.result.kind),
                 resolution.result.id);
    std::println("quest: {}", resolution.ir_hash);
    std::println("inserted: {}", resolution.inserted);
    return {};
  }
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"quest">(resolution.ir_hash);
  builder.append_raw(",\"result\":{");
  builder.append_key_value<"kind">(memix::to_string(resolution.result.kind));
  builder.append_comma();
  builder.append_key_value<"id">(resolution.result.id);
  builder.end_object();
  builder.append_comma();
  builder.append_key_value<"inserted">(resolution.inserted);
  builder.end_object();
  auto text = memix::detail::json_from_builder(builder);
  if (!text)
    return memix::cli_detail::unexpected_error(text.error().message);
  std::println("{}", *text);
  return {};
}

memix::cli_detail::VoidResult run_tree(const ViewCliOptions &options) {
  if (options.tree_reference.empty()) {
    return memix::cli_detail::unexpected_error("tree expects a typed view id");
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  auto reference = view_reference(options.tree_reference);
  if (!reference)
    return std::unexpected(reference.error());
  const auto entries =
      options.command == ViewCommand::Ls
          ? service->view_ls(*reference, options.tree_path)
          : service->view_tree(*reference, {
                                               .path = options.tree_path,
                                               .depth = options.tree_depth,
                                           });
  if (!entries) {
    return memix::cli_detail::unexpected_error(entries.error());
  }
  if (options.tree_json) {
    const std::vector<ViewCatalogEntryJson> json =
        *entries
        | std::views::transform([](const memix::ViewCatalogEntry &entry) {
            return ViewCatalogEntryJson{
                .path = entry.path.generic_string(),
                .kind = entry.directory ? "directory" : "file",
                .file_count = entry.file_count,
                .size_bytes = entry.total_size_bytes,
                .blob = entry.blob_id,
            };
          })
        | std::ranges::to<std::vector>();
    return print_json(json);
  }
  std::println("{:<11}{:>8}{:>12}  {}", "KIND", "FILES", "SIZE_BYTES", "PATH");
  for (const memix::ViewCatalogEntry &entry : *entries) {
    std::println("{:<11}{:>8}{:>12}  {}",
                 entry.directory ? "directory" : "file", entry.file_count,
                 entry.total_size_bytes,
                 entry.path.empty() ? "." : entry.path.generic_string());
  }
  return {};
}

memix::cli_detail::VoidResult run_cat(const ViewCliOptions &options) {
  if (options.cat_reference.empty()) {
    return memix::cli_detail::unexpected_error("cat expects a typed view id");
  }
  if (options.cat_path.empty()) {
    return memix::cli_detail::unexpected_error("cat expects a view path");
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  auto reference = view_reference(options.cat_reference);
  if (!reference)
    return std::unexpected(reference.error());
  auto file = service->read_output(*reference, options.cat_path);
  if (!file)
    return memix::cli_detail::unexpected_error(file.error());
  std::print("{}", file->contents);
  return {};
}

void print_view_environment_summary(const memix::ViewEnvironment &environment) {
  std::println("view_environment_id: {}", environment.id);
  std::println("manifest: {}", environment.manifest_path.string());
  std::println("inserted: {}", environment.inserted ? "true" : "false");
  std::println("bindings: {}", environment.bindings.size());
  for (const memix::ViewEnvironmentBinding &binding : environment.bindings) {
    std::println("  {} <- {}:{}", binding.mount.generic_string(),
                 memix::cli_detail::short_id(binding.derivation_id),
                 binding.output_role);
  }
  std::println("closure_derivations: {}",
               environment.closure.derivations.size());
  std::println("closure_artifacts: {}", environment.closure.artifacts.size());
  std::println("closure_raw_objects: {}",
               environment.closure.raw_objects.size());
}

memix::cli_detail::VoidResult
run_materialization(const ViewCliOptions &options) {
  auto service = make_service(options);
  if (!service)
    return std::unexpected(service.error());
  const memix::BuildOptions metadata{
      .intent = options.intent,
      .name = options.name,
  };
  if (options.command == ViewCommand::Compose) {
    auto bindings = bindings_from_cli(options);
    if (!bindings)
      return std::unexpected(bindings.error());
    auto result = service->compose(std::move(*bindings), metadata);
    if (!result)
      return memix::cli_detail::unexpected_error(result.error());
    return print_resolution(
        {
            .result =
                {
                    .kind = memix::ViewObjectKind::Environment,
                    .id = result->id,
                },
            .ir_hash = result->ir_hash,
            .inserted = result->inserted,
        },
        options.materialization_json);
  }
  auto target = require_selection_target(options.target_name);
  if (!target)
    return std::unexpected(target.error());
  auto expression = expression_from_cli(*service, options, *target);
  if (!expression)
    return std::unexpected(expression.error());
  auto result = service->select(*expression, metadata);
  if (!result)
    return memix::cli_detail::unexpected_error(result.error());
  return print_resolution(
      {
          .result =
              {
                  .kind = memix::ViewObjectKind::Derivation,
                  .id = result->id,
              },
          .ir_hash = result->ir_hash,
          .inserted = result->inserted,
      },
      options.materialization_json);
}

memix::cli_detail::VoidResult run_define(const ViewCliOptions &options) {
  auto service = make_service(options);
  if (!service)
    return std::unexpected(service.error());
  const memix::QuestDefinitionOptions metadata{
      .intent = options.intent_provided
                  ? std::optional<std::string>{options.intent}
                  : std::nullopt,
      .name = options.name_provided ? std::optional<std::string>{options.name}
                                    : std::nullopt,
  };
  memix::ViewResult<memix::QuestDefinition> definition;
  if (options.quest_kind == "composition") {
    auto bindings = bindings_from_cli(options);
    if (!bindings)
      return std::unexpected(bindings.error());
    definition = service->define_composition(std::move(*bindings), metadata);
  } else if (options.quest_kind == "selection") {
    auto expression =
        expression_from_cli(*service, options, memix::ViewTargetKind::FileTree);
    if (!expression)
      return std::unexpected(expression.error());
    definition = service->define_quest(*expression, metadata);
  } else {
    return memix::cli_detail::unexpected_error(
        "--kind must be selection or composition");
  }
  if (!definition) {
    return memix::cli_detail::unexpected_error(definition.error());
  }
  const memix::ViewResult<std::vector<memix::QuestInfo>> quests =
      service->list_quests();
  if (!quests) {
    return memix::cli_detail::unexpected_error(quests.error());
  }
  const auto info = std::ranges::find(*quests, definition->ir_hash,
                                      &memix::QuestInfo::ir_hash);
  if (info == quests->end()) {
    return memix::cli_detail::unexpected_error(
        "defined quest is missing from the quest listing");
  }
  if (options.define_json) {
    return print_json(QuestDefinitionJson{
        .ir_hash = info->ir_hash,
        .result_kind = std::string(memix::to_string(info->result_kind)),
        .intent = info->intent,
        .name = info->name,
        .created_at = info->created_at,
        .existed = definition->existed,
    });
  }
  std::println("quest: {}", info->ir_hash);
  std::println("intent: {}", info->intent);
  std::println("existed: {}", definition->existed ? "true" : "false");
  return {};
}

memix::cli_detail::VoidResult run_quests(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<std::vector<memix::QuestInfo>> quests =
      service->list_quests();
  if (!quests) {
    return memix::cli_detail::unexpected_error(quests.error());
  }
  return print_quests(*quests, options.quests_json);
}

memix::cli_detail::VoidResult run_stats(const ViewCliOptions &options) {
  if (options.quest_id.empty()) {
    return memix::cli_detail::unexpected_error("stats expects a quest id");
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::QuestStats> stats =
      service->quest_stats(options.quest_id);
  if (!stats) {
    return memix::cli_detail::unexpected_error(stats.error());
  }
  if (options.stats_json) {
    const std::vector<QuestProfileActivationsJson> activations_by_profile =
        stats->activations_by_profile
        | std::views::transform(
            [](const memix::QuestProfileActivations &activations) {
              return QuestProfileActivationsJson{
                  .profile = activations.profile,
                  .activation_count = activations.activation_count,
              };
            })
        | std::ranges::to<std::vector>();
    const std::vector<QuestEnvironmentReadsJson> reads_by_environment =
        stats->reads_by_environment
        | std::views::transform([](const memix::QuestEnvironmentReads &reads) {
            return QuestEnvironmentReadsJson{
                .environment_id = reads.environment_id,
                .read_count = reads.read_count,
            };
          })
        | std::ranges::to<std::vector>();
    std::vector<QuestDerivationReadsJson> derivation_reads;
    for (const auto &reads : stats->reads_by_derivation) {
      derivation_reads.push_back({
          .derivation_id = reads.derivation_id,
          .read_count = reads.read_count,
      });
    }
    return print_json(QuestStatsJson{
        .ir_hash = stats->ir_hash,
        .resolution_count = stats->resolution_count,
        .last_resolved_at = stats->last_resolved_at,
        .activation_count = stats->activation_count,
        .first_activated_at = stats->first_activated_at,
        .last_activated_at = stats->last_activated_at,
        .activations_by_profile = activations_by_profile,
        .read_count = stats->read_count,
        .first_read_at = stats->first_read_at,
        .last_read_at = stats->last_read_at,
        .cat_count = stats->cat_count,
        .tree_count = stats->tree_count,
        .reads_by_environment = reads_by_environment,
        .reads_by_derivation = std::move(derivation_reads),
        .ls_count = stats->ls_count,
    });
  }
  std::println("quest: {}", stats->ir_hash);
  std::println("resolutions: {}", stats->resolution_count);
  std::println("last_resolved: {}", stats->last_resolved_at);
  std::println("activations: {}", stats->activation_count);
  std::println("first_activated: {}", stats->first_activated_at);
  std::println("last_activated: {}", stats->last_activated_at);
  std::println("activations_by_profile:");
  for (const memix::QuestProfileActivations &activations :
       stats->activations_by_profile) {
    std::println("  {} {}", activations.profile, activations.activation_count);
  }
  std::println("reads: {}", stats->read_count);
  std::println("first_read: {}", stats->first_read_at);
  std::println("last_read: {}", stats->last_read_at);
  std::println("cat_reads: {}", stats->cat_count);
  std::println("tree_reads: {}", stats->tree_count);
  std::println("ls_reads: {}", stats->ls_count);
  std::println("reads_by_derivation:");
  for (const auto &reads : stats->reads_by_derivation) {
    std::println("  {} {}", memix::cli_detail::short_id(reads.derivation_id),
                 reads.read_count);
  }
  std::println("reads_by_environment:");
  for (const memix::QuestEnvironmentReads &reads :
       stats->reads_by_environment) {
    std::println("  {} {}", memix::cli_detail::short_id(reads.environment_id),
                 reads.read_count);
  }
  return {};
}

memix::cli_detail::VoidResult run_find(const ViewCliOptions &options) {
  if (options.find_terms.empty() && !options.covers.has_value()) {
    return memix::cli_detail::unexpected_error(
        "find requires at least one term or --covers");
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<std::vector<memix::QuestInfo>> quests =
      service->find_quests(options.find_terms, options.covers);
  if (!quests) {
    return memix::cli_detail::unexpected_error(quests.error());
  }
  return print_quests(*quests, options.find_json);
}

memix::cli_detail::VoidResult run_resolve(const ViewCliOptions &options) {
  if (options.quest_id.empty())
    return memix::cli_detail::unexpected_error("resolve expects a quest id");
  auto service = make_service(options);
  if (!service)
    return std::unexpected(service.error());
  auto resolution = service->resolve_quest(options.quest_id);
  if (!resolution)
    return memix::cli_detail::unexpected_error(resolution.error());
  return print_resolution(*resolution, options.resolve_json);
}

memix::cli_detail::VoidResult run_freshness(const ViewCliOptions &options) {
  if (options.quest_id.empty()) {
    return memix::cli_detail::unexpected_error("freshness expects a quest id");
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::QuestFreshness> freshness =
      service->quest_freshness(options.quest_id);
  if (!freshness) {
    return memix::cli_detail::unexpected_error(freshness.error());
  }
  if (options.freshness_json) {
    const std::vector<QuestFreshnessInputJson> inputs =
        freshness->inputs
        | std::views::transform([](const memix::QuestFreshnessInput &input) {
            return QuestFreshnessInputJson{
                .locator = input.locator,
                .recorded_generation = input.recorded_generation,
                .current_generation = input.current_generation,
                .pinned = input.pinned,
                .fresh = input.fresh,
            };
          })
        | std::ranges::to<std::vector>();
    return print_json(QuestFreshnessJson{
        .ir_hash = freshness->ir_hash,
        .result =
            {
                .kind = std::string(memix::to_string(freshness->result.kind)),
                .id = freshness->result.id,
            },
        .resolved_at = freshness->resolved_at,
        .inputs = inputs,
        .stale = freshness->stale,
    });
  }
  for (const memix::QuestFreshnessInput &input : freshness->inputs) {
    std::string_view state = "stale";
    if (input.pinned) {
      state = "pinned";
    } else if (input.fresh) {
      state = "fresh";
    }
    const std::string current =
        input.current_generation.has_value()
            ? memix::cli_detail::short_id(*input.current_generation)
            : "-";
    std::println("{}  {}  recorded={} current={}", state, input.locator,
                 memix::cli_detail::short_id(input.recorded_generation),
                 current);
  }
  std::println("stale: {}", freshness->stale ? "true" : "false");
  return {};
}

memix::cli_detail::VoidResult run_show(const ViewCliOptions &options) {
  auto reference = view_reference(options.show_reference);
  if (!reference)
    return std::unexpected(reference.error());
  auto service = make_service(options);
  if (!service)
    return std::unexpected(service.error());
  fs::path manifest;
  if (reference->kind == memix::ViewObjectKind::Derivation) {
    auto derivation = service->resolve_derivation(reference->id);
    if (!derivation)
      return memix::cli_detail::unexpected_error(derivation.error());
    manifest = derivation->manifest_path;
    if (!options.show_json) {
      std::println("derivation: {}\nquest: {}\nfiles: {}\nmanifest: {}",
                   derivation->id, derivation->ir_hash,
                   derivation->resolved_files.size(), manifest.string());
      return {};
    }
  } else {
    auto environment = service->resolve_environment(reference->id);
    if (!environment)
      return memix::cli_detail::unexpected_error(environment.error());
    manifest = environment->manifest_path;
    if (!options.show_json) {
      print_view_environment_summary(*environment);
      return {};
    }
  }
  auto contents = memix::cli_detail::read_file_contents(manifest);
  if (!contents)
    return std::unexpected(contents.error());
  std::println("{}", *contents);
  return {};
}

memix::cli_detail::Result<std::string>
resolve_cli_profile(const ViewCliOptions &options) {
  if (!options.profile_name.empty()) {
    const memix::ViewResult<std::string> valid =
        memix::validated_profile_name(options.profile_name);
    if (!valid) {
      return memix::cli_detail::unexpected_error(valid.error());
    }
    return *valid;
  }
  fs::path workspace = fs::path(options.into_path).parent_path();
  if (workspace.empty()) {
    workspace = ".";
  }
  const memix::FsResult<fs::path> canonical = fs::weakly_canonical(workspace);
  if (!canonical) {
    return memix::cli_detail::unexpected_error(canonical.error());
  }
  return memix::default_profile_name(*canonical);
}

memix::cli_detail::Result<memix::ActivationMode>
require_activation_mode(std::string_view mode_name) {
  const std::optional<memix::ActivationMode> mode =
      memix::parse_activation_mode(mode_name);
  if (!mode.has_value()) {
    return memix::cli_detail::unexpected_error(std::format(
        "unsupported activation mode `{}`; expected link|copy", mode_name));
  }
  return *mode;
}

void print_activation_summary(const memix::ViewActivation &activation) {
  std::println("profile: {}", activation.profile);
  std::println("generation: {}", activation.generation);
  std::println("environment: {}", activation.environment_id);
  std::println("workspace: {}", activation.workspace_path.string());
  std::println("switched: {}", activation.switched ? "true" : "false");
  std::println("materialized: {}", activation.materialized ? "true" : "false");
}

memix::cli_detail::VoidResult run_activate(const ViewCliOptions &options) {
  if (options.activate_environment_id.empty()) {
    return memix::cli_detail::unexpected_error(
        "activate expects an environment id");
  }
  const memix::cli_detail::Result<std::string> profile =
      resolve_cli_profile(options);
  if (!profile) {
    return std::unexpected(profile.error());
  }
  const memix::cli_detail::Result<memix::ActivationMode> mode =
      require_activation_mode(options.mode_name);
  if (!mode) {
    return std::unexpected(mode.error());
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  auto reference = view_reference(options.activate_environment_id);
  if (!reference)
    return std::unexpected(reference.error());
  if (reference->kind != memix::ViewObjectKind::Environment) {
    return memix::cli_detail::unexpected_error(
        "activate requires an Environment; compose the Derivation first");
  }
  const memix::ViewResult<memix::ViewActivation> activation =
      service->activate(reference->id, memix::ActivationOptions{
                                           .profile = *profile,
                                           .into = fs::path(options.into_path),
                                           .mode = *mode,
                                           .force = options.force,
                                       });
  if (!activation) {
    return memix::cli_detail::unexpected_error(activation.error());
  }
  print_activation_summary(*activation);
  return {};
}

memix::cli_detail::VoidResult run_generations(const ViewCliOptions &options) {
  const memix::cli_detail::Result<std::string> profile =
      resolve_cli_profile(options);
  if (!profile) {
    return std::unexpected(profile.error());
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> generations =
      service->generations(*profile);
  if (!generations) {
    return memix::cli_detail::unexpected_error(generations.error());
  }
  std::println("profile: {}", *profile);
  std::println("{:<12}{:<15}{:<6}{:<22}{:<8}", "GENERATION", "ENVIRONMENT",
               "MODE", "ACTIVATED_AT", "CURRENT");
  for (const memix::ViewGenerationInfo &generation : *generations) {
    std::println("{:<12}{:<15}{:<6}{:<22}{:<8}", generation.number,
                 memix::cli_detail::short_id(generation.environment_id),
                 memix::to_string(generation.mode), generation.activated_at,
                 generation.current ? "*" : "");
  }
  return {};
}

memix::cli_detail::VoidResult run_rollback(const ViewCliOptions &options) {
  const memix::cli_detail::Result<std::string> profile =
      resolve_cli_profile(options);
  if (!profile) {
    return std::unexpected(profile.error());
  }
  std::optional<std::uint64_t> target;
  if (options.rollback_to.has_value()) {
    const std::string_view value = *options.rollback_to;
    std::uint64_t number = 0;
    if (value.empty()) {
      return memix::cli_detail::unexpected_error(std::format(
          "rollback --to expects a generation number, got `{}`", value));
    }
    const char *const begin = value.data();
    // std::from_chars requires a raw pointer range into this contiguous view.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char *const end = begin + value.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, number);
    if (error != std::errc{} || parsed_end != end) {
      return memix::cli_detail::unexpected_error(std::format(
          "rollback --to expects a generation number, got `{}`", value));
    }
    target = number;
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ViewActivation> activation =
      service->rollback(*profile, target);
  if (!activation) {
    return memix::cli_detail::unexpected_error(activation.error());
  }
  print_activation_summary(*activation);
  return {};
}

memix::cli_detail::VoidResult run_deactivate(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ViewDeactivation> deactivation =
      service->deactivate(fs::path(options.into_path), options.wipe_profile);
  if (!deactivation) {
    return memix::cli_detail::unexpected_error(deactivation.error());
  }
  std::println("deactivated: {}", deactivation->workspace_path.string());
  std::println("profile: {}", deactivation->profile);
  std::println("profile_wiped: {}",
               deactivation->wiped_profile ? "true" : "false");
  return {};
}

memix::cli_detail::VoidResult run_status(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ViewActivationStatus> status =
      service->activation_status(fs::path(options.into_path));
  if (!status) {
    return memix::cli_detail::unexpected_error(status.error());
  }
  std::println("active: {}", status->active ? "true" : "false");
  if (!status->active) {
    return {};
  }
  std::println("mode: {}", memix::to_string(status->mode));
  std::println("profile: {}", status->profile);
  std::println("generation: {}", status->generation);
  std::println("environment: {}", status->environment_id);
  std::println("stale: {}", status->stale ? "true" : "false");
  return {};
}

void add_list_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--kind")
                 .argName("derivation|environment")
                 .handler([&options](std::string_view value) {
                   options.object_kind = value;
                 }));
  parser.add(Argum::Option("--json")
                 .help("Emit summaries for the selected object kind as JSON.")
                 .handler([&options] { options.list_json = true; }));
}

void add_tree_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(
      Argum::Positional("view-id")
          .help("derivation:ID or environment:ID; bare IDs name environments.")
          .occurs(Argum::neverOrOnce)
          .handler([&options](std::string_view value) {
            options.tree_reference = value;
          }));
  parser.add(Argum::Option("--path")
                 .argName("PATH")
                 .help("Limit output to one path or subtree.")
                 .handler([&options](std::string_view value) {
                   options.tree_path = fs::path(value);
                 }));
  parser.add(
      Argum::Option("--depth")
          .argName("INTEGER")
          .help("Maximum depth relative to --path (0 shows the root only).")
          .handler([&options](std::string_view value) -> Argum::Expected<void> {
            std::size_t depth = 0;
            const char *const begin = value.data();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const char *const end = begin + value.size();
            const auto [parsed_end, error] = std::from_chars(begin, end, depth);
            if (value.empty() || error != std::errc{} || parsed_end != end) {
              return memix::cli_detail::argum_validation_error(std::format(
                  "--depth expects a non-negative integer, got `{}`", value));
            }
            options.tree_depth = depth;
            return {};
          }));
  parser.add(Argum::Option("--json")
                 .help("Emit structured tree entries as JSON.")
                 .handler([&options] { options.tree_json = true; }));
}

void add_cat_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(
      Argum::Positional("view-id")
          .help("derivation:ID or environment:ID; bare IDs name environments.")
          .occurs(Argum::neverOrOnce)
          .handler([&options](std::string_view value) {
            options.cat_reference = value;
          }));
  parser.add(Argum::Positional("path")
                 .help("File path inside the selected view.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.cat_path = fs::path(value);
                 }));
}

void add_view_inputs(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--source")
                 .argName("TEXT")
                 .help("View locator, for example rules/repo/AGENTS.md or "
                       "chat/task/2026-05-20/@latest.")
                 .handler([&options](std::string_view value) {
                   options.sources.emplace_back(value);
                 }));
}

void add_mount_inputs(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(
      Argum::Option("--mount")
          .argName("DERIVATION[=MOUNT]")
          .help("Mount an immutable Derivation; repeat for multiple bindings.")
          .handler([&options](std::string_view value) {
            options.mounts.emplace_back(value);
          }));
}

void add_quest_metadata(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--intent")
                 .argName("TEXT")
                 .help("Semantic intent used to find this quest.")
                 .handler([&options](std::string_view value) {
                   options.intent = value;
                   options.intent_provided = true;
                 }));
  parser.add(Argum::Option("--name")
                 .argName("TEXT")
                 .help("Optional comment-style quest name.")
                 .handler([&options](std::string_view value) {
                   options.name = value;
                   options.name_provided = true;
                 }));
}

void add_select_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_view_inputs(parser, options);
  parser.add(Argum::Option("--target")
                 .argName("TEXT")
                 .help("Selection target. Only raw file_tree is supported.")
                 .handler([&options](std::string_view value) {
                   options.target_name = value;
                 }));
  add_quest_metadata(parser, options);
  parser.add(Argum::Option("--json")
                 .help("Emit a typed Derivation resolution as JSON.")
                 .handler([&options] { options.materialization_json = true; }));
}

void add_compose_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_mount_inputs(parser, options);
  add_quest_metadata(parser, options);
  parser.add(Argum::Option("--json").handler(
      [&options] { options.materialization_json = true; }));
}

void add_ls_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(
      Argum::Positional("view-id")
          .occurs(Argum::neverOrOnce)
          .help("derivation:ID or environment:ID; bare IDs name environments.")
          .handler([&options](std::string_view value) {
            options.tree_reference = value;
          }));
  parser.add(Argum::Positional("path")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.tree_path = fs::path(value);
                 }));
  parser.add(Argum::Option("--json").handler(
      [&options] { options.tree_json = true; }));
}

void add_define_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_view_inputs(parser, options);
  add_mount_inputs(parser, options);
  parser.add(Argum::Option("--kind")
                 .argName("selection|composition")
                 .handler([&options](std::string_view value) {
                   options.quest_kind = value;
                 }));
  add_quest_metadata(parser, options);
  parser.add(Argum::Option("--json")
                 .help("Emit the quest definition as JSON.")
                 .handler([&options] { options.define_json = true; }));
}

void add_quests_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--json")
                 .help("Emit quest summaries as JSON.")
                 .handler([&options] { options.quests_json = true; }));
}

void add_find_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("terms")
                 .help("Case-insensitive intent/name search terms.")
                 .occurs(Argum::zeroOrMoreTimes)
                 .handler([&options](std::string_view value) {
                   options.find_terms.emplace_back(value);
                 }));
  parser.add(Argum::Option("--covers")
                 .argName("PATH")
                 .help("Find quests structurally covering a logical path.")
                 .handler([&options](std::string_view value) {
                   options.covers = fs::path(value);
                 }));
  parser.add(Argum::Option("--json")
                 .help("Emit matching quest summaries as JSON.")
                 .handler([&options] { options.find_json = true; }));
}

void add_quest_id_command(Argum::Parser &parser, ViewCliOptions &options,
                          std::string_view help, bool &json) {
  parser.add(Argum::Positional("quest-id")
                 .help(help)
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.quest_id = value;
                 }));
  parser.add(Argum::Option("--json")
                 .help("Emit structured JSON output.")
                 .handler([&json] { json = true; }));
}

void add_resolve_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_quest_id_command(parser, options, "Quest id or unique prefix.",
                       options.resolve_json);
}

void add_freshness_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_quest_id_command(parser, options, "Quest id or unique prefix.",
                       options.freshness_json);
}

void add_stats_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_quest_id_command(parser, options, "Quest id or unique prefix.",
                       options.stats_json);
}

void add_show_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(
      Argum::Positional("view-id")
          .help("derivation:ID or environment:ID; bare IDs name environments.")
          .occurs(Argum::neverOrOnce)
          .handler([&options](std::string_view value) {
            options.show_reference = value;
          }));
  parser.add(Argum::Option("--json")
                 .help("Emit the selected object manifest as JSON.")
                 .handler([&options] { options.show_json = true; }));
}

void add_into_option(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--into")
                 .argName("PATH")
                 .help("Workspace path to attach the view to. Defaults to "
                       "./.memory.")
                 .handler([&options](std::string_view value) {
                   options.into_path = value;
                 }));
}

void add_profile_option(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--profile")
                 .argName("NAME")
                 .help("Profile name [a-z0-9][a-z0-9._-]*. Defaults to "
                       "ws-<hash> derived from the workspace directory, "
                       "which isolates parallel workspaces.")
                 .handler([&options](std::string_view value) {
                   options.profile_name = value;
                 }));
}

void add_activate_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("environment-id")
                 .help("View environment id or unique prefix.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.activate_environment_id = value;
                 }));
  add_into_option(parser, options);
  add_profile_option(parser, options);
  parser.add(Argum::Option("--mode")
                 .argName("MODE")
                 .help("Materialization mode: link (symlink into the store) "
                       "or copy (real files). Defaults to link.")
                 .handler([&options](std::string_view value) {
                   options.mode_name = value;
                 }));
  parser.add(Argum::Option("--force")
                 .help("Replace an existing workspace symlink.")
                 .handler([&options] { options.force = true; }));
}

void add_generations_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_into_option(parser, options);
  add_profile_option(parser, options);
}

void add_rollback_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_into_option(parser, options);
  add_profile_option(parser, options);
  parser.add(Argum::Option("--to")
                 .argName("N")
                 .help("Roll back to generation N. Defaults to the previous "
                       "generation.")
                 .handler([&options](std::string_view value) {
                   options.rollback_to = value;
                 }));
}

void add_deactivate_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_into_option(parser, options);
  parser.add(
      Argum::Option("--wipe-profile")
          .help("Also remove the profile and its generations from the "
                "store. Other workspaces sharing the profile will dangle.")
          .handler([&options] { options.wipe_profile = true; }));
}

void add_status_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_into_option(parser, options);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void add_commands(Argum::Parser &parser, ViewCliOptions &options) {
  parser.addSubCommand(
      Argum::Positional("subcommand")
          .occurs(Argum::neverOrOnce)
          .help("Subcommand: define, find, resolve, freshness, quests, stats, "
                "list, ls, tree, cat, select, compose, show, activate, "
                "generations, "
                "rollback, deactivate, or status.")
          .handler([&parser,
                    &options](std::string_view value) -> Argum::Expected<void> {
            if (value == "list") {
              options.command = ViewCommand::List;
              add_list_command(parser, options);
              return {};
            }
            if (value == "tree") {
              options.command = ViewCommand::Tree;
              add_tree_command(parser, options);
              return {};
            }
            if (value == "cat") {
              options.command = ViewCommand::Cat;
              add_cat_command(parser, options);
              return {};
            }
            if (value == "select") {
              options.command = ViewCommand::Select;
              add_select_command(parser, options);
              return {};
            }
            if (value == "compose") {
              options.command = ViewCommand::Compose;
              add_compose_command(parser, options);
              return {};
            }
            if (value == "ls") {
              options.command = ViewCommand::Ls;
              add_ls_command(parser, options);
              return {};
            }
            if (value == "define") {
              options.command = ViewCommand::Define;
              add_define_command(parser, options);
              return {};
            }
            if (value == "find") {
              options.command = ViewCommand::Find;
              add_find_command(parser, options);
              return {};
            }
            if (value == "resolve") {
              options.command = ViewCommand::Resolve;
              add_resolve_command(parser, options);
              return {};
            }
            if (value == "freshness") {
              options.command = ViewCommand::Freshness;
              add_freshness_command(parser, options);
              return {};
            }
            if (value == "quests") {
              options.command = ViewCommand::Quests;
              add_quests_command(parser, options);
              return {};
            }
            if (value == "stats") {
              options.command = ViewCommand::Stats;
              add_stats_command(parser, options);
              return {};
            }
            if (value == "show") {
              options.command = ViewCommand::Show;
              add_show_command(parser, options);
              return {};
            }
            if (value == "activate") {
              options.command = ViewCommand::Activate;
              add_activate_command(parser, options);
              return {};
            }
            if (value == "generations") {
              options.command = ViewCommand::Generations;
              add_generations_command(parser, options);
              return {};
            }
            if (value == "rollback") {
              options.command = ViewCommand::Rollback;
              add_rollback_command(parser, options);
              return {};
            }
            if (value == "deactivate") {
              options.command = ViewCommand::Deactivate;
              add_deactivate_command(parser, options);
              return {};
            }
            if (value == "status") {
              options.command = ViewCommand::Status;
              add_status_command(parser, options);
              return {};
            }
            return memix::cli_detail::argum_validation_error(std::format(
                "unsupported command `{}`; expected "
                "define|find|resolve|freshness|quests|stats|list|tree|cat|"
                "select|compose|ls|show|activate|generations|rollback|"
                "deactivate|status",
                value));
          }));
}

void print_root_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_commands(parser, options);
  std::print("{}", parser.formatHelp(program_name));
}

void print_list_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_list_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} list", program_name)));
}

void print_tree_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_tree_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} tree", program_name)));
}

void print_cat_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_cat_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} cat", program_name)));
}

void print_materialization_help(std::string_view program_name,
                                bool compose = false) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  if (compose)
    add_compose_command(parser, options);
  else
    add_select_command(parser, options);
  std::print("{}", parser.formatHelp(std::format(
                       "{} {}", program_name, compose ? "compose" : "select")));
}

void print_ls_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_ls_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} ls", program_name)));
}

void print_define_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_define_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} define", program_name)));
}

void print_find_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_find_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} find", program_name)));
}

void print_resolve_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_resolve_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} resolve", program_name)));
}

void print_freshness_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_freshness_command(parser, options);
  std::print("{}",
             parser.formatHelp(std::format("{} freshness", program_name)));
}

void print_quests_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_quests_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} quests", program_name)));
}

void print_stats_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_stats_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} stats", program_name)));
}

void print_show_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_show_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} show", program_name)));
}

void print_activate_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_activate_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} activate", program_name)));
}

void print_generations_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_generations_command(parser, options);
  std::print("{}",
             parser.formatHelp(std::format("{} generations", program_name)));
}

void print_rollback_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_rollback_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} rollback", program_name)));
}

void print_deactivate_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_deactivate_command(parser, options);
  std::print("{}",
             parser.formatHelp(std::format("{} deactivate", program_name)));
}

void print_status_help(std::string_view program_name) {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_status_command(parser, options);
  std::print("{}", parser.formatHelp(std::format("{} status", program_name)));
}

void print_selected_help(ViewCommand command, std::string_view program_name) {
  switch (command) {
  case ViewCommand::List:
    print_list_help(program_name);
    return;
  case ViewCommand::Tree:
    print_tree_help(program_name);
    return;
  case ViewCommand::Cat:
    print_cat_help(program_name);
    return;
  case ViewCommand::Select:
    print_materialization_help(program_name);
    return;
  case ViewCommand::Compose:
    print_materialization_help(program_name, true);
    return;
  case ViewCommand::Ls:
    print_ls_help(program_name);
    return;
  case ViewCommand::Define:
    print_define_help(program_name);
    return;
  case ViewCommand::Find:
    print_find_help(program_name);
    return;
  case ViewCommand::Resolve:
    print_resolve_help(program_name);
    return;
  case ViewCommand::Freshness:
    print_freshness_help(program_name);
    return;
  case ViewCommand::Quests:
    print_quests_help(program_name);
    return;
  case ViewCommand::Stats:
    print_stats_help(program_name);
    return;
  case ViewCommand::Show:
    print_show_help(program_name);
    return;
  case ViewCommand::Activate:
    print_activate_help(program_name);
    return;
  case ViewCommand::Generations:
    print_generations_help(program_name);
    return;
  case ViewCommand::Rollback:
    print_rollback_help(program_name);
    return;
  case ViewCommand::Deactivate:
    print_deactivate_help(program_name);
    return;
  case ViewCommand::Status:
    print_status_help(program_name);
    return;
  case ViewCommand::None:
    print_root_help(program_name);
    return;
  }
}

memix::cli_detail::VoidResult dispatch(const ViewCliOptions &options) {
  switch (options.command) {
  case ViewCommand::List:
    return run_list(options);
  case ViewCommand::Tree:
    return run_tree(options);
  case ViewCommand::Cat:
    return run_cat(options);
  case ViewCommand::Select:
  case ViewCommand::Compose:
    return run_materialization(options);
  case ViewCommand::Ls:
    return run_tree(options);
  case ViewCommand::Define:
    return run_define(options);
  case ViewCommand::Find:
    return run_find(options);
  case ViewCommand::Resolve:
    return run_resolve(options);
  case ViewCommand::Freshness:
    return run_freshness(options);
  case ViewCommand::Quests:
    return run_quests(options);
  case ViewCommand::Stats:
    return run_stats(options);
  case ViewCommand::Show:
    return run_show(options);
  case ViewCommand::Activate:
    return run_activate(options);
  case ViewCommand::Generations:
    return run_generations(options);
  case ViewCommand::Rollback:
    return run_rollback(options);
  case ViewCommand::Deactivate:
    return run_deactivate(options);
  case ViewCommand::Status:
    return run_status(options);
  case ViewCommand::None:
    return memix::cli_detail::unexpected_error("a subcommand is required");
  }
  return memix::cli_detail::unexpected_error("unknown command");
}

int run_cli(const std::vector<std::string> &arguments,
            std::string_view program_name,
            const std::shared_ptr<spdlog::logger> &logger) {
  Argum::Parser parser;
  ViewCliOptions options;

  memix::cli_detail::add_common_options(parser, options.common);
  add_commands(parser, options);

  const memix::cli_detail::VoidResult parsed =
      memix::cli_detail::parse_arguments(parser, arguments);
  if (!parsed) {
    logger->error("{}", parsed.error().message);
    return 1;
  }
  if (options.common.version_requested) {
    memix::cli_detail::print_version();
    return 0;
  }
  if (options.common.help_requested) {
    print_selected_help(options.command, program_name);
    return 0;
  }

  const memix::cli_detail::VoidResult result = dispatch(options);
  if (!result) {
    logger->error("{}", result.error().message);
    return 1;
  }
  return 0;
}

} // namespace

int memix::cli::run_view(const std::vector<std::string> &arguments) {
  constexpr std::string_view program_name = "memix view";
  const std::shared_ptr<spdlog::logger> logger =
      memix::cli_detail::make_logger(program_name);
  return run_cli(arguments, program_name, logger);
}
