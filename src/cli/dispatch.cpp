#include "commands.hpp"

#include "cli_common.hpp"

#include <argum/data.h>
#include <argum/parser.h>

#include <format>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_help() {
  Argum::Parser parser;
  memix::cli_detail::CommonOptions options;
  memix::cli_detail::add_common_options(parser, options);
  parser.add(Argum::Positional("command")
                 .occurs(Argum::neverOrOnce)
                 .help("Command: store (source memory), view (derived views "
                       "and activation), or tui (terminal inspector)."));
  std::print("{}", parser.formatHelp("memix"));
  std::println("\nRun 'memix <command> --help' for command-specific help.\n"
               "Common options may precede or follow the command.");
}

} // namespace

int memix::cli::run(const std::vector<std::string> &arguments) {
  Argum::Parser parser;
  cli_detail::CommonOptions options;
  cli_detail::add_common_options(parser, options);
  const auto logger = cli_detail::make_logger("memix");

  // With no positionals registered, parsing stops at the namespace. Its
  // remaining arguments (including '--') belong exclusively to that CLI.
  const auto parsed = parser.parseUntilUnknown(arguments);
  if (!parsed) {
    logger->error("{}", parsed.error()->message());
    return 1;
  }
  if (parsed->empty()) {
    if (options.version_requested) {
      cli_detail::print_version();
      return 0;
    }
    if (options.help_requested || arguments.empty()) {
      print_help();
      return 0;
    }
    logger->error("a command is required: store|view|tui");
    return 1;
  }

  const std::string &command = parsed->front();
  using EntryPoint =
      int (*)(const std::vector<std::string> &, std::string_view);
  EntryPoint entry = nullptr;
  if (command == "store") {
    entry = run_store;
  } else if (command == "view") {
    entry = run_view;
  } else if (command == "tui") {
    entry = run_tui;
  } else {
    logger->error("unsupported command `{}`; expected store|view|tui", command);
    return 1;
  }

  std::vector<std::string> forwarded;
  if (!options.store_root.empty()) {
    // Keep paths beginning with '-' literal during the namespace's parse.
    forwarded.push_back(
        std::format("--store-root={}", options.store_root.string()));
  }
  if (options.help_requested) {
    forwarded.emplace_back("--help");
  }
  if (options.version_requested) {
    forwarded.emplace_back("--version");
  }
  forwarded.append_range(*parsed | std::views::drop(1));
  return entry(forwarded, std::format("memix {}", command));
}
