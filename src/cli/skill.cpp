#include "commands.hpp"

#include "cli_common.hpp"
#include "detail/json.hpp"
#include "detail/scope_exit.hpp"
#include "memix/fs.hpp"
#include "skill_assets.hpp"

#include <argum/data.h>
#include <argum/parser.h>
#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = memix::fs;
namespace cli = memix::cli_detail;
using cli::Result;
using cli::unexpected_error;
using cli::VoidResult;

constexpr std::string_view kSkillMarker =
    "<!-- Installed by \"memix skill setup\". Remove it with \"memix skill "
    "remove\". -->";
constexpr std::string_view kOpenaiMarker = "# Managed by memix skill setup.";
constexpr auto kAgents = std::to_array<std::string_view>(
    {"claude", "codex", "opencode", "pi", "dsh"});

struct Options {
  cli::CommonOptions common;
  std::string command;
  std::vector<std::string> agents;
  bool project = false;
  bool dry_run = false;
  bool force = false;
  bool json = false;
};

struct Harness {
  std::string id;
  bool detected = false;
  std::vector<fs::path> paths;
};

struct Target {
  fs::path path;
  std::vector<std::string> harnesses;
};

struct Inspection {
  std::string state;
  std::string reason;
};

struct Step {
  Target target;
  std::string change;
  std::string reason;
  bool failed = false;
};

std::string environment(const char *name) {
  if (*name == '\0') {
    return {};
  }
  // CLI dispatch is single-threaded; no environment mutations occur here.
  const char *value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
  return value == nullptr ? std::string{} : std::string(value);
}

Result<fs::file_type> entry_type(const fs::path &path) {
  const auto status = fs::symlink_status(path);
  if (status) {
    return status->type();
  }
  if (status.error().code == std::errc::no_such_file_or_directory) {
    return fs::file_type::not_found;
  }
  return unexpected_error(status.error());
}

bool directory_exists(const fs::path &path) {
  if (path.empty()) {
    return false;
  }
  const auto result = fs::is_directory(path);
  return result && *result;
}

bool executable_found(std::string_view name) {
#ifdef _WIN32
  constexpr char separator = ';';
  constexpr auto suffixes =
      std::to_array<std::string_view>({".exe", ".cmd", ".bat"});
#else
  constexpr char separator = ':';
  constexpr auto suffixes = std::to_array<std::string_view>({""});
#endif
  const auto path = environment("PATH");
  for (const auto part : path | std::views::split(separator)) {
    const std::string directory(part.begin(), part.end());
    if (directory.empty()) {
      continue;
    }
    for (const auto suffix : suffixes) {
      const auto candidate =
          fs::path(directory) / std::format("{}{}", name, suffix);
      const auto status = fs::status(candidate);
      if (!status || status->type() != fs::file_type::regular) {
        continue;
      }
#ifndef _WIN32
      constexpr auto executable = fs::perms::owner_exec
                                | fs::perms::group_exec
                                | fs::perms::others_exec;
      if ((status->permissions() & executable) == fs::perms::none) {
        continue;
      }
#endif
      return true;
    }
  }
  return false;
}

Result<fs::path> home_path(bool required) {
#ifdef _WIN32
  auto home = environment("USERPROFILE");
  if (home.empty()) {
    home = environment("HOME");
  }
#else
  const auto home = environment("HOME");
#endif
  if (home.empty() && !required) {
    return fs::path{};
  }
  if (home.empty() || !fs::path(home).is_absolute()) {
    return unexpected_error("cannot determine an absolute home directory; set "
                            "HOME (or USERPROFILE on Windows)");
  }
  return fs::path(home).lexically_normal();
}

Result<fs::path>
configured_path(const char *variable, const fs::path &fallback) {
  const auto value = environment(variable);
  if (value.empty()) {
    return fallback;
  }
  const fs::path path(value);
  if (!path.is_absolute()) {
    return unexpected_error(
        std::format("{} must be an absolute path", variable));
  }
  return path.lexically_normal();
}

