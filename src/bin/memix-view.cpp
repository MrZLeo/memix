#include "memix/view.hpp"

#include "cli_common.hpp"
#include "detail/error.hpp"
#include "detail/json.hpp"

#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"

#include <charconv>
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
#include <system_error>
#include <utility>
#include <vector>

#include <argum/data.h>
#include <argum/expected.h>
#include <argum/parser.h>
#include <simdjson.h>
#include <spdlog/logger.h>

namespace fs = memix::fs;

namespace {

enum class ViewCommand : std::uint8_t {
  None,
  List,
  Tree,
  Cat,
  Build,
  Show,
  Activate,
  Generations,
  Rollback,
  Deactivate,
  Status,
};

struct ViewCliOptions {
  std::optional<std::size_t> tree_depth;
  std::vector<std::string> sources;
  std::vector<std::string> view_sources;
  std::string tree_environment_id;
  fs::path tree_path;
  std::string cat_environment_id;
  fs::path cat_path;
  std::string target_name = "file_tree";
  std::string show_view_environment_id;
  std::string activate_environment_id;
  std::string into_path = ".memory";
  std::string profile_name;
  std::string mode_name = "link";
  memix::cli_detail::CommonOptions common;
  std::optional<std::string> rollback_to;
  ViewCommand command = ViewCommand::None;
  bool list_json = false;
  bool tree_json = false;
  bool build_json = false;
  bool show_json = false;
  bool force = false;
  bool wipe_profile = false;
};

struct ViewEnvironmentInfoJson {
  std::string id;
  std::string derivation;
  std::size_t file_count = 0;
  std::uintmax_t size_bytes = 0;
  std::size_t upstream_environment_count = 0;
};

struct ViewCatalogEntryJson {
  std::string path;
  std::string kind;
  std::size_t file_count = 0;
  std::uintmax_t size_bytes = 0;
  std::string blob;
};

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ViewEnvironmentInfoJson &environment) {
  builder.start_object();
  builder.template append_key_value<"id">(environment.id);
  builder.append_comma();
  builder.template append_key_value<"derivation">(environment.derivation);
  builder.append_comma();
  builder.template append_key_value<"file_count">(environment.file_count);
  builder.append_comma();
  builder.template append_key_value<"size_bytes">(environment.size_bytes);
  builder.append_comma();
  builder.template append_key_value<"upstream_environment_count">(
      environment.upstream_environment_count);
  builder.end_object();
}

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                const ViewCatalogEntryJson &entry) {
  builder.start_object();
  builder.template append_key_value<"path">(entry.path);
  builder.append_comma();
  builder.template append_key_value<"kind">(entry.kind);
  builder.append_comma();
  builder.template append_key_value<"file_count">(entry.file_count);
  builder.append_comma();
  builder.template append_key_value<"size_bytes">(entry.size_bytes);
  builder.append_comma();
  builder.template append_key_value<"blob">(entry.blob);
  builder.end_object();
}

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

template <typename Values>
memix::cli_detail::VoidResult print_json(const Values &values) {
  simdjson::builder::string_builder builder;
  builder.append(values);
  const memix::detail::CommonResult<std::string> json =
      memix::detail::json_from_builder(builder);
  if (!json) {
    return memix::cli_detail::unexpected_error(json.error().message);
  }
  std::println("{}", *json);
  return {};
}

memix::cli_detail::VoidResult run_list(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<std::vector<memix::ViewEnvironmentInfo>>
      environments = service->list_environments();
  if (!environments) {
    return memix::cli_detail::unexpected_error(environments.error());
  }
  if (options.list_json) {
    const std::vector<ViewEnvironmentInfoJson> json =
        *environments
        | std::views::transform([](const memix::ViewEnvironmentInfo &info) {
            return ViewEnvironmentInfoJson{
                .id = info.id,
                .derivation = info.derivation_id,
                .file_count = info.file_count,
                .size_bytes = info.total_size_bytes,
                .upstream_environment_count = info.upstream_environment_count,
            };
          })
        | std::ranges::to<std::vector>();
    return print_json(json);
  }
  std::println("{:<66}{:<66}{:>8}{:>12}{:>10}", "ENVIRONMENT", "DERIVATION",
               "FILES", "SIZE_BYTES", "UPSTREAMS");
  for (const memix::ViewEnvironmentInfo &environment : *environments) {
    std::println("{:<66}{:<66}{:>8}{:>12}{:>10}", environment.id,
                 environment.derivation_id, environment.file_count,
                 environment.total_size_bytes,
                 environment.upstream_environment_count);
  }
  return {};
}

