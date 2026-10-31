#include "memix/fs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = memix::fs;

namespace {

std::string read(const fs::path &path) {
  const auto text = fs::read_text_file(path);
  REQUIRE(text);
  return *text;
}

std::string normalized_newlines(std::string text) {
  std::erase(text, '\r');
  return text;
}

void write(const fs::path &path, std::string_view text) {
  REQUIRE(fs::create_directories(path.parent_path()));
  REQUIRE(fs::write_text_file(path, text));
}

std::string quote(std::string_view value) {
#ifdef _WIN32
  return "\"" + std::string(value) + "\"";
#else
  std::string result = "'";
  for (const char character : value) {
    result += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return result + "'";
#endif
}

void set_env(const char *name, const std::optional<std::string> &value) {
#ifdef _WIN32
  REQUIRE(_putenv_s(name, value.value_or("").c_str()) == 0);
#else
  REQUIRE((value ? setenv(name, value->c_str(), 1) : unsetenv(name)) == 0);
#endif
}

struct Result {
  int status;
  std::string output;
  std::string error;
};

class Sandbox {
public:
  Sandbox() {
    const auto temp = fs::temp_directory_path();
    REQUIRE(temp);
    const auto unique = fs::unique_temp_dir(
        *temp, {.prefix = "skill-test",
                .failure_message = "cannot create skill test directory"});
    REQUIRE(unique);
    root = *unique;
    home = root / "home";
    REQUIRE(fs::create_directories(home));
    cwd_ = std::filesystem::current_path();
    const char *override_binary = std::getenv("MEMIX_TEST_BINARY_OVERRIDE");
    binary = override_binary != nullptr && *override_binary != '\0'
               ? override_binary
               : MEMIX_TEST_BINARY;
    binary = std::filesystem::absolute(binary).string();
    for (const auto *name :
         {"HOME", "USERPROFILE", "PATH", "CODEX_HOME", "CLAUDE_CONFIG_DIR",
          "PI_CODING_AGENT_DIR", "DSH_HOME", "XDG_CONFIG_HOME"}) {
      const char *value = std::getenv(name);
      environment_.emplace_back(name, value == nullptr
                                          ? std::nullopt
                                          : std::optional<std::string>(value));
      set_env(name, std::nullopt);
    }
    set_env("HOME", home.string());
    set_env("USERPROFILE", home.string());
    set_env("PATH", std::string{});
    std::filesystem::current_path(root);
  }

  ~Sandbox() {
    std::filesystem::current_path(cwd_);
    for (const auto &[name, value] : environment_) {
      set_env(name.c_str(), value);
    }
    static_cast<void>(fs::remove_all(root));
  }

  Result full(const std::vector<std::string> &arguments) const {
    std::string command = quote(binary);
    for (const auto &argument : arguments) {
      command += " " + quote(argument);
    }
    command += " >" + quote((root / "stdout").string());
    command += " 2>" + quote((root / "stderr").string());
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    const auto status = std::system(command.c_str()); // NOLINT(cert-env33-c)
    // Preserve shell output bytes; comparisons use the same output mode.
    const auto output = read(root / "stdout");
    return {.status = status, .output = output, .error = read(root / "stderr")};
  }

  Result run(const std::vector<std::string> &arguments) const {
    std::vector<std::string> full_arguments{
        "--store-root", (root / "absent-store").string(), "skill"};
    full_arguments.append_range(arguments);
    return full(full_arguments);
  }

  fs::path shared() const { return home / ".agents/skills/memix-cli"; }

  fs::path root;
  fs::path home;
  std::string binary;

private:
  fs::path cwd_;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment_;
};

void succeeds(const Result &result) {
  INFO(result.output);
  INFO(result.error);
  REQUIRE(result.status == 0);
  REQUIRE(result.error.empty());
}

bool path_exists(const fs::path &path) {
  const auto value = fs::exists(path);
  REQUIRE(value);
  return *value;
}

} // namespace

TEST_CASE("skill help and show need neither a home nor a store",
          "[skill][cli]") {
  Sandbox sandbox;
  set_env("HOME", std::nullopt);
  set_env("USERPROFILE", std::nullopt);
  for (const auto *command : {"setup", "status", "remove", "show"}) {
    const auto result = sandbox.run({command, "--help"});
    succeeds(result);
    REQUIRE(
        result.output.contains("Usage: memix skill " + std::string(command)));
  }
  succeeds(sandbox.run({"--help"}));
  const auto shown = sandbox.run({"show"});
  succeeds(shown);
  REQUIRE(shown.output == sandbox.full({"agent", "guide"}).output);
  REQUIRE(sandbox.run({"setup", "--agent", "pi"}).status != 0);
  REQUIRE_FALSE(path_exists(sandbox.root / "absent-store"));
}

TEST_CASE("skill status matches Rex groups and abbreviates only home paths",
          "[skill][cli]") {
  Sandbox sandbox;
  succeeds(sandbox.run({"setup", "--agent", "pi"}));
  const auto status = sandbox.run({"status", "--agent", "pi"});
  succeeds(status);
  REQUIRE(normalized_newlines(status.output)
          == "Pi (not found)\n"
             "  skill  current      ~/.agents/skills/memix-cli\n\n");
  REQUIRE_FALSE(status.output.contains('\x1b'));
  const auto json = sandbox.run({"status", "--agent", "pi", "--json"});
  succeeds(json);
  REQUIRE(json.output.starts_with("{\"scope\":\"user\",\"harnesses\":["));
  REQUIRE(json.output.contains("\"state\":\"current\""));
  REQUIRE_FALSE(json.output.contains("~/"));
  REQUIRE_FALSE(json.output.contains("(not found)"));

  const auto external = sandbox.root / "home-extra";
  set_env("PI_CODING_AGENT_DIR", external.string());
  const auto outside = sandbox.run({"status", "--agent", "pi"});
  succeeds(outside);
  REQUIRE(normalized_newlines(outside.output)
          == "Pi (not found)\n  skill  missing      "
                 + (external / "skills/memix-cli").string()
                 + "\n\n");
}

TEST_CASE("skill status matches Rex state details and conditional setup hints",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto config = sandbox.home / ".pi/agent";
  REQUIRE(fs::create_directories(config));
  const std::string hint =
      "Run \"memix skill setup --agent pi\" to install or update the skill.\n";
  const auto missing = sandbox.run({"status", "--agent", "pi"});
  succeeds(missing);
  REQUIRE(normalized_newlines(missing.output)
          == "Pi (found)\n  skill  missing      ~/.agents/skills/memix-cli\n\n"
                 + hint);

  succeeds(sandbox.run({"setup", "--agent", "pi"}));
  const auto current = sandbox.run({"status", "--agent", "pi"});
  succeeds(current);
  REQUIRE(
      normalized_newlines(current.output)
      == "Pi (found)\n  skill  current      ~/.agents/skills/memix-cli\n\n");

  write(sandbox.shared() / "SKILL.md",
        read(sandbox.shared() / "SKILL.md") + "edited\n");
  const auto outdated = sandbox.run({"status", "--agent", "pi"});
  succeeds(outdated);
  REQUIRE(normalized_newlines(outdated.output)
          == "Pi (found)\n  skill  outdated     ~/.agents/skills/memix-cli\n\n"
                 + hint);

  REQUIRE(fs::remove(config));
  const auto absent_agent = sandbox.run({"status", "--agent", "pi"});
  succeeds(absent_agent);
  REQUIRE(normalized_newlines(absent_agent.output)
          == "Pi (not found)\n  skill  outdated     "
             "~/.agents/skills/memix-cli\n\n");
  REQUIRE(fs::create_directories(config));

  write(sandbox.shared() / "SKILL.md", "foreign\n");
  const auto foreign = sandbox.run({"status", "--agent", "pi"});
  succeeds(foreign);
  REQUIRE(
      normalized_newlines(foreign.output)
      == "Pi (found)\n  skill  foreign      ~/.agents/skills/memix-cli\n\n");
  const auto json = sandbox.run({"status", "--agent", "pi", "--json"});
  succeeds(json);
  REQUIRE(json.output.contains("SKILL.md was not installed by memix"));

  write(sandbox.shared() / "SKILL.md", std::string(2 * 1024 * 1024, 'x'));
  const auto unreadable = sandbox.run({"status", "--agent", "pi"});
  succeeds(unreadable);
  REQUIRE(normalized_newlines(unreadable.output)
          == "Pi (found)\n  skill  unreadable   skill file is too large: "
                 + (sandbox.shared() / "SKILL.md").string()
                 + "\n\n");
}

TEST_CASE("skill status setup hints preserve project and agent selection",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto repo = sandbox.root / "project";
  write(repo / ".git", "gitdir: elsewhere\n");
  REQUIRE(fs::create_directories(repo / ".pi"));
  std::filesystem::current_path(repo);
  const auto all = sandbox.run({"status", "--project"});
  succeeds(all);
  REQUIRE(
      normalized_newlines(all.output).starts_with("Claude Code (not found)\n"));
  REQUIRE(normalized_newlines(all.output)
              .ends_with("Run \"memix skill setup --project\" to install or "
                         "update the skill.\n"));
  REQUIRE_FALSE(all.output.contains("Scope:"));
  REQUIRE_FALSE(all.output.contains("Summary:"));
  const auto selected = sandbox.run({"status", "--project", "--agent", "pi"});
  succeeds(selected);
  REQUIRE(normalized_newlines(selected.output)
              .ends_with("Run \"memix skill setup --project --agent pi\" to "
                         "install or update the skill.\n"));
}

TEST_CASE("skill operation reports distinguish previews from applied changes",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto preview =
      sandbox.run({"setup", "--agent", "codex", "--agent", "pi", "--dry-run"});
  succeeds(preview);
  REQUIRE(preview.output.contains("Memix skill setup (preview)"));
  REQUIRE(preview.output.contains("  ACTION      AGENTS"));
  REQUIRE(preview.output.contains("  create      Codex, Pi"));
  REQUIRE(preview.output.contains("Summary: 1 to create"));
  REQUIRE(preview.output.contains("Preview only; no files were changed."));
  REQUIRE_FALSE(preview.output.contains("would:"));
  REQUIRE_FALSE(path_exists(sandbox.shared()));
  const auto json =
      sandbox.run({"setup", "--agent", "pi", "--dry-run", "--json"});
  succeeds(json);
  REQUIRE(json.output.contains("\"dry_run\":true"));
  REQUIRE(json.output.contains("\"change\":\"created\""));
  REQUIRE_FALSE(json.output.contains("Summary:"));
  const auto installed = sandbox.run({"setup", "--agent", "pi"});
  succeeds(installed);
  REQUIRE(installed.output.contains("Summary: 1 created"));
  REQUIRE(installed.output.contains("Next: memix skill show"));
  const auto removal = sandbox.run({"remove", "--agent", "pi", "--dry-run"});
  succeeds(removal);
  REQUIRE(removal.output.contains("Summary: 1 to remove"));
  REQUIRE(path_exists(sandbox.shared()));
  const auto removed = sandbox.run({"remove", "--agent", "pi"});
  succeeds(removed);
  REQUIRE(removed.output.contains("Summary: 1 removed"));
}

TEST_CASE("skill conflicts show readable wrapped notes without changing JSON",
          "[skill][cli]") {
  Sandbox sandbox;
  write(sandbox.shared() / "SKILL.md", "foreign\n");
  const auto result = sandbox.run({"setup", "--agent", "pi", "--dry-run"});
  REQUIRE(result.status != 0);
  REQUIRE(result.output.contains("  conflict    Pi"));
  REQUIRE(
      result.output.contains("    Note: SKILL.md was not installed by memix"));
  REQUIRE(result.output.contains("          replace the two skill files"));
  REQUIRE(result.output.contains("Summary: 1 conflict"));
  std::istringstream lines(result.output);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.ends_with('\r')) {
      line.pop_back();
    }
    if (line.starts_with("    Note:") || line.starts_with("          ")) {
      REQUIRE(line.size() <= 78);
    }
  }
  const auto json =
      sandbox.run({"setup", "--agent", "pi", "--dry-run", "--json"});
  REQUIRE(json.status != 0);
  REQUIRE(json.output.contains("\"change\":\"conflict\""));
  REQUIRE(json.output.contains(
      "review it and use --force to replace the two skill files"));
  REQUIRE_FALSE(json.output.contains("Summary:"));
}

