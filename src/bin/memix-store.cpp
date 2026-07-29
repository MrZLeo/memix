#include "cli_common.hpp"
#include "detail/error.hpp"
#include "detail/json.hpp"
#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"

#include <array>
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

constexpr int kTypeColumnWidth = 11;
constexpr int kIdColumnWidth = 14;
constexpr int kSizeColumnWidth = 10;
constexpr int kTimestampColumnWidth = 21;
constexpr int kContextPathColumnWidth = 24;
constexpr std::uintmax_t kSizeUnitBaseBytes = 1024;
constexpr double kSizeUnitBase = 1024.0;
constexpr std::size_t kJsonIndentSpaces = 2;

enum class StoreCommand : std::uint8_t {
  None,
  List,
  Ingest,
  Cat,
  Tree,
  Expose,
  Rules,
  RulesCat,
  RulesTree,
};

struct StoreCliOptions {
  memix::cli_detail::CommonOptions common;
  StoreCommand command = StoreCommand::None;
  std::vector<fs::path> ingest_paths;
  std::string ingest_type_name;
  fs::path ingest_context_dest;
  std::string ingest_message;
  std::string list_type_name;
  bool list_json = false;
  std::string cat_type_name;
  fs::path cat_path;
  std::string cat_generation;
  std::string tree_type_name;
  std::string tree_generation;
  fs::path tree_path;
  std::optional<std::size_t> tree_depth;
  bool tree_all = false;
  bool tree_json = false;
  bool expose_json = false;
};

std::string supported_types_message() {
  return memix::memory_object_types()
       | std::views::transform([](memix::MemoryObjectType type) {
           return memix::to_string(type);
         })
       | std::views::join_with('|')
       | std::ranges::to<std::string>();
}

std::unexpected<memix::StoreError>
store_error(memix::detail::CommonError error) {
  return std::unexpected(
      memix::detail::translate_error<memix::StoreError>(std::move(error)));
}

memix::cli_detail::Result<memix::MemoryObjectType>
require_type(std::string_view value) {
  const auto type = memix::parse_memory_object_type(value);
  if (!type.has_value()) {
    return memix::cli_detail::unexpected_error(
        std::format("unsupported object type `{}`; expected {}", value,
                    supported_types_message()));
  }
  return *type;
}

std::string format_size(std::uintmax_t size_bytes) {
  constexpr std::array<std::string_view, 7> units = {
      "B", "KB", "MB", "GB", "TB", "PB", "EB",
  };

  if (size_bytes < kSizeUnitBaseBytes) {
    return std::format("{} B", size_bytes);
  }

  auto size = static_cast<double>(size_bytes);
  std::size_t unit_index = 0;
  while (size >= kSizeUnitBase && unit_index + 1 < units.size()) {
    size /= kSizeUnitBase;
    ++unit_index;
  }

  std::string formatted = std::format("{:.1f}", size);
  if (formatted.size() >= 2 && formatted.substr(formatted.size() - 2) == ".0") {
    formatted.resize(formatted.size() - 2);
  }
  return std::format("{} {}", formatted, units.at(unit_index));
}

void print_table(const std::vector<memix::StoredObject> &objects) {
  std::println("{:<{}}{:<{}}{:>{}}  {:<{}}{}", "TYPE", kTypeColumnWidth, "ID",
               kIdColumnWidth, "SIZE", kSizeColumnWidth, "CREATED_AT",
               kTimestampColumnWidth, "FILENAME");
  for (const memix::StoredObject &object : objects) {
    const std::string size = format_size(object.size_bytes);
    std::println("{:<{}}{:<{}}{:>{}}  {:<{}}{}", memix::to_string(object.type),
                 kTypeColumnWidth, memix::cli_detail::short_id(object.id),
                 kIdColumnWidth, size, kSizeColumnWidth, object.created_at,
                 kTimestampColumnWidth, object.filename);
  }
}

void print_context_tree_table(const std::vector<memix::StoreFile> &files) {
  memix::cli_detail::print_path_id_table(
      files,
      memix::cli_detail::PathIdTableLayout{
          .minimum_path_width = kContextPathColumnWidth,
          .id_column_width = kIdColumnWidth,
      },
      [](const memix::StoreFile &file) {
        return file.context_path.generic_string();
      },
      [](const memix::StoreFile &file) -> std::string_view {
        return file.blob.id;
      });
}

struct StoreCatalogEntryJson {
  std::string type;
  std::string path;
  std::string kind;
  std::size_t file_count = 0;
  std::uintmax_t size_bytes = 0;
  std::string latest_created_at;
  std::string blob;
};