memix::cli_detail::VoidResult run_tree(const ViewCliOptions &options) {
  if (options.tree_environment_id.empty()) {
    return memix::cli_detail::unexpected_error(
        "tree expects an environment id");
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<std::vector<memix::ViewCatalogEntry>> entries =
      service->environment_tree(options.tree_environment_id,
                                memix::ViewCatalogOptions{
                                    .path = options.tree_path,
                                    .depth = options.tree_depth,
                                });
  if (!entries) {
    return memix::cli_detail::unexpected_error(entries.error());
  }
  if (options.tree_json) {
    const std::vector<ViewCatalogEntryJson> json =
        *entries
        | std::views::transform([](const memix::ViewCatalogEntry &entry) {
            return ViewCatalogEntryJson{
                .path = entry.path.generic_string(),
                .kind = entry.directory ? "directory" : "file",
                .file_count = entry.file_count,
                .size_bytes = entry.total_size_bytes,
                .blob = entry.blob_id,
            };
          })
        | std::ranges::to<std::vector>();
    return print_json(json);
  }
  std::println("{:<11}{:>8}{:>12}  {}", "KIND", "FILES", "SIZE_BYTES", "PATH");
  for (const memix::ViewCatalogEntry &entry : *entries) {
    std::println("{:<11}{:>8}{:>12}  {}",
                 entry.directory ? "directory" : "file", entry.file_count,
                 entry.total_size_bytes,
                 entry.path.empty() ? "." : entry.path.generic_string());
  }
  return {};
}

memix::cli_detail::VoidResult run_cat(const ViewCliOptions &options) {
  if (options.cat_environment_id.empty()) {
    return memix::cli_detail::unexpected_error("cat expects an environment id");
  }
  if (options.cat_path.empty()) {
    return memix::cli_detail::unexpected_error("cat expects a view path");
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ResolvedViewFile> file =
      service->read_environment_file(options.cat_environment_id,
                                     options.cat_path);
  if (!file) {
    return memix::cli_detail::unexpected_error(file.error());
  }
  const memix::cli_detail::Result<std::string> contents =
      memix::cli_detail::read_file_contents(file->blob.object_dir
                                            / file->blob.content_path);
  if (!contents) {
    return std::unexpected(contents.error());
  }
  std::print("{}", *contents);
  return {};
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
  memix::ViewResult<memix::AuthoredViewExpression> expression =
      service->expression_from_sources(options.sources, *target);
  if (!options.view_sources.empty()) {
    std::vector<memix::AuthoredViewInput> inputs;
    inputs.reserve(options.sources.size() + options.view_sources.size());
    for (const std::string &source : options.sources) {
      inputs.push_back(memix::AuthoredViewInput{
          .kind = memix::ViewInputKind::Context,
          .locator = source,
          .environment_id = {},
          .output_role = "file_tree",
          .mount = {},
      });
    }
    for (const std::string &view_source : options.view_sources) {
      const std::size_t separator = view_source.find('=');
      const std::string environment = view_source.substr(0, separator);
      if (environment.empty()) {
        return memix::cli_detail::unexpected_error(
            "--view expects ENVIRONMENT[=MOUNT]");
      }
      const fs::path mount = separator == std::string::npos
                               ? fs::path{}
                               : fs::path(view_source.substr(separator + 1));
      inputs.push_back(memix::AuthoredViewInput{
          .kind = memix::ViewInputKind::Environment,
          .locator = {},
          .environment_id = environment,
          .output_role = "file_tree",
          .mount = mount,
      });
    }
    expression = service->expression_from_inputs(std::move(inputs), *target);
  }
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

memix::cli_detail::Result<std::string>
resolve_cli_profile(const ViewCliOptions &options) {
  if (!options.profile_name.empty()) {
    const memix::ViewResult<std::string> valid =
        memix::validated_profile_name(options.profile_name);
    if (!valid) {
      return memix::cli_detail::unexpected_error(valid.error());
    }
    return *valid;
  }
  fs::path workspace = fs::path(options.into_path).parent_path();
  if (workspace.empty()) {
    workspace = ".";
  }
  const memix::FsResult<fs::path> canonical = fs::weakly_canonical(workspace);
  if (!canonical) {
    return memix::cli_detail::unexpected_error(canonical.error());
  }
  return memix::default_profile_name(*canonical);
}

memix::cli_detail::Result<memix::ActivationMode>
require_activation_mode(std::string_view mode_name) {
  const std::optional<memix::ActivationMode> mode =
      memix::parse_activation_mode(mode_name);
  if (!mode.has_value()) {
    return memix::cli_detail::unexpected_error(std::format(
        "unsupported activation mode `{}`; expected link|copy", mode_name));
  }
  return *mode;
}

void print_activation_summary(const memix::ViewActivation &activation) {
  std::println("profile: {}", activation.profile);
  std::println("generation: {}", activation.generation);
  std::println("environment: {}", activation.environment_id);
  std::println("derivation: {}", activation.derivation_id);
  std::println("workspace: {}", activation.workspace_path.string());
  std::println("switched: {}", activation.switched ? "true" : "false");
  std::println("materialized: {}", activation.materialized ? "true" : "false");
}

memix::cli_detail::VoidResult run_activate(const ViewCliOptions &options) {
  if (options.activate_environment_id.empty()) {
    return memix::cli_detail::unexpected_error(
        "activate expects an environment id");
  }
  const memix::cli_detail::Result<std::string> profile =
      resolve_cli_profile(options);
  if (!profile) {
    return std::unexpected(profile.error());
  }
  const memix::cli_detail::Result<memix::ActivationMode> mode =
      require_activation_mode(options.mode_name);
  if (!mode) {
    return std::unexpected(mode.error());
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ViewActivation> activation = service->activate(
      options.activate_environment_id, memix::ActivationOptions{
                                           .profile = *profile,
                                           .into = fs::path(options.into_path),
                                           .mode = *mode,
                                           .force = options.force,
                                       });
  if (!activation) {
    return memix::cli_detail::unexpected_error(activation.error());
  }
  print_activation_summary(*activation);
  return {};
}

memix::cli_detail::VoidResult run_generations(const ViewCliOptions &options) {
  const memix::cli_detail::Result<std::string> profile =
      resolve_cli_profile(options);
  if (!profile) {
    return std::unexpected(profile.error());
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<std::vector<memix::ViewGenerationInfo>> generations =
      service->generations(*profile);
  if (!generations) {
    return memix::cli_detail::unexpected_error(generations.error());
  }
  std::println("profile: {}", *profile);
  std::println("{:<12}{:<15}{:<6}{:<22}{:<8}", "GENERATION", "ENVIRONMENT",
               "MODE", "ACTIVATED_AT", "CURRENT");
  for (const memix::ViewGenerationInfo &generation : *generations) {
    std::println("{:<12}{:<15}{:<6}{:<22}{:<8}", generation.number,
                 memix::cli_detail::short_id(generation.environment_id),
                 memix::to_string(generation.mode), generation.activated_at,
                 generation.current ? "*" : "");
  }
  return {};
}

memix::cli_detail::VoidResult run_rollback(const ViewCliOptions &options) {
  const memix::cli_detail::Result<std::string> profile =
      resolve_cli_profile(options);
  if (!profile) {
    return std::unexpected(profile.error());
  }
  std::optional<std::uint64_t> target;
  if (options.rollback_to.has_value()) {
    const std::string_view value = *options.rollback_to;
    std::uint64_t number = 0;
    if (value.empty()) {
      return memix::cli_detail::unexpected_error(std::format(
          "rollback --to expects a generation number, got `{}`", value));
    }
    const char *const begin = value.data();
    // std::from_chars requires a raw pointer range into this contiguous view.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char *const end = begin + value.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, number);
    if (error != std::errc{} || parsed_end != end) {
      return memix::cli_detail::unexpected_error(std::format(
          "rollback --to expects a generation number, got `{}`", value));
    }
    target = number;
  }
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ViewActivation> activation =
      service->rollback(*profile, target);
  if (!activation) {
    return memix::cli_detail::unexpected_error(activation.error());
  }
  print_activation_summary(*activation);
  return {};
}

memix::cli_detail::VoidResult run_deactivate(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ViewDeactivation> deactivation =
      service->deactivate(fs::path(options.into_path), options.wipe_profile);
  if (!deactivation) {
    return memix::cli_detail::unexpected_error(deactivation.error());
  }
  std::println("deactivated: {}", deactivation->workspace_path.string());
  std::println("profile: {}", deactivation->profile);
  std::println("profile_wiped: {}",
               deactivation->wiped_profile ? "true" : "false");
  return {};
}

memix::cli_detail::VoidResult run_status(const ViewCliOptions &options) {
  const memix::cli_detail::Result<memix::ViewService> service =
      make_service(options);
  if (!service) {
    return std::unexpected(service.error());
  }
  const memix::ViewResult<memix::ViewActivationStatus> status =
      service->activation_status(fs::path(options.into_path));
  if (!status) {
    return memix::cli_detail::unexpected_error(status.error());
  }
  std::println("active: {}", status->active ? "true" : "false");
  if (!status->active) {
    return {};
  }
  std::println("mode: {}", memix::to_string(status->mode));
  std::println("profile: {}", status->profile);
  std::println("generation: {}", status->generation);
  std::println("environment: {}", status->environment_id);
  std::println("derivation: {}", status->derivation_id);
  std::println("stale: {}", status->stale ? "true" : "false");
  return {};
}

void add_list_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--json")
                 .help("Emit environment summaries as JSON.")
                 .handler([&options]() { options.list_json = true; }));
}

void add_tree_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("environment-id")
                 .help("View environment id or unique prefix.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.tree_environment_id = value;
                 }));
  parser.add(Argum::Option("--path")
                 .argName("PATH")
                 .help("Limit output to one path or subtree.")
                 .handler([&options](std::string_view value) {
                   options.tree_path = fs::path(value);
                 }));
  parser.add(
      Argum::Option("--depth")
          .argName("INTEGER")
          .help("Maximum depth relative to --path (0 shows the root only).")
          .handler([&options](std::string_view value) -> Argum::Expected<void> {
            std::size_t depth = 0;
            const char *const begin = value.data();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const char *const end = begin + value.size();
            const auto [parsed_end, error] = std::from_chars(begin, end, depth);
            if (value.empty() || error != std::errc{} || parsed_end != end) {
              return memix::cli_detail::argum_validation_error(std::format(
                  "--depth expects a non-negative integer, got `{}`", value));
            }
            options.tree_depth = depth;
            return {};
          }));
  parser.add(Argum::Option("--json")
                 .help("Emit structured tree entries as JSON.")
                 .handler([&options]() { options.tree_json = true; }));
}