TEST_CASE("skill empty reports and help provide grouped next steps",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto empty = sandbox.run({"setup", "--dry-run"});
  succeeds(empty);
  REQUIRE(empty.output.contains("No agents detected."));
  REQUIRE(empty.output.contains("  memix skill setup --agent pi"));
  REQUIRE(empty.output.contains("Summary: no changes"));
  REQUIRE_FALSE(empty.output.contains("ACTION"));
  const auto help = sandbox.run({"--help"});
  succeeds(help);
  REQUIRE(help.output.contains("Commands:"));
  REQUIRE(help.output.contains("Examples:"));
  REQUIRE(help.output.contains("Notes:"));
  REQUIRE(help.output.contains("Accepted but unused"));
  const auto show_help = sandbox.run({"show", "--help"});
  succeeds(show_help);
  REQUIRE(show_help.output.contains(
      "Prints the same Markdown as 'memix agent guide'"));
  REQUIRE_FALSE(show_help.output.contains("shared"));
  const auto status_help = sandbox.run({"status", "--help"});
  succeeds(status_help);
  REQUIRE(status_help.output.contains("States:"));
}

TEST_CASE("skill setup is bundled idempotent and shared by agents",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto source = fs::path(MEMIX_TEST_SKILL_SOURCE_DIR);
  const auto copied = sandbox.root / fs::path(sandbox.binary).filename();
  REQUIRE(fs::copy_file(sandbox.binary, copied));
  REQUIRE(
      fs::permissions(copied, fs::perms::owner_exec, fs::perm_options::add));
  sandbox.binary = copied.string();
  const std::vector<std::string> setup{
      "setup",   "--agent",  "pi",      "--agent", "codex",
      "--agent", "opencode", "--agent", "pi",      "--json"};
  auto result = sandbox.run(setup);
  succeeds(result);
  REQUIRE(
      result.output.contains("\"harnesses\":[\"codex\",\"opencode\",\"pi\"]"));
  REQUIRE(read(sandbox.shared() / "SKILL.md") == read(source / "SKILL.md"));
  REQUIRE(read(sandbox.shared() / "agents/openai.yaml")
          == read(source / "agents/openai.yaml"));
  result = sandbox.run(setup);
  succeeds(result);
  REQUIRE(result.output.contains("\"change\":\"unchanged\""));
  const auto status = sandbox.run({"status", "--agent", "pi", "--json"});
  succeeds(status);
  REQUIRE(status.output.contains("\"state\":\"current\""));
  REQUIRE(status.output.contains("\"detected\":false"));
  // Removal still finds skills even when the agent is no longer installed.
  succeeds(sandbox.run({"remove", "--json"}));
  REQUIRE_FALSE(path_exists(sandbox.shared()));
  REQUIRE_FALSE(path_exists(sandbox.root / "absent-store"));
}