struct ExposeTypeReportJson {
  std::string type;
  std::size_t files = 0;
};

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const StoreCatalogEntryJson &entry) {
  builder.start_object();
  builder.template append_key_value<"type">(entry.type);
  builder.append_comma();
  builder.template append_key_value<"path">(entry.path);
  builder.append_comma();
  builder.template append_key_value<"kind">(entry.kind);
  builder.append_comma();
  builder.template append_key_value<"file_count">(entry.file_count);
  builder.append_comma();
  builder.template append_key_value<"size_bytes">(entry.size_bytes);
  builder.append_comma();
  builder.template append_key_value<"latest_created_at">(
      entry.latest_created_at);
  builder.append_comma();
  builder.template append_key_value<"blob">(entry.blob);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ExposeTypeReportJson &report) {
  builder.start_object();
  builder.template append_key_value<"type">(report.type);
  builder.append_comma();
  builder.template append_key_value<"files">(report.files);
  builder.end_object();
}

std::vector<StoreCatalogEntryJson>
catalog_json_entries(const std::vector<memix::StoreCatalogEntry> &entries) {
  return entries
       | std::views::transform([](const memix::StoreCatalogEntry &entry) {
           return StoreCatalogEntryJson{
               .type = std::string(memix::to_string(entry.type)),
               .path = entry.context_path.generic_string(),
               .kind = entry.directory ? "directory" : "file",
               .file_count = entry.file_count,
               .size_bytes = entry.total_size_bytes,
               .latest_created_at = entry.latest_created_at,
               .blob = entry.blob_id,
           };
         })
       | std::ranges::to<std::vector>();
}

void print_catalog_table(const std::vector<memix::StoreCatalogEntry> &entries) {
  std::println("{:<{}}{:<11}{:>7}{:>{}}  {}", "TYPE", kTypeColumnWidth, "KIND",
               "FILES", "SIZE", kSizeColumnWidth, "PATH");
  for (const memix::StoreCatalogEntry &entry : entries) {
    std::println(
        "{:<{}}{:<11}{:>7}{:>{}}  {}", memix::to_string(entry.type),
        kTypeColumnWidth, entry.directory ? "directory" : "file",
        entry.file_count, format_size(entry.total_size_bytes), kSizeColumnWidth,
        entry.context_path.empty() ? "." : entry.context_path.generic_string());
  }
}

std::expected<std::string, memix::StoreError>
metadata_json_array(const std::vector<memix::StoredObject> &objects) {
  std::string output = "[";
  bool needs_comma = false;
  for (const memix::StoredObject &object : objects) {
    if (needs_comma) {
      output += ",";
    }
    const std::expected<std::string, memix::StoreError> metadata =
        memix::metadata_json(object);
    if (!metadata) {
      return std::unexpected(metadata.error());
    }
    output += *metadata;
    needs_comma = true;
  }
  output += "]";
  return output;
}

std::expected<std::string, memix::StoreError>
pretty_json(std::string_view json) {
  auto document = memix::detail::parse_json(json);
  if (!document) {
    return store_error(memix::detail::with_error_context(
        std::move(document.error()), "generated JSON"));
  }

  simdjson::fractured_json_options options;
  options.indent_spaces = kJsonIndentSpaces;
  options.max_inline_length = 0;
  options.max_inline_complexity = 0;
  options.max_compact_array_complexity = 0;
  options.enable_table_format = false;
  options.enable_compact_multiline = false;
  return simdjson::fractured_json(document->root(), options);
}

memix::cli_detail::VoidResult
print_json_array(const std::vector<memix::StoredObject> &objects) {
  const std::expected<std::string, memix::StoreError> json =
      metadata_json_array(objects);
  if (!json) {
    return memix::cli_detail::unexpected_error(json.error());
  }
  const std::expected<std::string, memix::StoreError> formatted_json =
      pretty_json(*json);
  if (!formatted_json) {
    return memix::cli_detail::unexpected_error(formatted_json.error());
  }
  std::println("{}", *formatted_json);
  return {};
}

memix::cli_detail::VoidResult
print_catalog_json(const std::vector<memix::StoreCatalogEntry> &entries) {
  simdjson::builder::string_builder builder;
  builder.append(catalog_json_entries(entries));
  const memix::detail::CommonResult<std::string> json =
      memix::detail::json_from_builder(builder);
  if (!json) {
    return memix::cli_detail::unexpected_error(json.error().message);
  }
  const std::expected<std::string, memix::StoreError> formatted =
      pretty_json(*json);
  if (!formatted) {
    return memix::cli_detail::unexpected_error(formatted.error());
  }
  std::println("{}", *formatted);
  return {};
}