void add_cat_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("environment-id")
                 .help("View environment id or unique prefix.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.cat_environment_id = value;
                 }));
  parser.add(Argum::Positional("path")
                 .help("File path inside the selected view.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.cat_path = fs::path(value);
                 }));
}

void add_build_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--source")
                 .argName("TEXT")
                 .help("View locator, for example rules/repo/AGENTS.md or "
                       "chat/task/2026-05-20/@latest.")
                 .handler([&options](std::string_view value) {
                   options.sources.emplace_back(value);
                 }));
  parser.add(
      Argum::Option("--view")
          .argName("ENVIRONMENT[=MOUNT]")
          .help("Compose an existing view environment, optionally below a "
                "relative mount path. Repeat for multiple views.")
          .handler([&options](std::string_view value) {
            options.view_sources.emplace_back(value);
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

void add_into_option(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--into")
                 .argName("PATH")
                 .help("Workspace path to attach the view to. Defaults to "
                       "./.memory.")
                 .handler([&options](std::string_view value) {
                   options.into_path = value;
                 }));
}

void add_profile_option(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Option("--profile")
                 .argName("NAME")
                 .help("Profile name [a-z0-9][a-z0-9._-]*. Defaults to "
                       "ws-<hash> derived from the workspace directory, "
                       "which isolates parallel workspaces.")
                 .handler([&options](std::string_view value) {
                   options.profile_name = value;
                 }));
}

void add_activate_command(Argum::Parser &parser, ViewCliOptions &options) {
  parser.add(Argum::Positional("environment-id")
                 .help("View environment id or unique prefix.")
                 .occurs(Argum::neverOrOnce)
                 .handler([&options](std::string_view value) {
                   options.activate_environment_id = value;
                 }));
  add_into_option(parser, options);
  add_profile_option(parser, options);
  parser.add(Argum::Option("--mode")
                 .argName("MODE")
                 .help("Materialization mode: link (symlink into the store) "
                       "or copy (real files). Defaults to link.")
                 .handler([&options](std::string_view value) {
                   options.mode_name = value;
                 }));
  parser.add(Argum::Option("--force")
                 .help("Replace an existing workspace symlink.")
                 .handler([&options]() { options.force = true; }));
}

void add_generations_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_into_option(parser, options);
  add_profile_option(parser, options);
}