TEST_CASE("skill dry runs and status are read only", "[skill][cli]") {
  Sandbox sandbox;
  succeeds(sandbox.run({"status", "--json"}));
  const auto dry =
      sandbox.run({"setup", "--agent", "claude", "--dry-run", "--json"});
  succeeds(dry);
  REQUIRE(dry.output.contains("\"change\":\"created\""));
  REQUIRE_FALSE(path_exists(sandbox.home / ".claude"));
  succeeds(sandbox.run({"setup", "--agent", "claude"}));
  const auto skill = sandbox.home / ".claude/skills/memix-cli";
  const auto before = read(skill / "SKILL.md");
  succeeds(sandbox.run({"remove", "--agent", "claude", "--dry-run", "--json"}));
  REQUIRE(read(skill / "SKILL.md") == before);
  succeeds(sandbox.run({"remove", "--agent", "claude"}));
  REQUIRE_FALSE(path_exists(skill));
}

TEST_CASE("skill auto detection selects config directories or executables",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto empty = sandbox.run({"setup", "--json"});
  succeeds(empty);
  REQUIRE(empty.output.contains("\"steps\":[]"));
  REQUIRE_FALSE(path_exists(sandbox.shared()));
  REQUIRE(fs::create_directories(sandbox.home / ".pi/agent"));
  REQUIRE(fs::create_directories(sandbox.home / ".codex"));
  const auto detected = sandbox.run({"setup", "--json"});
  succeeds(detected);
  REQUIRE(detected.output.contains("\"harnesses\":[\"codex\",\"pi\"]"));
  REQUIRE_FALSE(path_exists(sandbox.home / ".claude"));
  REQUIRE_FALSE(path_exists(sandbox.home / ".dsh"));
#ifdef _WIN32
  const auto executable = sandbox.root / "tools/dsh.cmd";
#else
  const auto executable = sandbox.root / "tools/dsh";
#endif
  write(executable, "not executed\n");
  REQUIRE(fs::permissions(executable, fs::perms::owner_exec,
                          fs::perm_options::add));
  set_env("PATH", executable.parent_path().string());
  succeeds(sandbox.run({"setup", "--json"}));
  REQUIRE(path_exists(sandbox.home / ".dsh/skills/memix-cli/SKILL.md"));
}