memix::cli_detail::VoidResult
print_expose_json(const memix::ExposeReport &report) {
  const std::vector<ExposeTypeReportJson> values =
      report.files_by_type
      | std::views::transform([](const memix::ExposeTypeReport &type_report) {
          return ExposeTypeReportJson{
              .type = std::string(memix::to_string(type_report.type)),
              .files = type_report.file_count,
          };
        })
      | std::ranges::to<std::vector>();
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

memix::cli_detail::Result<memix::Store>
make_store(const StoreCliOptions &options) {
  const memix::cli_detail::Result<fs::path> root =
      memix::cli_detail::resolve_store_root(options.common.store_root);
  if (!root) {
    return std::unexpected(root.error());
  }
  return memix::Store(*root);
}

memix::cli_detail::Result<std::optional<memix::MemoryObjectType>>
optional_type(std::string_view type_name) {
  if (type_name.empty()) {
    return std::optional<memix::MemoryObjectType>{};
  }
  const memix::cli_detail::Result<memix::MemoryObjectType> type =
      require_type(type_name);
  if (!type) {
    return std::unexpected(type.error());
  }
  return *type;
}

std::optional<std::string_view>
optional_generation(const std::string &generation) {
  if (generation.empty()) {
    return std::nullopt;
  }
  return generation;
}

memix::cli_detail::VoidResult run_list(const StoreCliOptions &options) {
  const memix::cli_detail::Result<memix::Store> store = make_store(options);
  if (!store) {
    return std::unexpected(store.error());
  }
  const memix::cli_detail::Result<std::optional<memix::MemoryObjectType>> type =
      optional_type(options.list_type_name);
  if (!type) {
    return std::unexpected(type.error());
  }
  const std::expected<std::vector<memix::StoredObject>, memix::StoreError>
      objects = store->list(*type);
  if (!objects) {
    return memix::cli_detail::unexpected_error(objects.error());
  }
  if (options.list_json) {
    return print_json_array(*objects);
  }
  print_table(*objects);
  return {};
}

struct CommandTypeRequest {
  std::string_view command_name;
  std::string_view type_name;
};

memix::cli_detail::Result<memix::MemoryObjectType>
require_command_type(CommandTypeRequest request) {
  if (request.type_name.empty()) {
    return memix::cli_detail::unexpected_error(std::format(
        "{} requires --type with an object type", request.command_name));
  }
  return require_type(request.type_name);
}

memix::cli_detail::VoidResult
run_ingest(const StoreCliOptions &options,
           const std::shared_ptr<spdlog::logger> &logger) {
  if (options.ingest_type_name.empty()) {
    return memix::cli_detail::unexpected_error(
        "ingest requires --type with an object type");
  }
  if (options.ingest_paths.empty()) {
    return memix::cli_detail::unexpected_error(
        "ingest expects one or more source paths");
  }

  const memix::cli_detail::Result<memix::MemoryObjectType> type =
      require_type(options.ingest_type_name);
  if (!type) {
    return std::unexpected(type.error());
  }
  const memix::cli_detail::Result<memix::Store> store = make_store(options);
  if (!store) {
    return std::unexpected(store.error());
  }

  if (!options.ingest_context_dest.empty()) {
    if (options.ingest_paths.size() != 1) {
      return memix::cli_detail::unexpected_error(
          "ingest with --dest expects exactly one source path");
    }

    const std::expected<memix::FileIngestResult, memix::StoreError> result =
        store->ingest_file({
            .type = *type,
            .source_path = options.ingest_paths.front(),
            .context_path = options.ingest_context_dest,
            .message = options.ingest_message,
        });
    if (!result) {
      return memix::cli_detail::unexpected_error(result.error());
    }
    logger->info("{} type={} id={} filename={} path={} generation={}",
                 result->blob.inserted ? "ingested" : "exists",
                 memix::to_string(result->blob.object.type),
                 result->blob.object.id, result->blob.object.filename,
                 result->context_path.generic_string(), result->generation.id);
    if (result->projection_warning.has_value()) {
      logger->warn("{}", *result->projection_warning);
    }
    return {};
  }

  if (!options.ingest_message.empty()) {
    return memix::cli_detail::unexpected_error(
        "--message is only valid when ingest uses --dest");
  }

  for (const auto &path : options.ingest_paths) {
    const std::expected<memix::IngestResult, memix::StoreError> result =
        store->ingest(*type, path);
    if (!result) {
      return memix::cli_detail::unexpected_error(result.error());
    }
    logger->info("{} type={} id={} filename={}",
                 result->inserted ? "ingested" : "exists",
                 memix::to_string(result->object.type), result->object.id,
                 result->object.filename);
  }
  return {};
}

memix::cli_detail::VoidResult run_expose(const StoreCliOptions &options) {
  const memix::cli_detail::Result<memix::Store> store = make_store(options);
  if (!store) {
    return std::unexpected(store.error());
  }
  const memix::StoreResult<memix::ExposeReport> report = store->expose();
  if (!report) {
    return memix::cli_detail::unexpected_error(report.error());
  }
  if (options.expose_json) {
    return print_expose_json(*report);
  }
  for (const memix::ExposeTypeReport &type_report : report->files_by_type) {
    std::println("{}: {} file{}", memix::to_string(type_report.type),
                 type_report.file_count,
                 type_report.file_count == 1 ? "" : "s");
  }
  return {};
}

memix::cli_detail::VoidResult run_cat(const StoreCliOptions &options) {
  const memix::cli_detail::Result<memix::MemoryObjectType> type =
      require_command_type(CommandTypeRequest{
          .command_name = "cat",
          .type_name = options.cat_type_name,
      });
  if (!type) {
    return std::unexpected(type.error());
  }
  if (options.cat_path.empty()) {
    return memix::cli_detail::unexpected_error("cat expects a logical path");
  }
  const memix::cli_detail::Result<memix::Store> store = make_store(options);
  if (!store) {
    return std::unexpected(store.error());
  }
  const std::expected<memix::StoredObject, memix::StoreError> object =
      store->read_file(*type, options.cat_path,
                       optional_generation(options.cat_generation));
  if (!object) {
    return memix::cli_detail::unexpected_error(object.error());
  }
  const memix::cli_detail::Result<std::string> contents =
      memix::cli_detail::read_file_contents(object->object_dir
                                            / object->content_path);
  if (!contents) {
    return std::unexpected(contents.error());
  }
  std::print("{}", *contents);
  return {};
}

memix::cli_detail::VoidResult run_tree(const StoreCliOptions &options) {
  if (options.tree_all && !options.tree_type_name.empty()) {
    return memix::cli_detail::unexpected_error(
        "tree accepts either --all or --type, not both");
  }
  if (!options.tree_all && options.tree_type_name.empty()) {
    return memix::cli_detail::unexpected_error(
        "tree requires --type with an object type, or --all");
  }
  const memix::cli_detail::Result<std::optional<memix::MemoryObjectType>> type =
      optional_type(options.tree_type_name);
  if (!type) {
    return std::unexpected(type.error());
  }
  const memix::cli_detail::Result<memix::Store> store = make_store(options);
  if (!store) {
    return std::unexpected(store.error());
  }
  const bool legacy_output = !options.tree_all
                          && options.tree_path.empty()
                          && !options.tree_depth.has_value()
                          && !options.tree_json;
  if (legacy_output) {
    if (!type->has_value()) {
      return memix::cli_detail::unexpected_error(
          "tree requires --type with an object type");
    }
    const std::expected<std::vector<memix::StoreFile>, memix::StoreError>
        files = store->list_files(type->value(),
                                  optional_generation(options.tree_generation));
    if (!files) {
      return memix::cli_detail::unexpected_error(files.error());
    }
    print_context_tree_table(*files);
    return {};
  }

  const std::expected<std::vector<memix::StoreCatalogEntry>, memix::StoreError>
      entries = store->catalog(memix::StoreCatalogOptions{
          .type = *type,
          .path = options.tree_path,
          .depth = options.tree_depth,
          .generation = options.tree_generation.empty()
                          ? std::nullopt
                          : std::optional<std::string>(options.tree_generation),
      });
  if (!entries) {
    return memix::cli_detail::unexpected_error(entries.error());
  }
  if (options.tree_json) {
    return print_catalog_json(*entries);
  }
  print_catalog_table(*entries);
  return {};
}

void add_list_command(Argum::Parser &parser, StoreCliOptions &options) {
  parser.add(Argum::Option("--type")
                 .argName("TEXT")
                 .help(std::format("Filter by object type: {}",
                                   supported_types_message()))
                 .handler([&options](std::string_view value) {
                   options.list_type_name = value;
                 }));
  parser.add(Argum::Option("--json")
                 .help("Emit metadata as a JSON array instead of a table.")
                 .handler([&options]() { options.list_json = true; }));
}

void add_ingest_command(Argum::Parser &parser, StoreCliOptions &options) {
  parser.add(
      Argum::Option("--type")
          .argName("TEXT")
          .help(std::format("Object type: {}", supported_types_message()))
          .handler([&options](std::string_view value) {
            options.ingest_type_name = value;
          }));
  parser.add(Argum::Positional("path")
                 .help("Source path(s) to ingest into the store.")
                 .occurs(Argum::zeroOrMoreTimes)
                 .handler([&options](std::string_view value) {
                   options.ingest_paths.emplace_back(value);
                 }));
  parser.add(
      Argum::Option("--dest")
          .argName("TEXT")
          .help("Logical destination path inside the type's canonical context "
                "tree.")
          .handler([&options](std::string_view value) {
            options.ingest_context_dest = fs::path(value);
          }));
  parser.add(Argum::Option("--message")
                 .argName("TEXT")
                 .help("Context generation message. Only valid with --dest.")
                 .handler([&options](std::string_view value) {
                   options.ingest_message = value;
                 }));
}

void add_type_option(Argum::Parser &parser, std::string &type_name) {
  parser.add(
      Argum::Option("--type")
          .argName("TEXT")
          .help(std::format("Object type: {}", supported_types_message()))
          .handler(
              [&type_name](std::string_view value) { type_name = value; }));
}

void add_generation_option(Argum::Parser &parser, std::string &generation,
                           std::string_view help) {
  parser.add(Argum::Option("--generation")
                 .argName("TEXT")
                 .help(help)
                 .handler([&generation](std::string_view value) {
                   generation = value;
                 }));
}

void add_cat_command(Argum::Parser &parser, StoreCliOptions &options) {
  add_type_option(parser, options.cat_type_name);
  parser.add(Argum::Positional("path")
                 .help("Logical path inside the selected type's context tree.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.cat_path = fs::path(value);
                 }));
  add_generation_option(parser, options.cat_generation,
                        "Generation id. Defaults to the selected type's "
                        "latest ref.");
}

