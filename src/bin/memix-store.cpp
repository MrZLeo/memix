#include "memix/store.hpp"

#include "memix/error.hpp"
#include "memix/fs.hpp"

#include "cli_common.hpp"

#include <array>
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
  Agents,
  AgentsCat,
  AgentsTree,
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
};

std::string supported_types_message() {
  return memix::memory_object_types()
       | std::views::transform([](memix::MemoryObjectType type) {
           return memix::to_string(type);
         })
       | std::views::join_with('|')
       | std::ranges::to<std::string>();
}

std::unexpected<memix::StoreError> store_error(std::string message) {
  memix::StoreError error;
  error.message = std::move(message);
  return std::unexpected(std::move(error));
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
  simdjson::dom::parser parser;
  simdjson::dom::element document;
  const simdjson::padded_string padded_json(json);
  const simdjson::error_code error = parser.parse(padded_json).get(document);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("failed to parse generated JSON: {}",
                                   simdjson::error_message(error)));
  }

  simdjson::fractured_json_options options;
  options.indent_spaces = kJsonIndentSpaces;
  options.max_inline_length = 0;
  options.max_inline_complexity = 0;
  options.max_compact_array_complexity = 0;
  options.enable_table_format = false;
  options.enable_compact_multiline = false;
  return simdjson::fractured_json(document, options);
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
  const memix::cli_detail::Result<memix::MemoryObjectType> type =
      require_command_type(CommandTypeRequest{
          .command_name = "tree",
          .type_name = options.tree_type_name,
      });
  if (!type) {
    return std::unexpected(type.error());
  }
  const memix::cli_detail::Result<memix::Store> store = make_store(options);
  if (!store) {
    return std::unexpected(store.error());
  }
  const std::expected<std::vector<memix::StoreFile>, memix::StoreError> files =
      store->list_files(*type, optional_generation(options.tree_generation));
  if (!files) {
    return memix::cli_detail::unexpected_error(files.error());
  }
  print_context_tree_table(*files);
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
  add_generation_option(parser, options.tree_generation,
                        "Generation id. Defaults to the selected type's "
                        "latest ref.");
}

void add_agents_cat_command(Argum::Parser &parser, StoreCliOptions &options) {
  options.cat_type_name =
      std::string(memix::to_string(memix::MemoryObjectType::Agents));
  parser.add(Argum::Positional("agent-path")
                 .help("Logical path inside the agents tree.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.cat_path = fs::path(value);
                 }));
  add_generation_option(parser, options.cat_generation,
                        "Generation id. Defaults to agents/latest.");
}

void add_agents_tree_command(Argum::Parser &parser, StoreCliOptions &options) {
  options.tree_type_name =
      std::string(memix::to_string(memix::MemoryObjectType::Agents));
  add_generation_option(parser, options.tree_generation,
                        "Generation id. Defaults to agents/latest.");
}

void add_agents_commands(Argum::Parser &parser, StoreCliOptions &options) {
  options.command = StoreCommand::Agents;
  parser.addSubCommand(
      Argum::Positional("agents-command")
          .occurs(Argum::neverOrOnce)
          .help("Agents command: cat or tree.")
          .handler([&parser,
                    &options](std::string_view value) -> Argum::Expected<void> {
            if (value == "cat") {
              options.command = StoreCommand::AgentsCat;
              add_agents_cat_command(parser, options);
              return {};
            }
            if (value == "tree") {
              options.command = StoreCommand::AgentsTree;
              add_agents_tree_command(parser, options);
              return {};
            }
            return memix::cli_detail::argum_validation_error(std::format(
                "unsupported agents command `{}`; expected cat|tree", value));
          }));
}

void add_commands(Argum::Parser &parser, StoreCliOptions &options) {
  parser.addSubCommand(
      Argum::Positional("subcommand")
          .occurs(Argum::neverOrOnce)
          .help("Subcommand: list, ingest, cat, tree, or agents.")
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
            if (value == "agents") {
              add_agents_commands(parser, options);
              return {};
            }
            return memix::cli_detail::argum_validation_error(
                std::format("unsupported command `{}`; expected "
                            "list|ingest|cat|tree|agents",
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

void print_agents_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_agents_commands(parser, options);
  std::print("{}", parser.formatHelp("memix-store agents"));
}

void print_agents_cat_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_agents_cat_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store agents cat"));
}

void print_agents_tree_help() {
  Argum::Parser parser;
  StoreCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_agents_tree_command(parser, options);
  std::print("{}", parser.formatHelp("memix-store agents tree"));
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
  case StoreCommand::Agents:
    print_agents_help();
    return;
  case StoreCommand::AgentsCat:
    print_agents_cat_help();
    return;
  case StoreCommand::AgentsTree:
    print_agents_tree_help();
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
  case StoreCommand::AgentsCat:
    return run_cat(options);
  case StoreCommand::Tree:
  case StoreCommand::AgentsTree:
    return run_tree(options);
  case StoreCommand::Agents:
    return memix::cli_detail::unexpected_error(
        "agents requires a subcommand: cat|tree");
  case StoreCommand::None:
    return memix::cli_detail::unexpected_error("a subcommand is required");
  }
  return memix::cli_detail::unexpected_error("unknown command");
}

int run_cli(int argc, char **argv,
            const std::shared_ptr<spdlog::logger> &logger) {
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