TEST_CASE("skill honors agent home overrides", "[skill][cli]") {
  Sandbox sandbox;
  for (const auto &[agent, variable] :
       std::to_array<std::pair<const char *, const char *>>(
           {{"claude", "CLAUDE_CONFIG_DIR"},
            {"codex", "CODEX_HOME"},
            {"pi", "PI_CODING_AGENT_DIR"},
            {"dsh", "DSH_HOME"}})) {
    const auto custom = sandbox.root / (std::string(agent) + " custom home");
    set_env(variable, custom.string());
    succeeds(sandbox.run({"setup", "--agent", agent}));
    REQUIRE(path_exists(custom / "skills/memix-cli/SKILL.md"));
    REQUIRE_FALSE(path_exists(sandbox.shared()));
    succeeds(sandbox.run({"remove", "--agent", agent}));
    set_env(variable, std::nullopt);
  }
  const auto xdg = sandbox.root / "xdg";
  set_env("XDG_CONFIG_HOME", xdg.string());
  REQUIRE(fs::create_directories(xdg / "opencode"));
  succeeds(sandbox.run({"setup"}));
  REQUIRE(path_exists(sandbox.shared() / "SKILL.md"));
}

TEST_CASE("skill project scope resolves repository roots including worktrees",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto repo = sandbox.root / "project";
  SECTION("Git worktree") { write(repo / ".git", "gitdir: elsewhere\n"); }
  SECTION("Jujutsu") { REQUIRE(fs::create_directories(repo / ".jj")); }
  REQUIRE(fs::create_directories(repo / "sub/directory"));
  std::filesystem::current_path(repo / "sub/directory");
  succeeds(sandbox.run({"setup", "--project", "--agent", "pi", "--agent",
                        "claude", "--agent", "dsh"}));
  REQUIRE(path_exists(repo / ".agents/skills/memix-cli/SKILL.md"));
  REQUIRE(path_exists(repo / ".claude/skills/memix-cli/SKILL.md"));
  REQUIRE(path_exists(repo / ".dsh/skills/memix-cli/SKILL.md"));
  REQUIRE_FALSE(path_exists(sandbox.shared()));
  const auto status = sandbox.run({"status", "--project", "--json"});
  succeeds(status);
  REQUIRE(status.output.contains("\"scope\":\"project\""));
  succeeds(sandbox.run({"remove", "--project"}));
  REQUIRE_FALSE(path_exists(repo / ".agents/skills/memix-cli"));
}