Result<fs::path> project_root() {
  std::error_code error;
  auto path = std::filesystem::current_path(error);
  if (error) {
    return unexpected_error(
        std::format("cannot read current directory: {}", error.message()));
  }
  while (true) {
    for (const auto *marker : {".git", ".jj"}) {
      const auto type = entry_type(path / marker);
      if (!type) {
        return std::unexpected(type.error());
      }
      if (*type != fs::file_type::not_found) {
        return path;
      }
    }
    const auto parent = path.parent_path();
    if (parent == path) {
      return unexpected_error("--project requires a Git or Jujutsu repository");
    }
    path = parent;
  }
}

std::vector<fs::path>
skill_paths(const fs::path &primary, const fs::path &native) {
  std::vector<fs::path> paths{primary / "memix-cli"};
  if (primary == native / "skills") {
    return paths;
  }
  const auto legacy = native / "skills/memix-cli";
  const auto type = entry_type(legacy);
  if (!type || *type != fs::file_type::not_found) {
    // Reconcile existing native copies instead of creating a shadowed shared
    // installation. Unknown legacy files still need --force.
    const auto primary_type = entry_type(paths.front());
    if (primary_type && *primary_type == fs::file_type::not_found) {
      paths.clear();
    }
    paths.push_back(legacy);
  }
  return paths;
}

Result<std::vector<Harness>> harnesses(const Options &options) {
  const auto home = home_path(!options.project);
  if (!home) {
    return std::unexpected(home.error());
  }
  const auto root = options.project ? project_root() : home;
  if (!root) {
    return std::unexpected(root.error());
  }
  const auto config_home =
      configured_path("XDG_CONFIG_HOME", *home / ".config");
  if (!config_home) {
    return std::unexpected(config_home.error());
  }
  struct Definition {
    std::string_view id;
    const char *variable;
    fs::path default_home;
    fs::path local;
    bool shared;
  };
  const auto definitions = std::to_array<Definition>({
      {
          .id = "claude",
          .variable = "CLAUDE_CONFIG_DIR",
          .default_home = *home / ".claude",
          .local = ".claude",
          .shared = false,
      },
      {
          .id = "codex",
          .variable = "CODEX_HOME",
          .default_home = *home / ".codex",
          .local = ".codex",
          .shared = true,
      },
      {
          .id = "opencode",
          .variable = "",
          .default_home = *config_home / "opencode",
          .local = ".opencode",
          .shared = true,
      },
      {
          .id = "pi",
          .variable = "PI_CODING_AGENT_DIR",
          .default_home = *home / ".pi/agent",
          .local = ".pi",
          .shared = true,
      },
      {
          .id = "dsh",
          .variable = "DSH_HOME",
          .default_home = *home / ".dsh",
          .local = ".dsh",
          .shared = false,
      },
  });
  std::vector<Harness> result;
  for (const auto &definition : definitions) {
    const auto config =
        configured_path(definition.variable, definition.default_home);
    if (!config) {
      return std::unexpected(config.error());
    }
    const auto native = options.project ? *root / definition.local : *config;
    const bool shared =
        definition.shared
        && (options.project || environment(definition.variable).empty());
    const auto primary = shared ? *root / ".agents/skills" : native / "skills";
    Harness harness{
        .id = std::string(definition.id),
        .detected = (!home->empty() && directory_exists(*config))
                 || (options.project && directory_exists(native))
                 || executable_found(definition.id),
        .paths = skill_paths(primary, native),
    };
    result.push_back(std::move(harness));
  }
  return result;
}

Result<std::string> read_skill_file(const fs::path &path) {
  const auto size = fs::file_size(path);
  if (!size) {
    return unexpected_error(size.error());
  }
  constexpr auto maximum = 1024U * 1024U;
  if (*size > maximum) {
    return unexpected_error(
        std::format("skill file is too large: {}", path.string()));
  }
  const auto text = fs::read_text_file(path);
  if (!text) {
    return unexpected_error(text.error());
  }
  return *text;
}

