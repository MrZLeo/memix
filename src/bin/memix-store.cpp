#include "memix/store.hpp"
#include "memix/version.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
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
    std::println("{:<{}}{:<{}}{:>{}}  {:<{}}{}", memix::to_string(object.type),
                 kTypeColumnWidth, object.id.substr(0, kShortIdLength),
                 kIdColumnWidth, object.size_bytes, kSizeColumnWidth,
                 object.created_at, kTimestampColumnWidth, object.filename);
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

} // namespace

int main(int argc, char **argv) {
  std::shared_ptr<spdlog::logger> logger;
  try {
    logger = make_logger();
    CLI::App app{"memix store utilities."};
    std::filesystem::path store_root;
    std::vector<std::string> ingest_paths;
    std::string ingest_type_name;
    std::string list_type_name;
    bool list_json = false;

    app.set_version_flag("--version", std::format("memix {}", MEMIX_VERSION));
    app.add_option(
        "--store-root", store_root,
        "Store root. Defaults to MEMIX_STORE_HOME, XDG_DATA_HOME/memix, "
        "or HOME/.memix.");
    app.require_subcommand(1);

    auto *list_cmd =
        app.add_subcommand("list", "List the contents of the store.");
    list_cmd->add_option(
        "--type", list_type_name,
        std::format("Filter by object type: {}", supported_types_message()));
    list_cmd->add_flag("--json", list_json,
                       "Emit metadata as a JSON array instead of a table.");

    auto *ingest_cmd = app.add_subcommand(
        "ingest", "Ingest one or more paths into the store.");
    ingest_cmd
        ->add_option("--type", ingest_type_name,
                     std::format("Object type: {}", supported_types_message()))
        ->required();
    ingest_cmd
        ->add_option("path", ingest_paths, "Path(s) to ingest into the store.")
        ->required();

    list_cmd->callback([&]() {
      const auto type = list_type_name.empty()
                          ? std::optional<memix::MemoryObjectType>{}
                          : std::optional<memix::MemoryObjectType>{
                                require_type(list_type_name)};
      const memix::Store store(resolve_store_root(store_root));
      const std::expected<std::vector<memix::StoredObject>, memix::StoreError>
          objects = store.list(type);
      if (!objects) {
        throw std::runtime_error(objects.error().message);
      }
      if (list_json) {
        print_json_array(*objects);
      } else {
        print_table(*objects);
      }
    });
    ingest_cmd->callback([&]() {
      const memix::MemoryObjectType type = require_type(ingest_type_name);
      const memix::Store store(resolve_store_root(store_root));
      for (const auto &path : ingest_paths) {
        const std::expected<memix::IngestResult, memix::StoreError> result =
            store.ingest(type, path);
        if (!result) {
          throw std::runtime_error(result.error().message);
        }
        logger->info("{} type={} id={} filename={}",
                     result->inserted ? "ingested" : "exists",
                     memix::to_string(result->object.type), result->object.id,
                     result->object.filename);
      }
    });

    CLI11_PARSE(app, argc, argv);
  } catch (const std::exception &error) {
    log_error(logger, error.what());
    return 1;
  } catch (...) {
    log_error(logger, "unknown error");
    return 1;
  }

  return 0;
}