TEST_CASE("skill diagnoses unsupported scope and arguments before writing",
          "[skill][cli]") {
  Sandbox sandbox;
  for (const auto &arguments : std::vector<std::vector<std::string>>{
           {"setup", "--project", "--agent", "pi"},
           {"unknown"},
           {"setup", "extra"},
           {"setup", "--agent", "unknown"},
           {"setup", "--agent"},
           {"--force"},
           {"remove", "--force"},
           {"status", "--dry-run"},
           {"show", "--json"},
           {"show", "--project"},
           {"show", "--agent", "pi"},
           {"setup", "--unknown"}}) {
    const auto result = sandbox.run(arguments);
    REQUIRE(result.status != 0);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.output.empty());
  }
  set_env("CODEX_HOME", std::string("relative/path"));
  REQUIRE(sandbox.run({"setup", "--agent", "codex"}).status != 0);
  REQUIRE_FALSE(path_exists(sandbox.shared()));
}

TEST_CASE("skill protects foreign and legacy copies unless force is explicit",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto native = sandbox.home / ".pi/agent/skills/memix-cli";
  write(native / "SKILL.md", "old unmarked skill\n");
  write(native / "notes.txt", "keep me\n");
  const auto conflict = sandbox.run({"setup", "--agent", "pi", "--json"});
  REQUIRE(conflict.status != 0);
  REQUIRE(conflict.output.contains("\"change\":\"conflict\""));
  REQUIRE(read(native / "SKILL.md") == "old unmarked skill\n");
  succeeds(sandbox.run({"remove", "--agent", "pi"}));
  REQUIRE(path_exists(native / "SKILL.md"));
  succeeds(sandbox.run({"setup", "--agent", "pi", "--force", "--dry-run"}));
  REQUIRE(read(native / "SKILL.md") == "old unmarked skill\n");
  succeeds(sandbox.run({"setup", "--agent", "pi", "--force"}));
  REQUIRE(read(native / "SKILL.md").contains("memix skill show"));
  REQUIRE_FALSE(path_exists(sandbox.shared()));
  REQUIRE(read(native / "notes.txt") == "keep me\n");
  const auto original = read(native / "SKILL.md");
  write(native / "SKILL.md", original + "local edit\n");
  const auto outdated = sandbox.run({"status", "--agent", "pi", "--json"});
  succeeds(outdated);
  REQUIRE(outdated.output.contains("\"state\":\"outdated\""));
  succeeds(sandbox.run({"setup", "--agent", "pi"}));
  REQUIRE(read(native / "SKILL.md") == original);
  write(native / "agents/extra.yaml", "also keep me\n");
  succeeds(sandbox.run({"remove", "--agent", "pi"}));
  REQUIRE_FALSE(path_exists(native / "SKILL.md"));
  REQUIRE_FALSE(path_exists(native / "agents/openai.yaml"));
  REQUIRE(read(native / "notes.txt") == "keep me\n");
  REQUIRE(read(native / "agents/extra.yaml") == "also keep me\n");
}