void add_rollback_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_into_option(parser, options);
  add_profile_option(parser, options);
  parser.add(Argum::Option("--to")
                 .argName("N")
                 .help("Roll back to generation N. Defaults to the previous "
                       "generation.")
                 .handler([&options](std::string_view value) {
                   options.rollback_to = value;
                 }));
}

void add_deactivate_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_into_option(parser, options);
  parser.add(
      Argum::Option("--wipe-profile")
          .help("Also remove the profile and its generations from the "
                "store. Other workspaces sharing the profile will dangle.")
          .handler([&options]() { options.wipe_profile = true; }));
}

void add_status_command(Argum::Parser &parser, ViewCliOptions &options) {
  add_into_option(parser, options);
}

void add_commands(Argum::Parser &parser, ViewCliOptions &options) {
  parser.addSubCommand(
      Argum::Positional("subcommand")
          .occurs(Argum::neverOrOnce)
          .help("Subcommand: list, tree, cat, build, show, activate, "
                "generations, rollback, deactivate, or status.")
          .handler([&parser,
                    &options](std::string_view value) -> Argum::Expected<void> {
            if (value == "list") {
              options.command = ViewCommand::List;
              add_list_command(parser, options);
              return {};
            }
            if (value == "tree") {
              options.command = ViewCommand::Tree;
              add_tree_command(parser, options);
              return {};
            }
            if (value == "cat") {
              options.command = ViewCommand::Cat;
              add_cat_command(parser, options);
              return {};
            }
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
            if (value == "activate") {
              options.command = ViewCommand::Activate;
              add_activate_command(parser, options);
              return {};
            }
            if (value == "generations") {
              options.command = ViewCommand::Generations;
              add_generations_command(parser, options);
              return {};
            }
            if (value == "rollback") {
              options.command = ViewCommand::Rollback;
              add_rollback_command(parser, options);
              return {};
            }
            if (value == "deactivate") {
              options.command = ViewCommand::Deactivate;
              add_deactivate_command(parser, options);
              return {};
            }
            if (value == "status") {
              options.command = ViewCommand::Status;
              add_status_command(parser, options);
              return {};
            }
            return memix::cli_detail::argum_validation_error(std::format(
                "unsupported command `{}`; expected "
                "list|tree|cat|build|show|activate|generations|rollback|"
                "deactivate|status",
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

void print_list_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_list_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view list"));
}

void print_tree_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_tree_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view tree"));
}

void print_cat_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_cat_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view cat"));
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

void print_activate_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_activate_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view activate"));
}

void print_generations_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_generations_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view generations"));
}

void print_rollback_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_rollback_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view rollback"));
}

void print_deactivate_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_deactivate_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view deactivate"));
}