Inspection inspect(const fs::path &path) {
  const auto directory = entry_type(path);
  if (!directory) {
    return {.state = "unreadable", .reason = directory.error().message};
  }
  if (*directory == fs::file_type::not_found) {
    return {.state = "missing", .reason = {}};
  }
  if (*directory != fs::file_type::directory) {
    return {
        .state = "foreign",
        .reason =
            "skill path is not a real directory; symlinks are not followed",
    };
  }
  bool current = true;
  bool owned = false;
  const auto agents = entry_type(path / "agents");
  if (!agents) {
    return {.state = "unreadable", .reason = agents.error().message};
  }
  if (*agents != fs::file_type::not_found
      && *agents != fs::file_type::directory) {
    return {
        .state = "foreign",
        .reason = "agents path is not a real directory",
    };
  }
  struct File {
    std::string_view path;
    std::string_view marker;
    std::string_view expected;
  };
  const auto files = std::to_array<File>({
      {
          .path = "SKILL.md",
          .marker = kSkillMarker,
          .expected = cli::skill_markdown,
      },
      {
          .path = "agents/openai.yaml",
          .marker = kOpenaiMarker,
          .expected = cli::skill_openai,
      },
  });
  for (const auto &file : files) {
    const auto type = entry_type(path / file.path);
    if (!type) {
      return {.state = "unreadable", .reason = type.error().message};
    }
    if (*type == fs::file_type::not_found) {
      current = false;
      continue;
    }
    if (*type != fs::file_type::regular) {
      return {
          .state = "foreign",
          .reason = std::format("{} is not a regular file", file.path),
      };
    }
    const auto text = read_skill_file(path / file.path);
    if (!text) {
      return {.state = "unreadable", .reason = text.error().message};
    }
    if (!text->contains(file.marker)) {
      return {
          .state = "foreign",
          .reason = std::format("{} was not installed by memix", file.path),
      };
    }
    owned = true;
    current = current && *text == file.expected;
  }
  if (!owned) {
    return {
        .state = "foreign",
        .reason = "directory has no Memix-managed files",
    };
  }
  return {.state = current ? "current" : "outdated", .reason = {}};
}

VoidResult safe_layout(const fs::path &path) {
  for (const auto &directory : {path, path / "agents"}) {
    const auto type = entry_type(directory);
    if (!type) {
      return std::unexpected(type.error());
    }
    if (*type != fs::file_type::not_found
        && *type != fs::file_type::directory) {
      return unexpected_error(std::format(
          "refusing non-directory or symlink: {}", directory.string()));
    }
  }
  for (const auto *file : {"SKILL.md", "agents/openai.yaml"}) {
    const auto type = entry_type(path / file);
    if (!type) {
      return std::unexpected(type.error());
    }
    if (*type != fs::file_type::not_found && *type != fs::file_type::regular) {
      return unexpected_error(std::format(
          "refusing non-regular file or symlink: {}", (path / file).string()));
    }
  }
  return {};
}

