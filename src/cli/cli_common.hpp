#pragma once

#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"
#include "memix/version.hpp"

#include <argum/expected.h>
#include <argum/parser.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstddef>
#include <expected>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memix::cli_detail {

namespace fs = memix::fs;

inline constexpr std::size_t kShortIdLength = 12;

struct CliError {
  std::string message;
  std::optional<FsError> filesystem_error;
  ErrorKind kind = ErrorKind::Unknown;
};

template <typename Value>
using Result = std::expected<Value, CliError>;

using VoidResult = std::expected<void, CliError>;

inline std::unexpected<CliError> unexpected_error(std::string_view message) {
  CliError error;
  error.message = std::string(message);
  return std::unexpected(std::move(error));
}

inline std::unexpected<CliError> unexpected_error(const StoreError &error) {
  return std::unexpected(CliError{
      .message = error.message,
      .filesystem_error = error.filesystem_error,
      .kind = error.kind,
  });
}

inline std::unexpected<CliError> unexpected_error(const ViewError &error) {
  return std::unexpected(CliError{
      .message = error.message,
      .filesystem_error = error.filesystem_error,
      .kind = error.kind,
  });
}

inline std::unexpected<CliError> unexpected_error(const FsError &error) {
  return std::unexpected(CliError{
      .message = error.message,
      .filesystem_error = error,
      .kind = ErrorKind::Filesystem,
  });
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

inline Result<fs::path>
resolve_store_root(const fs::path &store_root_override) {
  if (!store_root_override.empty()) {
    return store_root_override;
  }
  const StoreResult<fs::path> root = default_store_root();
  if (!root) {
    return unexpected_error(root.error());
  }
  return *root;
}

inline Result<std::string> read_file_contents(const fs::path &path) {
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
  return value
       | std::views::take(kShortIdLength)
       | std::ranges::to<std::string>();
}

inline std::string
join_json_documents_as_array(const std::vector<std::string> &documents) {
  return std::format("[{}]", documents
                                 | std::views::join_with(',')
                                 | std::ranges::to<std::string>());
}

struct CommonOptions {
  fs::path store_root;
  bool help_requested = false;
  bool version_requested = false;
};

inline void add_common_options(Argum::Parser &parser, CommonOptions &options,
                               std::string_view store_help =
                                   "Store root. Defaults to MEMIX_STORE_HOME, "
                                   "XDG_DATA_HOME/memix, or HOME/.memix.") {
  parser.add(Argum::Option("--help", "-h")
                 .help("Print this help message and exit")
                 .handler([&options] { options.help_requested = true; }));
  parser.add(Argum::Option("--version")
                 .help("Display program version information and exit")
                 .handler([&options] { options.version_requested = true; }));
  parser.add(Argum::Option("--store-root")
                 .argName("TEXT")
                 .help(store_help)
                 .handler([&options](std::string_view value) {
                   options.store_root = fs::path(value);
                 }));
}

inline VoidResult parse_arguments(const Argum::Parser &parser,
                                  const std::vector<std::string> &arguments) {
  const Argum::Expected<void> result = parser.parse(arguments);
  if (const auto error = result.error()) {
    return unexpected_error(error->message());
  }
  return {};
}

inline void print_version() { std::println("memix {}", MEMIX_VERSION); }

} // namespace memix::cli_detail
