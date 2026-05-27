#include "memix/view.hpp"

#include "cli_common.hpp"

#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"

#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <argum/data.h>
#include <argum/expected.h>
#include <argum/parser.h>
#include <spdlog/logger.h>

namespace fs = memix::fs;

namespace {

enum class ViewCommand : std::uint8_t {
  None,
  Build,
  Show,
};

struct ViewCliOptions {
  memix::cli_detail::CommonOptions common;
  ViewCommand command = ViewCommand::None;
  std::vector<std::string> sources;
  std::string target_name = "file_tree";
  bool build_json = false;
  std::string show_view_environment_id;
  bool show_json = false;
};

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
require_build_target(std::string_view target_name) {
  const std::optional<memix::ViewTargetKind> target =
      memix::parse_view_target_kind(target_name);
  if (!target.has_value() || *target != memix::ViewTargetKind::FileTree) {
    return memix::cli_detail::unexpected_error(std::format(
        "unsupported build target `{}`; expected file_tree", target_name));
  }
  return *target;
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

memix::cli_detail::VoidResult run_build(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewTargetKind> target =
      require_build_target(options.target_name);
  if (!target) {
    return std::unexpected(target.error());
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      service->expression_from_sources(options.sources, *target);
  if (!expression) {
    return memix::cli_detail::unexpected_error(expression.error());
  }
  const memix::ViewResult<memix::ViewEnvironment> environment =
      service->build(*expression);
  if (!environment) {
    return memix::cli_detail::unexpected_error(environment.error());
  }
  if (options.build_json) {
    const memix::cli_detail::Result<std::string> contents =
        memix::cli_detail::read_file_contents(environment->manifest_path);
    if (!contents) {
      return std::unexpected(contents.error());
    }
    std::println("{}", *contents);
    return {};
  }
  print_view_environment_summary(*environment);
  return {};
}

memix::cli_detail::VoidResult run_show(const ViewCliOptions &options) {
  if (options.show_view_environment_id.empty()) {
    return memix::cli_detail::unexpected_error(
        "show expects an environment id");
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ViewEnvironment> environment =
      service->resolve_environment(options.show_view_environment_id);
  if (!environment) {
    return memix::cli_detail::unexpected_error(environment.error());
  }
  if (options.show_json) {
    const memix::cli_detail::Result<std::string> contents =
        memix::cli_detail::read_file_contents(environment->manifest_path);
    if (!contents) {
      return std::unexpected(contents.error());
    }
    std::println("{}", *contents);
    return {};
  }
  print_view_environment_summary(*environment);
  return {};
}

void add_build_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--source")
                 .argName("TEXT")
                 .help("View locator, for example agents/repo/AGENTS.md or "
                       "chat/task/2026-05-20/@latest.")
                 .handler([&options](std::string_view value) {
                   options.sources.emplace_back(value);
                 }));
  parser.add(Argum::Option("--target")
                 .argName("TEXT")
                 .help("Build target. Only file_tree is supported.")
                 .handler([&options](std::string_view value) {
                   options.target_name = value;
                 }));
  parser.add(Argum::Option("--json")
                 .help("Emit the view environment manifest JSON.")
                 .handler([&options]() { options.build_json = true; }));
}

void add_show_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("environment-id")
                 .help("View environment id or unique prefix.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.show_view_environment_id = value;
                 }));
  parser.add(Argum::Option("--json")
                 .help("Emit the view environment manifest JSON.")
                 .handler([&options]() { options.show_json = true; }));
}

void add_commands(Argum::Parser &parser, ViewCliOptions &options) {
  parser.addSubCommand(
      Argum::Positional("subcommand")
          .occurs(Argum::neverOrOnce)
          .help("Subcommand: build or show.")
          .handler([&parser,
                    &options](std::string_view value) -> Argum::Expected<void> {
            if (value == "build") {
              options.command = ViewCommand::Build;
              add_build_command(parser, options);
              return {};
            }
            if (value == "show") {
              options.command = ViewCommand::Show;
              add_show_command(parser, options);
              return {};
            }
            return memix::cli_detail::argum_validation_error(std::format(
                "unsupported command `{}`; expected build|show", value));
          }));
}

void print_root_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_commands(parser, options);
  std::print("{}", parser.formatHelp("memix-view"));
}

void print_build_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_build_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view build"));
}

void print_show_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_show_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view show"));
}

void print_selected_help(ViewCommand command) {
  switch (command) {
  case ViewCommand::Build:
    print_build_help();
    return;
  case ViewCommand::Show:
    print_show_help();
    return;
  case ViewCommand::None:
    print_root_help();
    return;
  }
}

memix::cli_detail::VoidResult dispatch(const ViewCliOptions &options) {
  switch (options.command) {
  case ViewCommand::Build:
    return run_build(options);
  case ViewCommand::Show:
    return run_show(options);
  case ViewCommand::None:
    return memix::cli_detail::unexpected_error("a subcommand is required");
  }
  return memix::cli_detail::unexpected_error("unknown command");
}

int run_cli(int argc, char **argv,
            const std::shared_ptr<spdlog::logger> &logger) {
  Argum::Parser parser;
  ViewCliOptions options;

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

  const memix::cli_detail::VoidResult result = dispatch(options);
  if (!result) {
    logger->error("{}", result.error().message);
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::shared_ptr<spdlog::logger> logger =
      memix::cli_detail::make_logger("memix-view");
  return run_cli(argc, argv, logger);
}