VoidResult install(const fs::path &path) {
  const auto safe = safe_layout(path);
  if (!safe) {
    return safe;
  }
  const auto staged = fs::unique_temp_dir(
      path.parent_path(),
      {.prefix = "memix-skill", .failure_message = "cannot stage skill"});
  if (!staged) {
    return unexpected_error(staged.error());
  }
  const auto created = fs::create_directory(*staged);
  if (!created) {
    return unexpected_error(created.error());
  }
  if (!*created) {
    return unexpected_error("skill staging directory already exists");
  }
  const memix::scope_exit cleanup([&] {
    if (const auto result = fs::remove_all(*staged); !result) {
      std::println(stderr, "warning: {}", result.error().message);
    }
  });
  if (const auto result = fs::create_directory(*staged / "agents"); !result) {
    return unexpected_error(result.error());
  }
  if (const auto result =
          fs::write_text_file(*staged / "SKILL.md", cli::skill_markdown);
      !result) {
    return unexpected_error(result.error());
  }
  if (const auto result = fs::write_text_file(*staged / "agents/openai.yaml",
                                              cli::skill_openai);
      !result) {
    return unexpected_error(result.error());
  }
  const auto staged_state = inspect(*staged);
  if (staged_state.state != "current") {
    return unexpected_error("staged skill verification failed: "
                            + staged_state.reason);
  }
  const auto type = entry_type(path);
  if (!type) {
    return std::unexpected(type.error());
  }
  if (*type == fs::file_type::not_found) {
    const auto result = fs::rename(*staged, path);
    return result ? VoidResult{} : unexpected_error(result.error());
  }
  if (const auto result = fs::create_directories(path / "agents"); !result) {
    return unexpected_error(result.error());
  }
  // Only replace the two owned filenames; preserve all user-added files.
  for (const auto *file : {"agents/openai.yaml", "SKILL.md"}) {
    if (const auto result = fs::replace_file(*staged / file, path / file);
        !result) {
      return unexpected_error(result.error());
    }
  }
  return {};
}

VoidResult uninstall(const fs::path &path) {
  for (const auto *file : {"SKILL.md", "agents/openai.yaml"}) {
    if (const auto result = fs::remove(path / file); !result) {
      return unexpected_error(result.error());
    }
  }
  // Non-recursive removal keeps unrelated files and directories intact.
  for (const auto &directory : {path / "agents", path}) {
    const auto result = fs::remove(directory);
    if (!result && result.error().code != std::errc::directory_not_empty) {
      return unexpected_error(result.error());
    }
  }
  return {};
}

Step plan(const Target &target, const Options &options) {
  const auto state = inspect(target.path);
  Step step{
      .target = target,
      .change = "unchanged",
      .reason = state.reason,
      .failed = false,
  };
  if (state.state == "unreadable") {
    step.change = "failed";
    step.failed = true;
  } else if (options.command == "remove") {
    if (state.state == "foreign") {
      step.change = "skipped";
    } else if (state.state != "missing") {
      step.change = "removed";
    }
  } else if (state.state == "foreign" && !options.force) {
    step.change = "conflict";
    step.reason += "; review it and use --force to replace the two skill files";
    step.failed = true;
  } else if (state.state != "current") {
    const auto safe = safe_layout(target.path);
    if (!safe) {
      step.change = "failed";
      step.reason = safe.error().message;
      step.failed = true;
    } else {
      step.change = state.state == "missing" ? "created" : "updated";
    }
  }
  return step;
}

bool changes_files(const Step &step) {
  return step.change == "created"
      || step.change == "updated"
      || step.change == "removed";
}

Step apply(const Target &target, const Options &options) {
  auto step = plan(target, options);
  if (options.dry_run || !changes_files(step)) {
    return step;
  }
  const auto fail = [&step](std::string_view reason) {
    step.change = "failed";
    step.reason = reason;
    step.failed = true;
    return step;
  };
  const auto parent = fs::create_directories(target.path.parent_path());
  if (!parent) {
    return fail(parent.error().message);
  }
  const auto lock = target.path.parent_path() / ".memix-cli-skill.lock";
  const auto acquired = fs::create_directory(lock);
  if (!acquired) {
    return fail(acquired.error().message);
  }
  if (!*acquired) {
    return fail(std::format(
        "skill operation lock exists: {}; do not remove a live lock",
        lock.string()));
  }
  const memix::scope_exit unlock([&] {
    if (const auto result = fs::remove(lock); !result) {
      std::println(stderr, "warning: {}", result.error().message);
    }
  });
  step = plan(target, options);
  if (!changes_files(step)) {
    return step;
  }
  const auto result = options.command == "setup" ? install(target.path)
                                                 : uninstall(target.path);
  if (!result) {
    return fail(result.error().message);
  }
  if (options.command == "setup" && inspect(target.path).state != "current") {
    return fail(
        "installed skill verification failed; inspect status before retrying");
  }
  step.reason.clear();
  return step;
}