void add_tree_command(Argum::Parser &parser, StoreCliOptions &options) {
  add_type_option(parser, options.tree_type_name);
  parser.add(Argum::Option("--all")
                 .help("Explore every available object type.")
                 .handler([&options]() { options.tree_all = true; }));
  parser.add(Argum::Option("--path")
                 .argName("TEXT")
                 .help("Limit output to one logical path or subtree.")
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
                 .help("Emit structured catalog entries as JSON.")
                 .handler([&options]() { options.tree_json = true; }));
  add_generation_option(parser, options.tree_generation,
                        "Generation id. Defaults to the selected type's "
                        "latest ref.");
}

void add_expose_command(Argum::Parser &parser, StoreCliOptions &options) {
  parser.add(Argum::Option("--json")
                 .help("Emit per-type projection file counts as JSON.")
                 .handler([&options]() { options.expose_json = true; }));
}

void add_rules_cat_command(Argum::Parser &parser, StoreCliOptions &options) {
  options.cat_type_name =
      std::string(memix::to_string(memix::MemoryObjectType::Rules));
  parser.add(Argum::Positional("rule-path")
                 .help("Logical path inside the rules tree.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.cat_path = fs::path(value);
                 }));
  add_generation_option(parser, options.cat_generation,
                        "Generation id. Defaults to rules/latest.");
}

