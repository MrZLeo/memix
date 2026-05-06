#include "memix/store.hpp"
#include "memix/version.hpp"
#include "memix/view.hpp"

#include <algorithm>
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
#include <system_error>
#include <vector>

#include <CLI/CLI.hpp>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

constexpr std::size_t kShortIdLength = 12;
constexpr int kDerivationColumnWidth = 14;
constexpr int kExpressionColumnWidth = 14;
constexpr int kGenerationColumnWidth = 14;
constexpr int kInputsColumnWidth = 6;
constexpr int kTreePathColumnWidth = 24;
constexpr int kTreeIdColumnWidth = 14;

struct ViewTreeEntry {
  std::filesystem::path path;
  std::string blob_id;
};

std::shared_ptr<spdlog::logger> make_logger() {
  auto logger = spdlog::stderr_color_mt("memix-view");
  logger->set_pattern("[%T] [%n] [%^%l%$] %v");
  logger->flush_on(spdlog::level::info);
  return logger;
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

std::string short_id(std::string_view value) {
  return std::string(value.substr(0, std::min(value.size(), kShortIdLength)));
}

void print_artifact_table(const std::vector<memix::ViewArtifact> &artifacts) {
  std::println("{:<{}}{:<{}}{:<{}}{:>{}}", "DERIVATION", kDerivationColumnWidth,
               "EXPRESSION", kExpressionColumnWidth, "GENERATION",
               kGenerationColumnWidth, "INPUTS", kInputsColumnWidth);
  for (const memix::ViewArtifact &artifact : artifacts) {
    std::println("{:<{}}{:<{}}{:<{}}{:>{}}", short_id(artifact.derivation_id),
                 kDerivationColumnWidth, short_id(artifact.expression_id),
                 kExpressionColumnWidth, short_id(artifact.agent_generation),
                 kGenerationColumnWidth, artifact.input_blob_ids.size(),
                 kInputsColumnWidth);
  }
}

void print_artifact_summary(const memix::ViewArtifact &artifact) {
  std::println("derivation_id: {}", artifact.derivation_id);
  std::println("expression_id: {}", artifact.expression_id);
  std::println("agent_generation: {}", artifact.agent_generation);
  std::println("input_count: {}", artifact.input_blob_ids.size());
  std::println("manifest: {}", artifact.manifest_path.string());
  std::println("tree: {}", artifact.tree_path.string());
  std::println("inserted: {}", artifact.inserted ? "true" : "false");
}

void print_artifact_json(const memix::ViewArtifact &artifact) {
  std::println("{}", read_file_contents(artifact.manifest_path));
}

void print_artifacts_json(const std::vector<memix::ViewArtifact> &artifacts) {
  std::string output = "[";
  bool needs_comma = false;
  for (const memix::ViewArtifact &artifact : artifacts) {
    if (needs_comma) {
      output += ",";
    }
    output += read_file_contents(artifact.manifest_path);
    needs_comma = true;
  }
  output += "]";
  std::println("{}", output);
}

std::string blob_id_for_tree_link(const std::filesystem::path &link_path) {
  std::error_code error;
  const std::filesystem::path target =
      std::filesystem::read_symlink(link_path, error);
  if (error) {
    throw std::runtime_error(std::format("failed to read symlink `{}`: {}",
                                         link_path.string(), error.message()));
  }
  if (target.filename() != "object") {
    throw std::runtime_error(
        std::format("view tree symlink target is not a store object `{}`",
                    link_path.string()));
  }
  return target.parent_path().filename().string();
}

std::vector<ViewTreeEntry>
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
      throw std::runtime_error(std::format("failed to inspect `{}`: {}",
                                           entry.path().string(),
                                           status_error.message()));
    }
    if (!is_symlink) {
      continue;
    }
    entries.push_back(ViewTreeEntry{
        .path = entry.path().lexically_relative(artifact.tree_path),
        .blob_id = blob_id_for_tree_link(entry.path()),
    });
  }
  if (iterator_error) {
    throw std::runtime_error(std::format("failed to list directory `{}`: {}",
                                         artifact.tree_path.string(),
                                         iterator_error.message()));
  }

  std::ranges::sort(entries, {}, &ViewTreeEntry::path);
  return entries;
}