Result<std::vector<Target>>
targets(const std::vector<Harness> &agents, const Options &options) {
  std::vector<Target> result;
  for (const auto &agent : agents) {
    const bool selected = options.agents.empty()
                            ? (options.command != "setup" || agent.detected)
                            : std::ranges::contains(options.agents, agent.id);
    if (!selected) {
      continue;
    }
    for (const auto &path : agent.paths) {
      // Resolve the parent only: never hide a symlink at the skill itself.
      const auto parent = fs::weakly_canonical(path.parent_path());
      if (!parent) {
        return unexpected_error(parent.error());
      }
      const auto destination = *parent / path.filename();
      const auto found = std::ranges::find(result, destination, &Target::path);
      if (found == result.end()) {
        result.push_back({.path = destination, .harnesses = {agent.id}});
      } else {
        found->harnesses.push_back(agent.id);
      }
    }
  }
  return result;
}

void
add_options(Argum::Parser &parser, Options &options, std::string_view command) {
  cli::add_common_options(
      parser, options.common,
      "Accepted but unused; skill commands never open a memory Store.");
  if (command != "show") {
    parser.add(
        Argum::Option("--agent")
            .argName("AGENT")
            .occurs(Argum::zeroOrMoreTimes)
            .help("Select claude, codex, opencode, pi or dsh (repeatable).")
            .handler([&](std::string_view value) {
              options.agents.emplace_back(value);
            }));
    parser.add(Argum::Option("--project")
                   .help("Use the current repository instead of your account.")
                   .handler([&] { options.project = true; }));
    parser.add(Argum::Option("--json")
                   .help("Print a structured report as JSON.")
                   .handler([&] { options.json = true; }));
  }
  if (command.empty() || command == "setup" || command == "remove") {
    parser.add(Argum::Option("--dry-run")
                   .help("Report changes without writing anything.")
                   .handler([&] { options.dry_run = true; }));
  }
  if (command.empty() || command == "setup") {
    parser.add(Argum::Option("--force")
                   .help("Replace foreign regular skill files; never follow "
                         "skill symlinks.")
                   .handler([&] { options.force = true; }));
  }
}

void help(std::string_view command) {
  Argum::Parser parser;
  Options options;
  add_options(parser, options, command);
  if (command.empty()) {
    parser.add(Argum::Positional("command")
                   .occurs(Argum::neverOrOnce)
                   .help("Skill lifecycle command (see below)."));
  }
  std::print("{}",
             parser.formatHelp(command.empty()
                                   ? "memix skill"
                                   : std::format("memix skill {}", command)));
  if (command.empty()) {
    std::println("Commands:\n"
                 "  setup    Install or update the bundled skill\n"
                 "  status   Inspect installation state\n"
                 "  remove   Remove managed skill files, not memory\n"
                 "  show     Print the entry guide for agents\n\n"
                 "Examples:\n"
                 "  memix skill setup --agent pi --dry-run\n"
                 "  memix skill status --json\n"
                 "  memix skill show\n");
  } else if (command == "show") {
    std::println(
        "Prints the same Markdown as 'memix agent guide'.\n"
        "Follow its routing to load only the topics needed for the task.");
    return;
  } else if (command == "status") {
    std::println("States:\n"
                 "  current     Matches this binary\n"
                 "  outdated    Managed files need an update\n"
                 "  missing     Not installed\n"
                 "  foreign     Unmanaged files; left untouched\n"
                 "  unreadable  Could not inspect the installation\n");
  } else if (command == "setup") {
    std::println("Behavior:\n"
                 "  Detects agent config directories or executables unless "
                 "--agent is given.\n"
                 "  Updates managed copies in place; foreign files require "
                 "explicit --force.\n"
                 "  Use --dry-run to preview without writing files.\n");
  } else {
    std::println(
        "Behavior:\n"
        "  Checks known locations even when an agent is no longer installed.\n"
        "  Preserves foreign bundles and user-added files. Never deletes "
        "recursively.\n"
        "  Use --dry-run to preview without removing files.\n");
  }
  std::println("Notes:\n"
               "  Codex, OpenCode and Pi normally share .agents/skills.\n"
               "  Changes to a shared path affect every agent that reads it.\n"
               "  No hooks, agent settings, or memory Stores are changed.");
}