void print_status_help() {
  Argum::Parser parser;
  ViewCliOptions options;
  memix::cli_detail::add_common_options(parser, options.common);
  add_status_command(parser, options);
  std::print("{}", parser.formatHelp("memix-view status"));
}

void print_selected_help(ViewCommand command) {
  switch (command) {
  case ViewCommand::List:
    print_list_help();
    return;
  case ViewCommand::Tree:
    print_tree_help();
    return;
  case ViewCommand::Cat:
    print_cat_help();
    return;
  case ViewCommand::Build:
    print_build_help();
    return;
  case ViewCommand::Show:
    print_show_help();
    return;
  case ViewCommand::Activate:
    print_activate_help();
    return;
  case ViewCommand::Generations:
    print_generations_help();
    return;
  case ViewCommand::Rollback:
    print_rollback_help();
    return;
  case ViewCommand::Deactivate:
    print_deactivate_help();
    return;
  case ViewCommand::Status:
    print_status_help();
    return;
  case ViewCommand::None:
    print_root_help();
    return;
  }
}

memix::cli_detail::VoidResult dispatch(const ViewCliOptions &options) {
  switch (options.command) {
  case ViewCommand::List:
    return run_list(options);
  case ViewCommand::Tree:
    return run_tree(options);
  case ViewCommand::Cat:
    return run_cat(options);
  case ViewCommand::Build:
    return run_build(options);
  case ViewCommand::Show:
    return run_show(options);
  case ViewCommand::Activate:
    return run_activate(options);
  case ViewCommand::Generations:
    return run_generations(options);
  case ViewCommand::Rollback:
    return run_rollback(options);
  case ViewCommand::Deactivate:
    return run_deactivate(options);
  case ViewCommand::Status:
    return run_status(options);
  case ViewCommand::None:
    return memix::cli_detail::unexpected_error("a subcommand is required");
  }
  return memix::cli_detail::unexpected_error("unknown command");
}

int
run_cli(int argc, char **argv, const std::shared_ptr<spdlog::logger> &logger) {
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
