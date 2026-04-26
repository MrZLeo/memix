#include "memix/store.hpp"
#include "memix/version.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <simdjson.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

constexpr int kTypeColumnWidth = 11;
constexpr int kIdColumnWidth = 14;
constexpr int kSizeColumnWidth = 10;
constexpr int kTimestampColumnWidth = 21;
constexpr int kAgentPathColumnWidth = 24;
constexpr std::uintmax_t kSizeUnitBaseBytes = 1024;
constexpr double kSizeUnitBase = 1024.0;
constexpr std::size_t kShortIdLength = 12;
constexpr std::size_t kJsonIndentSpaces = 2;

std::string supported_types_message() {
  std::string message;
  const std::vector<memix::MemoryObjectType> types =
      memix::memory_object_types();
  for (const memix::MemoryObjectType type : types) {
    if (!message.empty()) {
      message += "|";
    }
    message += memix::to_string(type);
  }
  return message;
}

std::unexpected<memix::StoreError> store_error(std::string message) {
  return std::unexpected(memix::StoreError{.message = std::move(message)});
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

memix::MemoryObjectType require_type(std::string_view value) {
  const auto type = memix::parse_memory_object_type(value);
  if (!type.has_value()) {
    throw std::runtime_error(
        std::format("unsupported object type `{}`; expected {}", value,
                    supported_types_message()));
  }
  return *type;
}

void print_table(const std::vector<memix::StoredObject> &objects) {
  std::println("{:<{}}{:<{}}{:>{}}  {:<{}}{}", "TYPE", kTypeColumnWidth, "ID",
               kIdColumnWidth, "SIZE", kSizeColumnWidth, "CREATED_AT",
               kTimestampColumnWidth, "FILENAME");
  for (const memix::StoredObject &object : objects) {
    const std::string size = format_size(object.size_bytes);
    std::println("{:<{}}{:<{}}{:>{}}  {:<{}}{}", memix::to_string(object.type),
                 kTypeColumnWidth, object.id.substr(0, kShortIdLength),
                 kIdColumnWidth, size, kSizeColumnWidth, object.created_at,
                 kTimestampColumnWidth, object.filename);
  }
}

void print_agent_tree_table(const std::vector<memix::AgentFile> &files) {
  std::size_t path_width = kAgentPathColumnWidth;
  for (const memix::AgentFile &file : files) {
    path_width = std::max(path_width, file.agent_path.generic_string().size());
  }

  std::println("{:<{}}{:<{}}", "PATH", path_width + 2, "ID", kIdColumnWidth);
  for (const memix::AgentFile &file : files) {
    std::println("{:<{}}{:<{}}", file.agent_path.generic_string(),
                 path_width + 2, file.blob.id.substr(0, kShortIdLength),
                 kIdColumnWidth);
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

std::string read_file_contents(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error(
        std::format("failed to open `{}` for reading", path.string()));
  }
  std::string contents{
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
  if (input.bad()) {
    throw std::runtime_error(std::format("failed to read `{}`", path.string()));
  }
  return contents;
}

void print_json_array(const std::vector<memix::StoredObject> &objects) {
  const std::expected<std::string, memix::StoreError> json =
      metadata_json_array(objects);
  if (!json) {
    throw std::runtime_error(json.error().message);
  }
  const std::expected<std::string, memix::StoreError> formatted_json =
      pretty_json(*json);
  if (!formatted_json) {
    throw std::runtime_error(formatted_json.error().message);
  }
  std::println("{}", *formatted_json);
}

std::filesystem::path
resolve_store_root(const std::filesystem::path &store_root_override) {
  if (!store_root_override.empty()) {
    return store_root_override;
  }
  const std::expected<std::filesystem::path, memix::StoreError> root =
      memix::default_store_root();
  if (!root) {
    throw std::runtime_error(root.error().message);
  }
  return *root;
}

void fallback_print_error(std::string_view message) noexcept {
  static constexpr std::string_view prefix = "memix-store: ";
  static constexpr std::string_view newline = "\n";
  static_cast<void>(
      std::fwrite(prefix.data(), sizeof(char), prefix.size(), stderr));
  static_cast<void>(
      std::fwrite(message.data(), sizeof(char), message.size(), stderr));
  static_cast<void>(
      std::fwrite(newline.data(), sizeof(char), newline.size(), stderr));
}

std::shared_ptr<spdlog::logger> make_logger() {
  auto logger = spdlog::stderr_color_mt("memix-store");
  logger->set_pattern("[%T] [%n] [%^%l%$] %v");
  logger->flush_on(spdlog::level::info);
  return logger;
}

void log_error(const std::shared_ptr<spdlog::logger> &logger,
               std::string_view message) noexcept {
  try {
    if (logger) {
      logger->error("{}", message);
      return;
    }
  } catch (...) {
    fallback_print_error(message);
    return;
  }
  fallback_print_error(message);
}

struct StoreCliOptions {
  std::filesystem::path store_root;
  std::vector<std::filesystem::path> ingest_paths;
  std::string ingest_type_name;
  std::filesystem::path ingest_agent_dest;
  std::string ingest_message;
  std::string list_type_name;
  bool list_json = false;
  std::filesystem::path agent_cat_path;
  std::string agent_cat_generation;
  std::string agent_tree_generation;
};

memix::Store make_store(const StoreCliOptions &options) {
  return memix::Store(resolve_store_root(options.store_root));
}

std::optional<memix::MemoryObjectType>
optional_type(std::string_view type_name) {
  if (type_name.empty()) {
    return std::nullopt;
  }
  return require_type(type_name);
}

std::optional<std::string_view>
optional_generation(const std::string &generation) {
  if (generation.empty()) {
    return std::nullopt;
  }
  return generation;
}

void run_list(const StoreCliOptions &options) {
  const memix::Store store(make_store(options));
  const std::expected<std::vector<memix::StoredObject>, memix::StoreError>
      objects = store.list(optional_type(options.list_type_name));
  if (!objects) {
    throw std::runtime_error(objects.error().message);
  }
  if (options.list_json) {
    print_json_array(*objects);
    return;
  }
  print_table(*objects);
}

void run_ingest(const StoreCliOptions &options, spdlog::logger &logger) {
  const memix::MemoryObjectType type = require_type(options.ingest_type_name);
  const memix::Store store(make_store(options));

  if (type == memix::MemoryObjectType::Agents) {
    if (options.ingest_agent_dest.empty()) {
      throw std::runtime_error(
          "ingest --type agents requires --dest with a logical agents path");
    }
    if (options.ingest_paths.size() != 1) {
      throw std::runtime_error(
          "ingest --type agents expects exactly one source path");
    }
    const std::expected<memix::AgentIngestResult, memix::StoreError> result =
        store.ingest_agent_file({
            .source_path = options.ingest_paths.front(),
            .agent_path = options.ingest_agent_dest,
            .message = options.ingest_message,
        });
    if (!result) {
      throw std::runtime_error(result.error().message);
    }
    logger.info("{} type={} id={} filename={} path={} generation={}",
                result->blob.inserted ? "ingested" : "exists",
                memix::to_string(result->blob.object.type),
                result->blob.object.id, result->blob.object.filename,
                result->agent_path.generic_string(), result->generation.id);
    return;
  }

  if (!options.ingest_agent_dest.empty()) {
    throw std::runtime_error("--dest is only valid with --type agents");
  }
  if (!options.ingest_message.empty()) {
    throw std::runtime_error("--message is only valid with --type agents");
  }

  for (const auto &path : options.ingest_paths) {
    const std::expected<memix::IngestResult, memix::StoreError> result =
        store.ingest(type, path);
    if (!result) {
      throw std::runtime_error(result.error().message);
    }
    logger.info("{} type={} id={} filename={}",
                result->inserted ? "ingested" : "exists",
                memix::to_string(result->object.type), result->object.id,
                result->object.filename);
  }
}

void run_agents_cat(const StoreCliOptions &options) {
  const memix::Store store(make_store(options));
  const std::expected<memix::StoredObject, memix::StoreError> object =
      store.read_agent_file(options.agent_cat_path,
                            optional_generation(options.agent_cat_generation));
  if (!object) {
    throw std::runtime_error(object.error().message);
  }
  std::print("{}",
             read_file_contents(object->object_dir / object->content_path));
}

void run_agents_tree(const StoreCliOptions &options) {
  const memix::Store store(make_store(options));
  const std::expected<std::vector<memix::AgentFile>, memix::StoreError> files =
      store.list_agent_files(
          optional_generation(options.agent_tree_generation));
  if (!files) {
    throw std::runtime_error(files.error().message);
  }
  print_agent_tree_table(*files);
}

void add_global_options(CLI::App &app, StoreCliOptions &options) {
  app.set_version_flag("--version", std::format("memix {}", MEMIX_VERSION));
  app.add_option("--store-root", options.store_root,
                 "Store root. Defaults to MEMIX_STORE_HOME, "
                 "XDG_DATA_HOME/memix, or HOME/.memix.");
  app.require_subcommand(1);
}

void add_list_command(CLI::App &app, StoreCliOptions &options) {
  auto *list_cmd =
      app.add_subcommand("list", "List the contents of the store.");
  list_cmd->add_option(
      "--type", options.list_type_name,
      std::format("Filter by object type: {}", supported_types_message()));
  list_cmd->add_flag("--json", options.list_json,
                     "Emit metadata as a JSON array instead of a table.");
  list_cmd->callback([&options]() { run_list(options); });
}

void add_ingest_command(CLI::App &app, StoreCliOptions &options,
                        spdlog::logger &logger) {
  auto *ingest_cmd =
      app.add_subcommand("ingest", "Ingest one or more paths into the store.");
  ingest_cmd
      ->add_option("--type", options.ingest_type_name,
                   std::format("Object type: {}", supported_types_message()))
      ->required();
  ingest_cmd
      ->add_option("path", options.ingest_paths,
                   "Source path(s) to ingest into the store.")
      ->required();
  ingest_cmd->add_option(
      "--dest", options.ingest_agent_dest,
      "Logical destination path inside the agents tree. Required when "
      "--type agents.");
  ingest_cmd->add_option("--message", options.ingest_message,
                         "Agent generation message. Only valid with "
                         "--type agents.");
  ingest_cmd->callback([&options, &logger]() { run_ingest(options, logger); });
}

void add_agents_commands(CLI::App &app, StoreCliOptions &options,
                         spdlog::logger & /*logger*/) {
  auto *agents_cmd =
      app.add_subcommand("agents", "Manage the agents file tree.");
  agents_cmd->require_subcommand(1);

  auto *agents_cat_cmd =
      agents_cmd->add_subcommand("cat", "Print a file from an agents tree.");
  agents_cat_cmd
      ->add_option("agent-path", options.agent_cat_path,
                   "Logical path inside the agents tree.")
      ->required();
  agents_cat_cmd->add_option("--generation", options.agent_cat_generation,
                             "Generation id. Defaults to agents/latest.");
  agents_cat_cmd->callback([&options]() { run_agents_cat(options); });

  auto *agents_tree_cmd =
      agents_cmd->add_subcommand("tree", "List files in an agents tree.");
  agents_tree_cmd->add_option("--generation", options.agent_tree_generation,
                              "Generation id. Defaults to agents/latest.");
  agents_tree_cmd->callback([&options]() { run_agents_tree(options); });
}

int run_cli(int argc, char **argv, spdlog::logger &logger) {
  CLI::App app{"memix store utilities."};
  StoreCliOptions options;

  add_global_options(app, options);
  add_list_command(app, options);
  add_ingest_command(app, options, logger);
  add_agents_commands(app, options, logger);

  CLI11_PARSE(app, argc, argv);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  std::shared_ptr<spdlog::logger> logger;
  try {
    logger = make_logger();
    return run_cli(argc, argv, *logger);
  } catch (const std::exception &error) {
    log_error(logger, error.what());
    return 1;
  } catch (...) {
    log_error(logger, "unknown error");
    return 1;
  }

  return 0;
}