VoidResult validate(const Options &options) {
  for (const auto &agent : options.agents) {
    if (!std::ranges::contains(kAgents, agent)) {
      return unexpected_error(std::format(
          "unknown agent `{}`; expected claude|codex|opencode|pi|dsh", agent));
    }
  }
  if (options.force && options.command != "setup") {
    return unexpected_error("--force is only valid for skill setup");
  }
  if (options.dry_run
      && options.command != "setup"
      && options.command != "remove") {
    return unexpected_error("--dry-run is only valid for skill setup/remove");
  }
  if (options.command == "show"
      && (options.project || options.json || !options.agents.empty())) {
    return unexpected_error(
        "skill show prints Markdown and does not accept installation options");
  }
  return {};
}

std::string_view agent_label(std::string_view id) {
  constexpr auto names =
      std::to_array<std::pair<std::string_view, std::string_view>>({
          {"claude", "Claude Code"},
          {"codex", "Codex"},
          {"opencode", "OpenCode"},
          {"pi", "Pi"},
          {"dsh", "DSH"},
      });
  const auto *found = std::ranges::find_if(
      names, [id](const auto &name) { return name.first == id; });
  return found == names.end() ? id : found->second;
}

std::string agent_labels(const std::vector<std::string> &ids) {
  std::string result;
  for (const auto &id : ids) {
    if (!result.empty()) {
      result += ", ";
    }
    result += agent_label(id);
  }
  return result;
}

std::string display_path(const fs::path &path, const fs::path &home) {
  if (!home.empty()) {
    const auto relative =
        path.lexically_normal().lexically_relative(home.lexically_normal());
    if (!relative.empty() && *relative.begin() != "..") {
      return relative == "." ? "~"
                             : std::format("~/{}", relative.generic_string());
    }
  }
  return path.string();
}

void print_note(std::string_view message) {
  constexpr std::size_t width = 78;
  std::istringstream words{std::string(message)};
  std::string line = "    Note: ";
  std::string word;
  bool has_word = false;
  while (words >> word) {
    if (has_word && line.size() + 1 + word.size() > width) {
      std::println("{}", line);
      line = "          ";
      has_word = false;
    }
    if (has_word) {
      line += ' ';
    }
    line += word;
    has_word = true;
  }
  if (has_word) {
    std::println("{}", line);
  }
}

void print_details(const fs::path &path, std::string_view reason,
                   const fs::path &home) {
  std::println("    Path: {}", display_path(path, home));
  print_note(reason);
  std::println("");
}

std::string_view action_label(std::string_view change, bool preview) {
  if (preview) {
    if (change == "created") {
      return "create";
    }
    if (change == "updated") {
      return "update";
    }
    if (change == "removed") {
      return "remove";
    }
  }
  return change;
}

std::string summary_label(std::string_view state, bool preview, bool plural) {
  if (preview
      && (state == "created" || state == "updated" || state == "removed")) {
    return std::format("to {}", action_label(state, true));
  }
  return state == "conflict" && plural ? "conflicts" : std::string(state);
}

