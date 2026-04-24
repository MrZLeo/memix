#include "memix/version.hpp"

#include <cstdio>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

std::shared_ptr<spdlog::logger> make_logger() {
  auto logger = spdlog::stderr_color_mt("memix-view");
  logger->set_pattern("[%T] [%n] [%^%l%$] %v");
  logger->flush_on(spdlog::level::info);
  return logger;
}

void fallback_print_error(std::string_view message) noexcept {
  static constexpr std::string_view prefix = "memix-view: ";
  static constexpr std::string_view newline = "\n";
  static_cast<void>(
      std::fwrite(prefix.data(), sizeof(char), prefix.size(), stderr));
  static_cast<void>(
      std::fwrite(message.data(), sizeof(char), message.size(), stderr));
  static_cast<void>(
      std::fwrite(newline.data(), sizeof(char), newline.size(), stderr));
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
    CLI::App app{"memix view utilities."};
    std::vector<std::string> show_items;

    app.set_version_flag("--version", std::format("memix {}", MEMIX_VERSION));
    app.require_subcommand(1);

    auto *list_cmd = app.add_subcommand("list", "List the available views.");
    list_cmd->callback([&]() { logger->info("list is not implemented yet"); });

    auto *show_cmd =
        app.add_subcommand("show", "Show one or more items in the view.");
    show_cmd->add_option("item", show_items, "Item(s) to display in the view.")
        ->required();
    show_cmd->callback([&]() {
      logger->info("show is not implemented yet for {} item(s)",
                   show_items.size());
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