TEST_CASE("skill reconciles shared and native managed copies", "[skill][cli]") {
  Sandbox sandbox;
  succeeds(sandbox.run({"setup", "--agent", "pi"}));
  const auto native = sandbox.home / ".pi/agent/skills/memix-cli";
  write(native / "SKILL.md", read(sandbox.shared() / "SKILL.md"));
  write(native / "agents/openai.yaml",
        read(sandbox.shared() / "agents/openai.yaml"));
  succeeds(sandbox.run({"remove", "--agent", "pi", "--json"}));
  REQUIRE_FALSE(path_exists(sandbox.shared()));
  REQUIRE_FALSE(path_exists(native));
}

TEST_CASE("skill repairs partial bundles but protects foreign metadata",
          "[skill][cli]") {
  Sandbox sandbox;
  succeeds(sandbox.run({"setup", "--agent", "pi"}));
  const auto markdown = read(sandbox.shared() / "SKILL.md");
  const auto metadata = read(sandbox.shared() / "agents/openai.yaml");
  REQUIRE(fs::remove(sandbox.shared() / "agents/openai.yaml"));
  REQUIRE(sandbox.run({"status", "--agent", "pi", "--json"})
              .output.contains("\"state\":\"outdated\""));
  succeeds(sandbox.run({"setup", "--agent", "pi"}));
  REQUIRE(read(sandbox.shared() / "agents/openai.yaml") == metadata);
  REQUIRE(fs::remove(sandbox.shared() / "SKILL.md"));
  succeeds(sandbox.run({"setup", "--agent", "pi"}));
  REQUIRE(read(sandbox.shared() / "SKILL.md") == markdown);
  write(sandbox.shared() / "agents/openai.yaml", "foreign metadata\n");
  REQUIRE(sandbox.run({"setup", "--agent", "pi"}).status != 0);
  succeeds(sandbox.run({"remove", "--agent", "pi"}));
  REQUIRE(read(sandbox.shared() / "SKILL.md") == markdown);
  REQUIRE(read(sandbox.shared() / "agents/openai.yaml")
          == "foreign metadata\n");
  succeeds(sandbox.run({"setup", "--agent", "pi", "--force"}));
  REQUIRE(read(sandbox.shared() / "agents/openai.yaml") == metadata);
}