void print_summary(const std::vector<std::string> &states, bool preview) {
  std::string summary;
  for (const auto *state : {
           "current",
           "outdated",
           "missing",
           "foreign",
           "unreadable",
           "created",
           "updated",
           "removed",
           "unchanged",
           "skipped",
           "conflict",
           "failed",
       }) {
    const auto count = std::ranges::count(states, state);
    if (count == 0) {
      continue;
    }
    if (!summary.empty()) {
      summary += ", ";
    }
    summary +=
        std::format("{} {}", count, summary_label(state, preview, count != 1));
  }
  std::println("Summary: {}", summary.empty() ? "no changes" : summary);
}

void append_item(simdjson::builder::string_builder &builder,
                 const fs::path &path, const Inspection &state) {
  builder.start_object();
  builder.append_key_value<"kind">("skill");
  builder.append_comma();
  builder.append_key_value<"path">(path.string());
  builder.append_comma();
  builder.append_key_value<"state">(state.state);
  builder.append_comma();
  builder.append_key_value<"reason">(state.reason);
  builder.end_object();
}

VoidResult emit(const simdjson::builder::string_builder &builder) {
  const auto json = memix::detail::json_from_builder(builder);
  if (!json) {
    return unexpected_error(json.error().message);
  }
  std::println("{}", *json);
  return {};
}

bool append_harness(simdjson::builder::string_builder &builder,
                    const Harness &agent, bool json, const fs::path &home) {
  bool needs_setup = false;
  builder.start_object();
  builder.append_key_value<"id">(agent.id);
  builder.append_comma();
  builder.append_key_value<"detected">(agent.detected);
  builder.append_comma();
  if (!json) {
    std::println("{} ({})", agent_label(agent.id),
                 agent.detected ? "found" : "not found");
  }
  builder.append("items");
  builder.append_colon();
  builder.start_array();
  for (std::size_t index = 0; index < agent.paths.size(); ++index) {
    if (index != 0) {
      builder.append_comma();
    }
    const auto state = inspect(agent.paths[index]);
    append_item(builder, agent.paths[index], state);
    if (!json) {
      const auto detail = state.state == "unreadable"
                            ? state.reason
                            : display_path(agent.paths[index], home);
      std::println("  {:<5}  {:<11}  {}", "skill", state.state, detail);
      needs_setup =
          needs_setup
          || (agent.detected
              && (state.state == "missing" || state.state == "outdated"));
    }
  }
  builder.end_array();
  builder.end_object();
  if (!json) {
    std::println("");
  }
  return needs_setup;
}

VoidResult
report_status(const std::vector<Harness> &agents, const Options &options) {
  const auto home = home_path(false).value_or(fs::path{});
  bool needs_setup = false;
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"scope">(options.project ? "project" : "user");
  builder.append_comma();
  builder.append("harnesses");
  builder.append_colon();
  builder.start_array();
  bool first = true;
  for (const auto &agent : agents) {
    if (!options.agents.empty()
        && !std::ranges::contains(options.agents, agent.id)) {
      continue;
    }
    if (!first) {
      builder.append_comma();
    }
    first = false;
    const bool agent_needs_setup =
        append_harness(builder, agent, options.json, home);
    needs_setup = needs_setup || agent_needs_setup;
  }
  builder.end_array();
  builder.end_object();
  if (!options.json && needs_setup) {
    std::string setup = "memix skill setup";
    if (options.project) {
      setup += " --project";
    }
    for (const auto &agent : options.agents) {
      setup += std::format(" --agent {}", agent);
    }
    std::println("Run \"{}\" to install or update the skill.", setup);
  }
  return options.json ? emit(builder) : VoidResult{};
}