void add_rules_tree_command(Argum::Parser &parser, StoreCliOptions &options) {
  options.tree_type_name =
      std::string(memix::to_string(memix::MemoryObjectType::Rules));
  add_generation_option(parser, options.tree_generation,
                        "Generation id. Defaults to rules/latest.");
}

void add_rules_commands(Argum::Parser &parser, StoreCliOptions &options) {
  options.command = StoreCommand::Rules;
  parser.addSubCommand(
      Argum::Positional("rules-command")
          .occurs(Argum::neverOrOnce)
          .help("Rules command: cat or tree.")
          .handler([&parser,
                    &options](std::string_view value) -> Argum::Expected<void> {
            if (value == "cat") {
              options.command = StoreCommand::RulesCat;
              add_rules_cat_command(parser, options);
              return {};
            }
            if (value == "tree") {
              options.command = StoreCommand::RulesTree;
              add_rules_tree_command(parser, options);
              return {};
            }
            return memix::cli_detail::argum_validation_error(std::format(
                "unsupported rules command `{}`; expected cat|tree", value));
          }));
}

void add_commands(Argum::Parser &parser, StoreCliOptions &options) {
  parser.addSubCommand(
      Argum::Positional("subcommand")
          .occurs(Argum::neverOrOnce)
          .help("Subcommand: list, ingest, cat, tree, expose, or rules.")
          .handler([&parser,
                    &options](std::string_view value) -> Argum::Expected<void> {
            if (value == "list") {
              options.command = StoreCommand::List;
              add_list_command(parser, options);
              return {};
            }
            if (value == "ingest") {
              options.command = StoreCommand::Ingest;
              add_ingest_command(parser, options);
              return {};
            }
            if (value == "cat") {
              options.command = StoreCommand::Cat;
              add_cat_command(parser, options);
              return {};
            }
            if (value == "tree") {
              options.command = StoreCommand::Tree;
              add_tree_command(parser, options);
              return {};
            }
            if (value == "expose") {
              options.command = StoreCommand::Expose;
              add_expose_command(parser, options);
              return {};
            }
            if (value == "rules") {
              add_rules_commands(parser, options);
              return {};
            }
            return memix::cli_detail::argum_validation_error(
                std::format("unsupported command `{}`; expected "
                            "list|ingest|cat|tree|expose|rules",
                            value));
          }));
}