TEST_CASE("skill refuses unsafe layouts even with force", "[skill][cli]") {
  Sandbox sandbox;
  SECTION("skill root is a file") { write(sandbox.shared(), "foreign\n"); }
  SECTION("agents is a file") {
    write(sandbox.shared() / "agents", "foreign\n");
  }
  SECTION("SKILL.md is a directory") {
    REQUIRE(fs::create_directories(sandbox.shared() / "SKILL.md"));
  }
  const auto failed =
      sandbox.run({"setup", "--agent", "pi", "--force", "--json"});
  REQUIRE(failed.status != 0);
  REQUIRE(failed.output.contains("\"change\":\"failed\""));
  succeeds(sandbox.run({"remove", "--agent", "pi"}));
  REQUIRE(path_exists(sandbox.shared()));
}

#ifndef _WIN32
TEST_CASE("skill never follows symlinks within a skill", "[skill][cli]") {
  Sandbox sandbox;
  const auto outside = sandbox.root / "outside";
  write(outside / "SKILL.md", "do not modify\n");
  REQUIRE(fs::create_directories(sandbox.shared().parent_path()));
  SECTION("skill directory") {
    REQUIRE(fs::create_directory_symlink(outside, sandbox.shared()));
  }
  SECTION("SKILL.md") {
    REQUIRE(fs::create_directories(sandbox.shared()));
    REQUIRE(fs::create_symlink(outside / "SKILL.md",
                               sandbox.shared() / "SKILL.md"));
  }
  SECTION("agents directory") {
    REQUIRE(fs::create_directories(sandbox.shared()));
    REQUIRE(fs::create_directory_symlink(outside, sandbox.shared() / "agents"));
  }
  SECTION("metadata file") {
    REQUIRE(fs::create_directories(sandbox.shared() / "agents"));
    REQUIRE(fs::create_symlink(outside / "SKILL.md",
                               sandbox.shared() / "agents/openai.yaml"));
  }
  REQUIRE(sandbox.run({"setup", "--agent", "pi", "--force"}).status != 0);
  succeeds(sandbox.run({"remove", "--agent", "pi"}));
  REQUIRE(read(outside / "SKILL.md") == "do not modify\n");
}
#endif

TEST_CASE("skill locks protect installation and are not forcibly removed",
          "[skill][cli]") {
  Sandbox sandbox;
  const auto lock = sandbox.shared().parent_path() / ".memix-cli-skill.lock";
  REQUIRE(fs::create_directories(lock));
  const auto failed =
      sandbox.run({"setup", "--agent", "pi", "--force", "--json"});
  REQUIRE(failed.status != 0);
  REQUIRE(failed.output.contains("lock exists"));
  REQUIRE_FALSE(path_exists(sandbox.shared()));
  REQUIRE(path_exists(lock));
  REQUIRE(fs::remove(lock));
  succeeds(sandbox.run({"setup", "--agent", "pi"}));
  REQUIRE_FALSE(path_exists(lock));
}

TEST_CASE("skill reports unreadable oversized files without modifying them",
          "[skill][cli]") {
  Sandbox sandbox;
  write(sandbox.shared() / "SKILL.md", std::string(2 * 1024 * 1024, 'x'));
  const auto status = sandbox.run({"status", "--agent", "pi", "--json"});
  succeeds(status);
  REQUIRE(status.output.contains("\"state\":\"unreadable\""));
  REQUIRE(sandbox.run({"setup", "--agent", "pi", "--force"}).status != 0);
  REQUIRE(sandbox.run({"remove", "--agent", "pi"}).status != 0);
  REQUIRE(read(sandbox.shared() / "SKILL.md").size() == 2 * 1024 * 1024);
}

TEST_CASE("skill reports independent successes alongside conflicts",
          "[skill][cli]") {
  Sandbox sandbox;
  write(sandbox.shared() / "SKILL.md", "foreign\n");
  const auto result =
      sandbox.run({"setup", "--agent", "claude", "--agent", "pi", "--json"});
  REQUIRE(result.status != 0);
  REQUIRE(result.output.contains("\"change\":\"created\""));
  REQUIRE(result.output.contains("\"change\":\"conflict\""));
  REQUIRE(path_exists(sandbox.home / ".claude/skills/memix-cli/SKILL.md"));
  REQUIRE(read(sandbox.shared() / "SKILL.md") == "foreign\n");
}
