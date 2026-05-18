#include "memix/store.hpp"
#include "memix/view.hpp"

#include "cli_common.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <argum/data.h>
#include <argum/expected.h>
#include <argum/parser.h>
#include <spdlog/logger.h>

namespace {

constexpr int kDerivationColumnWidth = 14;
constexpr int kExpressionColumnWidth = 14;
constexpr int kTypeColumnWidth = 11;
constexpr int kGenerationColumnWidth = 14;
constexpr int kInputsColumnWidth = 6;
constexpr int kTreePathColumnWidth = 24;
constexpr int kTreeIdColumnWidth = 14;

enum class ViewCommand : std::uint8_t {
  None,
  Build,
  Show,
  Cd,
  Tree,
  List,
};

struct ViewTreeEntry {
  std::filesystem::path path;
  std::string blob_id;
};

struct ViewCliOptions {
  memix::cli_detail::CommonOptions common;
  ViewCommand command = ViewCommand::None;
  std::string type_name;
  std::vector<std::filesystem::path> include_paths;
  std::vector<std::filesystem::path> include_optional_paths;
  std::string generation;
  std::string ref;
  std::string name = "file-tree";
  bool build_json = false;
  std::string show_derivation_id;
  std::string cd_derivation_id;
  std::string tree_derivation_id;
  bool list_json = false;
};

std::string supported_types_message() {
  return memix::memory_object_types()
       | std::views::transform([](memix::MemoryObjectType type) {
           return memix::to_string(type);
         })
       | std::views::join_with('|')
       | std::ranges::to<std::string>();
}

memix::cli_detail::Result<memix::MemoryObjectType>
optional_source_type(std::string_view value) {
  if (value.empty()) {
    return memix::MemoryObjectType::Agents;
  }
  const std::optional<memix::MemoryObjectType> type =
      memix::parse_memory_object_type(value);
  if (!type.has_value()) {
    return memix::cli_detail::unexpected_error(
        std::format("unsupported object type `{}`; expected {}", value,
                    supported_types_message()));
  }
  return *type;
}

void print_artifact_table(const std::vector<memix::ViewArtifact> &artifacts) {
  std::println("{:<{}}{:<{}}{:<{}}{:<{}}{:>{}}", "DERIVATION",
               kDerivationColumnWidth, "EXPRESSION", kExpressionColumnWidth,
               "TYPE", kTypeColumnWidth, "GENERATION", kGenerationColumnWidth,
               "INPUTS", kInputsColumnWidth);
  for (const memix::ViewArtifact &artifact : artifacts) {
    std::println("{:<{}}{:<{}}{:<{}}{:<{}}{:>{}}",
                 memix::cli_detail::short_id(artifact.derivation_id),
                 kDerivationColumnWidth,
                 memix::cli_detail::short_id(artifact.expression_id),
                 kExpressionColumnWidth, memix::to_string(artifact.source_type),
                 kTypeColumnWidth,
                 memix::cli_detail::short_id(artifact.source_generation),
                 kGenerationColumnWidth, artifact.input_blob_ids.size(),
                 kInputsColumnWidth);
  }
}

void print_artifact_summary(const memix::ViewArtifact &artifact) {
  std::println("derivation_id: {}", artifact.derivation_id);
  std::println("expression_id: {}", artifact.expression_id);
  std::println("source_type: {}", memix::to_string(artifact.source_type));
  std::println("source_generation: {}", artifact.source_generation);
  std::println("input_count: {}", artifact.input_blob_ids.size());
  std::println("manifest: {}", artifact.manifest_path.string());
  std::println("tree: {}", artifact.tree_path.string());
  std::println("inserted: {}", artifact.inserted ? "true" : "false");
}

memix::cli_detail::VoidResult
print_artifact_json(const memix::ViewArtifact &artifact) {
  const memix::cli_detail::Result<std::string> contents =
      memix::cli_detail::read_file_contents(artifact.manifest_path);
  if (!contents) {
    return std::unexpected(contents.error());
  }
  std::println("{}", *contents);
  return {};
}

memix::cli_detail::VoidResult
print_artifacts_json(const std::vector<memix::ViewArtifact> &artifacts) {
  std::vector<std::string> manifests;
  manifests.reserve(artifacts.size());
  for (const memix::ViewArtifact &artifact : artifacts) {
    const memix::cli_detail::Result<std::string> contents =
        memix::cli_detail::read_file_contents(artifact.manifest_path);
    if (!contents) {
      return std::unexpected(contents.error());
    }
    manifests.push_back(*contents);
  }
  std::println("{}",
               memix::cli_detail::join_json_documents_as_array(manifests));
  return {};
}

memix::cli_detail::Result<std::string>
blob_id_for_tree_link(const std::filesystem::path &link_path) {
  std::error_code error;
  const std::filesystem::path target =
      std::filesystem::read_symlink(link_path, error);
  if (error) {
    return memix::cli_detail::unexpected_error(
        std::format("failed to read symlink `{}`: {}", link_path.string(),
                    error.message()));
  }
  if (target.filename() != "object") {
    return memix::cli_detail::unexpected_error(
        std::format("view tree symlink target is not a store object `{}`",
                    link_path.string()));
  }
  return target.parent_path().filename().string();
}

memix::cli_detail::Result<std::vector<ViewTreeEntry>>
collect_view_tree_entries(const memix::ViewArtifact &artifact) {
  std::vector<ViewTreeEntry> entries;

  std::error_code iterator_error;
  const std::filesystem::recursive_directory_iterator end;
  for (std::filesystem::recursive_directory_iterator iterator{
           artifact.tree_path, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const std::filesystem::directory_entry &entry = *iterator;
    std::error_code status_error;
    const bool is_symlink =
        std::filesystem::is_symlink(entry.symlink_status(status_error));
    if (status_error) {
      return memix::cli_detail::unexpected_error(
          std::format("failed to inspect `{}`: {}", entry.path().string(),
                      status_error.message()));
    }
    if (!is_symlink) {
      continue;
    }
    const memix::cli_detail::Result<std::string> blob_id =
        blob_id_for_tree_link(entry.path());
    if (!blob_id) {
      return std::unexpected(blob_id.error());
    }
    entries.push_back(ViewTreeEntry{
        .path = entry.path().lexically_relative(artifact.tree_path),
        .blob_id = *blob_id,
    });
  }
  if (iterator_error) {
    return memix::cli_detail::unexpected_error(
        std::format("failed to list directory `{}`: {}",
                    artifact.tree_path.string(), iterator_error.message()));
  }

  std::ranges::sort(entries, {}, &ViewTreeEntry::path);
  return entries;
}

void print_view_tree_table(const std::vector<ViewTreeEntry> &entries) {
  memix::cli_detail::print_path_id_table(
      entries,
      memix::cli_detail::PathIdTableLayout{
          .minimum_path_width = kTreePathColumnWidth,
          .id_column_width = kTreeIdColumnWidth,
      },
      [](const ViewTreeEntry &entry) { return entry.path.generic_string(); },
      [](const ViewTreeEntry &entry) -> std::string_view {
        return entry.blob_id;
      });
}

memix::cli_detail::Result<memix::ViewService>
make_view_service(const ViewCliOptions &options) {
  const memix::cli_detail::Result<std::filesystem::path> root =
      memix::cli_detail::resolve_store_root(options.common.store_root);
  if (!root) {
    return std::unexpected(root.error());
  }
  return memix::ViewService(memix::Store(*root));
}

memix::cli_detail::Result<memix::ViewExpression>
build_expression(const ViewCliOptions &options) {
  if (!options.generation.empty() && !options.ref.empty()) {
    return memix::cli_detail::unexpected_error(
        "build accepts only one of --generation or --ref");
  }

  memix::ViewExpression expression;
  const memix::cli_detail::Result<memix::MemoryObjectType> source_type =
      optional_source_type(options.type_name);
  if (!source_type) {
    return std::unexpected(source_type.error());
  }
  expression.source_type = *source_type;
  expression.name = options.name;
  if (!options.generation.empty()) {
    expression.source_generation = options.generation;
  } else if (!options.ref.empty()) {
    expression.source_ref = options.ref;
  }

  expression.selectors.reserve(options.include_paths.size()
                               + options.include_optional_paths.size());
  const auto required_selectors =
      options.include_paths
      | std::views::transform([](const std::filesystem::path &path) {
          return memix::ViewSelector{
              .source_path = path,
              .scope_label = std::nullopt,
              .required = true,
          };
        });
  std::ranges::copy(required_selectors,
                    std::back_inserter(expression.selectors));

  const auto optional_selectors =
      options.include_optional_paths
      | std::views::transform([](const std::filesystem::path &path) {
          return memix::ViewSelector{
              .source_path = path,
              .scope_label = std::nullopt,
              .required = false,
          };
        });
  std::ranges::copy(optional_selectors,
                    std::back_inserter(expression.selectors));
  return expression;
}

memix::cli_detail::VoidResult run_build(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> views =
      make_view_service(options);
  if (!views) {
    return std::unexpected(views.error());
  }
  const memix::cli_detail::Result<memix::ViewExpression> expression =
      build_expression(options);
  if (!expression) {
    return std::unexpected(expression.error());
  }
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views->build_file_tree(*expression);
  if (!artifact) {
    return memix::cli_detail::unexpected_error(artifact.error());
  }
  if (options.build_json) {
    return print_artifact_json(*artifact);
  }
  print_artifact_summary(*artifact);
  return {};
}

memix::cli_detail::VoidResult run_show(const ViewCliOptions &options) {
  if (options.show_derivation_id.empty()) {
    return memix::cli_detail::unexpected_error("show expects a derivation id");
  }
  const memix::cli_detail::Result<memix::ViewService> views =
      make_view_service(options);
  if (!views) {
    return std::unexpected(views.error());
  }
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views->resolve_file_tree(options.show_derivation_id);
  if (!artifact) {
    return memix::cli_detail::unexpected_error(artifact.error());
  }
  print_artifact_summary(*artifact);
  return {};
}

memix::cli_detail::VoidResult run_cd(const ViewCliOptions &options) {
  if (options.cd_derivation_id.empty()) {
    return memix::cli_detail::unexpected_error("cd expects a derivation id");
  }
  const memix::cli_detail::Result<memix::ViewService> views =
      make_view_service(options);
  if (!views) {
    return std::unexpected(views.error());
  }
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views->resolve_file_tree(options.cd_derivation_id);
  if (!artifact) {
    return memix::cli_detail::unexpected_error(artifact.error());
  }
  std::println("{}", artifact->tree_path.string());
  return {};
}

memix::cli_detail::VoidResult run_tree(const ViewCliOptions &options) {
  if (options.tree_derivation_id.empty()) {
    return memix::cli_detail::unexpected_error("tree expects a derivation id");
  }
  const memix::cli_detail::Result<memix::ViewService> views =
      make_view_service(options);
  if (!views) {
    return std::unexpected(views.error());
  }
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views->resolve_file_tree(options.tree_derivation_id);
  if (!artifact) {
    return memix::cli_detail::unexpected_error(artifact.error());
  }
  const memix::cli_detail::Result<std::vector<ViewTreeEntry>> entries =
      collect_view_tree_entries(*artifact);
  if (!entries) {
    return std::unexpected(entries.error());
  }
  print_view_tree_table(*entries);
  return {};
}

memix::cli_detail::VoidResult run_list(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> views =
      make_view_service(options);
  if (!views) {
    return std::unexpected(views.error());
  }
  const std::expected<std::vector<memix::ViewArtifact>, memix::StoreError>
      artifacts = views->list_file_trees();
  if (!artifacts) {
    return memix::cli_detail::unexpected_error(artifacts.error());
  }
  if (options.list_json) {
    return print_artifacts_json(*artifacts);
  }
  print_artifact_table(*artifacts);
  return {};
}

void add_build_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--type")
                 .argName("TEXT")
                 .help(std::format("Source context tree type: {}. Defaults "
                                   "to agents.",
                                   supported_types_message()))
                 .handler([&options](std::string_view value) {
                   options.type_name = value;
                 }));
  parser.add(Argum::Option("--include")
                 .argName("TEXT")
                 .help("Required logical source path(s) to include. Defaults "
                       "to all paths in the source generation.")
                 .handler([&options](std::string_view value) {
                   options.include_paths.emplace_back(value);
                 }));
  parser.add(Argum::Option("--include-optional")
                 .argName("TEXT")
                 .help("Optional logical source path(s) to include.")
                 .handler([&options](std::string_view value) {
                   options.include_optional_paths.emplace_back(value);
                 }));
  parser.add(Argum::Option("--generation")
                 .argName("TEXT")
                 .help("Source generation id. Mutually exclusive with --ref.")
                 .handler([&options](std::string_view value) {
                   options.generation = value;
                 }));
  parser.add(Argum::Option("--ref")
                 .argName("TEXT")
                 .help("Source ref to resolve. Defaults to TYPE/latest.")
                 .handler([&options](std::string_view value) {
                   options.ref = value;
                 }));
  parser.add(Argum::Option("--name")
                 .argName("TEXT")
                 .help("View name.")
                 .handler([&options](std::string_view value) {
                   options.name = value;
                 }));
  parser.add(Argum::Option("--json")
                 .help("Emit the manifest JSON instead of a summary.")
                 .handler([&options]() { options.build_json = true; }));
}