void print_root_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_commands(parser, options);
  std::print("{}", parser.formatHelp("memix-store"));
}

void print_list_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_list_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store list"));
}

void print_ingest_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_ingest_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store ingest"));
}

void print_cat_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_cat_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store cat"));
}

void print_tree_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_tree_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store tree"));
}

void print_expose_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_expose_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store expose"));
}

void print_rules_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_rules_commands(parser, options);
  std::print("{}", parser.formatHelp("memix-store rules"));
}

void print_rules_cat_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_rules_cat_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store rules cat"));
}

void print_rules_tree_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_rules_tree_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store rules tree"));
}

void print_selected_help(StoreCommand command) {
  switch (command) {
  case StoreCommand::List:
    print_list_help();
    return;
  case StoreCommand::Ingest:
    print_ingest_help();
    return;
  case StoreCommand::Cat:
    print_cat_help();
    return;
  case StoreCommand::Tree:
    print_tree_help();
    return;
  case StoreCommand::Expose:
    print_expose_help();
    return;
  case StoreCommand::Rules:
    print_rules_help();
    return;
  case StoreCommand::RulesCat:
    print_rules_cat_help();
    return;
  case StoreCommand::RulesTree:
    print_rules_tree_help();
    return;
  case StoreCommand::None:
    print_root_help();
    return;
  }
}

memix::cli_detail::VoidResult
dispatch(const StoreCliOptions &options,
         const std::shared_ptr<spdlog::logger> &logger) {
  switch (options.command) {
  case StoreCommand::List:
    return run_list(options);
  case StoreCommand::Ingest:
    return run_ingest(options, logger);
  case StoreCommand::Cat:
  case StoreCommand::RulesCat:
    return run_cat(options);
  case StoreCommand::Tree:
  case StoreCommand::RulesTree:
    return run_tree(options);
  case StoreCommand::Expose:
    return run_expose(options);
  case StoreCommand::Rules:
    return memix::cli_detail::unexpected_error(
        "rules requires a subcommand: cat|tree");
  case StoreCommand::None:
    return memix::cli_detail::unexpected_error("a subcommand is required");
  }
  return memix::cli_detail::unexpected_error("unknown command");
}

int
run_cli(int argc, char **argv, const std::shared_ptr<spdlog::logger> &logger) {
  Argum::Parser parser;
  StoreCliOptions options;

  memix::cli_detail::add_common_options(parser, options.common);
  add_commands(parser, options);

  const memix::cli_detail::VoidResult parsed =
      memix::cli_detail::parse_arguments(parser, argc, argv);
  if (!parsed) {
    logger->error("{}", parsed.error().message);
    return 1;
  }
  if (options.common.version_requested) {
    memix::cli_detail::print_version();
    return 0;
  }
  if (options.common.help_requested) {
    print_selected_help(options.command);
    return 0;
  }

  const memix::cli_detail::VoidResult result = dispatch(options, logger);
  if (!result) {
    logger->error("{}", result.error().message);
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::shared_ptr<spdlog::logger> logger =
      memix::cli_detail::make_logger("memix-store");
  return run_cli(argc, argv, logger);
}