void print_view_tree_table(const std::vector<ViewTreeEntry> &entries) {
  std::size_t path_width = kTreePathColumnWidth;
  for (const ViewTreeEntry &entry : entries) {
    path_width = std::max(path_width, entry.path.generic_string().size());
  }

  std::println("{:<{}}{:<{}}", "PATH", path_width + 2, "ID",
               kTreeIdColumnWidth);
  for (const ViewTreeEntry &entry : entries) {
    std::println("{:<{}}{:<{}}", entry.path.generic_string(), path_width + 2,
                 short_id(entry.blob_id), kTreeIdColumnWidth);
  }
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

struct ViewCliOptions {
  std::filesystem::path store_root;
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

memix::ViewService make_view_service(const ViewCliOptions &options) {
  return memix::ViewService(
      memix::Store(resolve_store_root(options.store_root)));
}

memix::ViewExpression build_expression(const ViewCliOptions &options) {
  if (!options.generation.empty() && !options.ref.empty()) {
    throw std::runtime_error("build accepts only one of --generation or --ref");
  }

  memix::ViewExpression expression;
  expression.name = options.name;
  if (!options.generation.empty()) {
    expression.agent_generation = options.generation;
  } else if (!options.ref.empty()) {
    expression.agent_ref = options.ref;
  }

  for (const std::filesystem::path &path : options.include_paths) {
    expression.selectors.push_back(memix::ViewSelector{
        .agent_path = path,
        .scope_label = std::nullopt,
        .required = true,
    });
  }
  for (const std::filesystem::path &path : options.include_optional_paths) {
    expression.selectors.push_back(memix::ViewSelector{
        .agent_path = path,
        .scope_label = std::nullopt,
        .required = false,
    });
  }
  return expression;
}

void run_build(const ViewCliOptions &options) {
  const memix::ViewService views(make_view_service(options));
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views.build_file_tree(build_expression(options));
  if (!artifact) {
    throw std::runtime_error(artifact.error().message);
  }
  if (options.build_json) {
    print_artifact_json(*artifact);
    return;
  }
  print_artifact_summary(*artifact);
}

void run_show(const ViewCliOptions &options) {
  const memix::ViewService views(make_view_service(options));
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views.resolve_file_tree(options.show_derivation_id);
  if (!artifact) {
    throw std::runtime_error(artifact.error().message);
  }
  print_artifact_summary(*artifact);
}

void run_cd(const ViewCliOptions &options) {
  const memix::ViewService views(make_view_service(options));
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views.resolve_file_tree(options.cd_derivation_id);
  if (!artifact) {
    throw std::runtime_error(artifact.error().message);
  }
  std::println("{}", artifact->tree_path.string());
}

void run_tree(const ViewCliOptions &options) {
  const memix::ViewService views(make_view_service(options));
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views.resolve_file_tree(options.tree_derivation_id);
  if (!artifact) {
    throw std::runtime_error(artifact.error().message);
  }
  print_view_tree_table(collect_view_tree_entries(*artifact));
}

void run_list(const ViewCliOptions &options) {
  const memix::ViewService views(make_view_service(options));
  const std::expected<std::vector<memix::ViewArtifact>, memix::StoreError>
      artifacts = views.list_file_trees();
  if (!artifacts) {
    throw std::runtime_error(artifacts.error().message);
  }
  if (options.list_json) {
    print_artifacts_json(*artifacts);
    return;
  }
  print_artifact_table(*artifacts);
}

void add_global_options(CLI::App &app, ViewCliOptions &options) {
  app.set_version_flag("--version", std::format("memix {}", MEMIX_VERSION));
  app.add_option("--store-root", options.store_root,
                 "Store root. Defaults to MEMIX_STORE_HOME, "
                 "XDG_DATA_HOME/memix, or HOME/.memix.");
  app.require_subcommand(1);
}

void add_build_command(CLI::App &app, ViewCliOptions &options) {
  auto *build_cmd =
      app.add_subcommand("build", "Build a read-only symlink file-tree view.");
  build_cmd->add_option("--include", options.include_paths,
                        "Required logical agent path(s) to include. Defaults "
                        "to all paths in the agent generation.");
  build_cmd->add_option("--include-optional", options.include_optional_paths,
                        "Optional logical agent path(s) to include.");
  build_cmd->add_option("--generation", options.generation,
                        "Agent generation id. Mutually exclusive with --ref.");
  build_cmd->add_option("--ref", options.ref,
                        "Agent ref to resolve. Defaults to agents/latest.");
  build_cmd->add_option("--name", options.name, "View name.");
  build_cmd->add_flag("--json", options.build_json,
                      "Emit the manifest JSON instead of a summary.");
  build_cmd->callback([&options]() { run_build(options); });
}

void add_show_command(CLI::App &app, ViewCliOptions &options) {
  auto *show_cmd =
      app.add_subcommand("show", "Show a materialized file-tree view.");
  show_cmd
      ->add_option("derivation-id", options.show_derivation_id,
                   "View derivation id.")
      ->required();
  show_cmd->callback([&options]() { run_show(options); });
}

void add_cd_command(CLI::App &app, ViewCliOptions &options) {
  auto *cd_cmd = app.add_subcommand(
      "cd", "Print a file-tree path suitable for shell cd integration.");
  cd_cmd
      ->add_option("derivation-id", options.cd_derivation_id,
                   "View derivation id or unique prefix.")
      ->required();
  cd_cmd->callback([&options]() { run_cd(options); });
}

void add_tree_command(CLI::App &app, ViewCliOptions &options) {
  auto *tree_cmd =
      app.add_subcommand("tree", "List files in a materialized view tree.");
  tree_cmd
      ->add_option("derivation-id", options.tree_derivation_id,
                   "View derivation id or unique prefix.")
      ->required();
  tree_cmd->callback([&options]() { run_tree(options); });
}

void add_list_command(CLI::App &app, ViewCliOptions &options) {
  auto *list_cmd =
      app.add_subcommand("list", "List materialized file-tree views.");
  list_cmd->add_flag("--json", options.list_json,
                     "Emit manifest JSON as an array instead of a table.");
  list_cmd->callback([&options]() { run_list(options); });
}

int run_cli(int argc, char **argv) {
  CLI::App app{"memix view utilities."};
  ViewCliOptions options;

  add_global_options(app, options);
  add_build_command(app, options);
  add_show_command(app, options);
  add_cd_command(app, options);
  add_tree_command(app, options);
  add_list_command(app, options);

  CLI11_PARSE(app, argc, argv);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  std::shared_ptr<spdlog::logger> logger;
  try {
    logger = make_logger();
    return run_cli(argc, argv);
  } catch (const std::exception &error) {
    log_error(logger, error.what());
    return 1;
  } catch (...) {
    log_error(logger, "unknown error");
    return 1;
  }
}