VoidResult
report_steps(const std::vector<Step> &steps, const Options &options) {
  constexpr int action_width = 12;
  const auto home = home_path(false).value_or(fs::path{});
  std::vector<std::string> changes;
  if (!options.json) {
    std::println("Memix skill {}{}\nScope: {}\n", options.command,
                 options.dry_run ? " (preview)" : "",
                 options.project ? "Project" : "User");
    if (!steps.empty()) {
      std::println("  {:<{}}{}", "ACTION", action_width, "AGENTS");
      std::println("  {:<{}}{}", "----------", action_width,
                   "--------------------");
    }
  }
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"action">(options.command == "setup" ? "install"
                                                                : "remove");
  builder.append_comma();
  builder.append_key_value<"scope">(options.project ? "project" : "user");
  builder.append_comma();
  builder.append_key_value<"dry_run">(options.dry_run);
  builder.append_comma();
  builder.append("steps");
  builder.append_colon();
  builder.start_array();
  for (std::size_t index = 0; index < steps.size(); ++index) {
    const auto &step = steps[index];
    if (index != 0) {
      builder.append_comma();
    }
    builder.start_object();
    builder.append_key_value<"harnesses">(step.target.harnesses);
    builder.append_comma();
    builder.append_key_value<"kind">("skill");
    builder.append_comma();
    builder.append_key_value<"path">(step.target.path.string());
    builder.append_comma();
    builder.append_key_value<"change">(step.change);
    builder.append_comma();
    builder.append_key_value<"reason">(step.reason);
    builder.end_object();
    if (!options.json) {
      changes.push_back(step.change);
      std::println("  {:<{}}{}", action_label(step.change, options.dry_run),
                   action_width, agent_labels(step.target.harnesses));
      print_details(step.target.path, step.reason, home);
    }
  }
  builder.end_array();
  builder.end_object();
  if (!options.json) {
    if (steps.empty()) {
      std::println("No agents detected. Select one explicitly, for example:\n"
                   "  memix skill setup --agent pi\n");
    }
    print_summary(changes, options.dry_run);
    if (options.dry_run) {
      std::println("Preview only; no files were changed.");
    } else if (options.command == "setup"
               && !steps.empty()
               && !std::ranges::any_of(steps, &Step::failed)) {
      std::println("Next: memix skill show");
    }
  }
  return options.json ? emit(builder) : VoidResult{};
}

} // namespace

int memix::cli::run_skill(const std::vector<std::string> &arguments) {
  Options options;
  Argum::Parser parser;
  add_options(parser, options, "");
  parser.add(
      Argum::Positional("command")
          .occurs(Argum::neverOrOnce)
          .handler([&](std::string_view value) { options.command = value; }));
  const auto logger = cli_detail::make_logger("memix skill");
  const auto parsed = parser.parse(arguments);
  if (!parsed) {
    logger->error("{}", parsed.error()->message());
    return 1;
  }
  if (!options.command.empty()
      && options.command != "setup"
      && options.command != "status"
      && options.command != "remove"
      && options.command != "show") {
    logger->error(
        "unknown skill command `{}`; expected setup|status|remove|show",
        options.command);
    return 1;
  }
  const auto valid = validate(options);
  if (!valid) {
    logger->error("{}", valid.error().message);
    return 1;
  }
  if (options.common.version_requested) {
    cli_detail::print_version();
    return 0;
  }
  if (options.common.help_requested || arguments.empty()) {
    help(options.command);
    return 0;
  }
  if (options.command.empty()) {
    logger->error("a skill command is required: setup|status|remove|show");
    return 1;
  }
  if (options.command == "show") {
    return run_agent({"guide"});
  }
  const auto agents = harnesses(options);
  if (!agents) {
    logger->error("{}", agents.error().message);
    return 1;
  }
  if (options.command == "status") {
    const auto result = report_status(*agents, options);
    if (!result) {
      logger->error("{}", result.error().message);
    }
    return result ? 0 : 1;
  }
  const auto selected = targets(*agents, options);
  if (!selected) {
    logger->error("{}", selected.error().message);
    return 1;
  }
  std::vector<Step> steps;
  for (const auto &target : *selected) {
    steps.push_back(apply(target, options));
  }
  const auto reported = report_steps(steps, options);
  if (!reported) {
    logger->error("{}", reported.error().message);
    return 1;
  }
  return std::ranges::any_of(steps, &Step::failed) ? 1 : 0;
}