void add_show_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("derivation-id")
                 .help("View derivation id.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.show_derivation_id = value;
                 }));
}

void add_cd_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("derivation-id")
                 .help("View derivation id or unique prefix.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.cd_derivation_id = value;
                 }));
}

void add_tree_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("derivation-id")
                 .help("View derivation id or unique prefix.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.tree_derivation_id = value;
                 }));
}

void add_list_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--json")
                 .help("Emit manifest JSON as an array instead of a table.")
                 .handler([&options]() { options.list_json = true; }));
}

void add_commands(Argum::Parser &parser, ViewCliOptions &options) {
  parser.addSubCommand(
      Argum::Positional("subcommand")
          .occurs(Argum::neverOrOnce)
          .help("Subcommand: build, show, cd, tree, or list.")
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
            if (value == "cd") {
              options.command = ViewCommand::Cd;
              add_cd_command(parser, options);
              return {};
            }
            if (value == "tree") {
              options.command = ViewCommand::Tree;
              add_tree_command(parser, options);
              return {};
            }
            if (value == "list") {
              options.command = ViewCommand::List;
              add_list_command(parser, options);
              return {};
            }
            return memix::cli_detail::argum_validation_error(std::format(
                "unsupported command `{}`; expected build|show|cd|tree|list",
                value));
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

void print_cd_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_cd_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view cd"));
}

void print_tree_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_tree_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view tree"));
}

void print_list_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_list_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view list"));
}

void print_selected_help(ViewCommand command) {
  switch (command) {
  case ViewCommand::Build:
    print_build_help();
    return;
  case ViewCommand::Show:
    print_show_help();
    return;
  case ViewCommand::Cd:
    print_cd_help();
    return;
  case ViewCommand::Tree:
    print_tree_help();
    return;
  case ViewCommand::List:
    print_list_help();
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
  case ViewCommand::Cd:
    return run_cd(options);
  case ViewCommand::Tree:
    return run_tree(options);
  case ViewCommand::List:
    return run_list(options);
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
