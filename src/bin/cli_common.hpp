#pragma once

#include "memix/store.hpp"
#include "memix/version.hpp"

#include <argum/expected.h>
#include <argum/parser.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace memix::cli_detail {

inline constexpr std::size_t kShortIdLength = 12;

struct CliError {
  std::string message;
};

template <typename Value>
using Result = std::expected<Value, CliError>;

using VoidResult = std::expected<void, CliError>;

inline std::unexpected<CliError> unexpected_error(std::string_view message) {
  return std::unexpected(CliError{.message = std::string(message)});
}

inline std::unexpected<CliError> unexpected_error(const StoreError &error) {
  return unexpected_error(error.message);
}

inline Argum::Expected<void> argum_validation_error(std::string_view message) {
  return {Argum::Failure<Argum::Parser::ValidationError>, message};
}

inline std::shared_ptr<spdlog::logger>
make_logger(std::string_view program_name) {
  auto logger = spdlog::stderr_color_mt(std::string(program_name));
  logger->set_pattern("[%T] [%n] [%^%l%$] %v");
  logger->flush_on(spdlog::level::info);
  return logger;
}

inline Result<std::filesystem::path>
resolve_store_root(const std::filesystem::path &store_root_override) {
  if (!store_root_override.empty()) {
    return store_root_override;
  }
  const std::expected<std::filesystem::path, StoreError> root =
      default_store_root();
  if (!root) {
    return unexpected_error(root.error());
  }
  return *root;
}

inline Result<std::string>
read_file_contents(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return unexpected_error(
        std::format("failed to open `{}` for reading", path.string()));
  }
  std::string contents{
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
  if (input.bad()) {
    return unexpected_error(std::format("failed to read `{}`", path.string()));
  }
  return contents;
}

inline std::string short_id(std::string_view value) {
  return std::string(value.substr(0, std::min(value.size(), kShortIdLength)));
}

inline std::string
join_json_documents_as_array(const std::vector<std::string> &documents) {
  std::string output = "[";
  bool needs_comma = false;
  for (const std::string &document : documents) {
    if (needs_comma) {
      output += ",";
    }
    output += document;
    needs_comma = true;
  }
  output += "]";
  return output;
}

struct CommonOptions {
  std::filesystem::path store_root;
  bool help_requested = false;
  bool version_requested = false;
};

inline void add_common_options(Argum::Parser &parser, CommonOptions &options) {
  parser.add(Argum::Option("--help", "-h")
                 .help("Print this help message and exit")
                 .handler([&options]() { options.help_requested = true; }));
  parser.add(Argum::Option("--version")
                 .help("Display program version information and exit")
                 .handler([&options]() { options.version_requested = true; }));
  parser.add(Argum::Option("--store-root")
                 .argName("TEXT")
                 .help("Store root. Defaults to MEMIX_STORE_HOME, "
                       "XDG_DATA_HOME/memix, or HOME/.memix.")
                 .handler([&options](std::string_view value) {
                   options.store_root = std::filesystem::path(value);
                 }));
}

inline VoidResult parse_arguments(Argum::Parser &parser, int argc,
                                  char **argv) {
  const Argum::Expected<void> result = parser.parse(argc, argv);
  if (auto error = result.error()) {
    return unexpected_error(error->message());
  }
  return {};
}

inline void print_version() { std::println("memix {}", MEMIX_VERSION); }

struct PathIdTableLayout {
  std::size_t minimum_path_width = 0;
  int id_column_width = 0;
};

template <typename Entry, typename PathText, typename IdText>
void print_path_id_table(const std::vector<Entry> &entries,
                         PathIdTableLayout layout, PathText path_text,
                         IdText id_text) {
  std::size_t path_width = layout.minimum_path_width;
  for (const Entry &entry : entries) {
    path_width = std::max(path_width, path_text(entry).size());
  }

  std::println("{:<{}}{:<{}}", "PATH", path_width + 2, "ID",
               layout.id_column_width);
  for (const Entry &entry : entries) {
    std::println("{:<{}}{:<{}}", path_text(entry), path_width + 2,
                 short_id(id_text(entry)), layout.id_column_width);
  }
}

} // namespace memix::cli_detail
