#include "detail/scope_exit.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"
#include "memix/version.hpp"
#include "memix/view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = memix::fs;

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto root = fs::temp_directory_path();
    REQUIRE(root);
    std::random_device random;
    path_ = *root
          / std::format(
              "memix-cli-{}-{}",
              std::chrono::steady_clock::now().time_since_epoch().count(),
              random());
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    std::error_code error;
    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(path_, error);
         !error && it != end; it.increment(error)) {
      std::filesystem::permissions(it->path(),
                                   std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, error);
    }
    static_cast<void>(fs::remove_all(path_));
  }

  const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

void write_file(const fs::path &path, std::string_view contents) {
  std::ofstream file(path, std::ios::binary);
  REQUIRE(file);
  file << contents;
  REQUIRE(file);
}

std::string read_text(const fs::path &path) {
  std::ifstream file(path);
  REQUIRE(file);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

std::string shell_quote(std::string_view value) {
#ifdef _WIN32
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('"');
#else
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
#endif
  return quoted;
}

std::string binary_path(const char *fallback, const char *override_name) {
  const char *override_path = std::getenv(override_name);
  return override_path != nullptr && override_path[0] != '\0' ? override_path
                                                              : fallback;
}

std::string unified_binary() {
  return binary_path(MEMIX_TEST_BINARY, "MEMIX_TEST_BINARY_OVERRIDE");
}

struct CliResult {
  int status;
  std::string output;
  std::string error;
};

CliResult run_binary(const fs::path &directory, std::string_view binary,
                     const std::vector<std::string> &arguments) {
  const auto output = directory / "stdout.txt";
  const auto error = directory / "stderr.txt";
  std::string command = shell_quote(binary);
  for (const auto &argument : arguments) {
    command += " " + shell_quote(argument);
  }
  command += " >" + shell_quote(output.string());
  command += " 2>" + shell_quote(error.string());
#ifdef _WIN32
  command = "\"" + command + "\"";
#endif
  const int status = std::system(command.c_str()); // NOLINT(cert-env33-c)
  return {status, read_text(output), read_text(error)};
}

CliResult
run(const fs::path &directory, const std::vector<std::string> &arguments) {
  return run_binary(directory, unified_binary(), arguments);
}

CliResult run_command(const fs::path &directory, std::string_view command,
                      const std::vector<std::string> &arguments) {
  std::vector<std::string> full{
      std::string(command),
      "--store-root",
      (directory / "store with spaces").string(),
  };
  full.append_range(arguments);
  return run(directory, full);
}

std::string latest_environment(const fs::path &directory) {
  const memix::ViewService views{memix::Store(directory / "store with spaces")};
  const auto quests = views.list_quests();
  REQUIRE(quests);
  REQUIRE(quests->size() == 1);
  REQUIRE_FALSE(quests->front().latest_environment_id.empty());
  return quests->front().latest_environment_id;
}

} // namespace

TEST_CASE("unified CLI discovers namespaces and reports versions", "[cli]") {
  TemporaryDirectory directory;
  const auto &root = directory.path();
  for (const std::vector<std::string> &arguments :
       {std::vector<std::string>{}, {"--help"}, {"-h"}}) {
    const auto result = run(root, arguments);
    REQUIRE(result.status == 0);
    REQUIRE(result.error.empty());
    REQUIRE(result.output.contains("Usage: memix"));
    REQUIRE(result.output.contains("store"));
    REQUIRE(result.output.contains("view"));
    REQUIRE(result.output.contains("tui"));
  }
  const auto version = run(root, {"--version"});
  REQUIRE(version.status == 0);
  REQUIRE(version.error.empty());
  REQUIRE(version.output == std::format("memix {}\n", MEMIX_VERSION));
  for (const auto *command : {"store", "view", "tui"}) {
    const auto help = run(root, {command, "--help"});
    REQUIRE(help.status == 0);
    REQUIRE(help.error.empty());
    REQUIRE(help.output.contains(std::format("Usage: memix {}", command)));
    REQUIRE(run(root, {"--help", command}).output == help.output);
    REQUIRE(run(root, {command, "--version"}).output == version.output);
    REQUIRE(run(root, {"--version", command}).output == version.output);
  }
}

TEST_CASE("unified CLI provides nested help", "[cli]") {
  TemporaryDirectory directory;
  for (const auto *command :
       {"list", "ingest", "cat", "tree", "expose", "rm", "rules"}) {
    const auto help = run(directory.path(), {"store", command, "--help"});
    REQUIRE(help.status == 0);
    REQUIRE(
        help.output.contains(std::format("Usage: memix store {}", command)));
    REQUIRE(help.error.empty());
  }
  for (const auto *command : {"cat", "tree"}) {
    const auto help =
        run(directory.path(), {"store", "rules", command, "--help"});
    REQUIRE(help.status == 0);
    REQUIRE(help.output.contains(
        std::format("Usage: memix store rules {}", command)));
  }
  for (const auto *command :
       {"define", "find", "resolve", "freshness", "quests", "stats", "list",
        "tree", "cat", "build", "show", "activate", "generations", "rollback",
        "deactivate", "status"}) {
    const auto help = run(directory.path(), {"view", command, "--help"});
    REQUIRE(help.status == 0);
    REQUIRE(help.output.contains(std::format("Usage: memix view {}", command)));
    REQUIRE(help.error.empty());
  }
}

TEST_CASE("unified CLI rejects invalid dispatch without touching the store",
          "[cli]") {
  TemporaryDirectory directory;
  const auto store = (directory.path() / "absent store").string();
  const std::vector<std::vector<std::string>> invalid{
      {"unknown"},
      {"--unknown"},
      {"--store-root"},
      {"--store-root", store},
      {"store"},
      {"view"},
      {"store", "unknown"},
      {"view", "unknown"},
      {"tui", "--unknown"},
      {"store", "--unknown"},
      {"view", "--unknown"},
      {"store", "--store-root", store, "ingest"},
  };
  for (const auto &arguments : invalid) {
    const auto result = run(directory.path(), arguments);
    REQUIRE(result.status != 0);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.output.empty());
  }
  for (const auto *command : {"store", "view", "tui"}) {
    const auto result =
        run(directory.path(), {"--store-root", store, command, "--help"});
    REQUIRE(result.status == 0);
  }
  const auto exists = fs::exists(store);
  REQUIRE(exists);
  REQUIRE_FALSE(*exists);
}

TEST_CASE("unified CLI preserves global options and option terminators",
          "[cli]") {
  TemporaryDirectory directory;
  const auto &root = directory.path();
  const auto store = (root / "store with spaces").string();
  const auto source = root / "source with spaces.txt";
  write_file(source, "literal namespace-like path\n");
  REQUIRE(run(root, {"--store-root=" + store, "store", "ingest", "--type",
                     "rules", "--dest=view", source.string()})
              .status
          == 0);
  const auto expected = run_command(root, "store", {"list", "--json"});
  REQUIRE(expected.status == 0);
  for (const std::vector<std::string> &arguments :
       {std::vector<std::string>{"--store-root", store, "store", "list",
                                 "--json"},
        {"store", "--store-root", store, "list", "--json"},
        {"store", "list", "--json", "--store-root", store}}) {
    const auto result = run(root, arguments);
    REQUIRE(result.status == 0);
    REQUIRE(result.error.empty());
    REQUIRE(result.output == expected.output);
  }
  const auto content =
      run_command(root, "store", {"cat", "--type", "rules", "--", "view"});
  REQUIRE(content.status == 0);
  REQUIRE(content.output == "literal namespace-like path\n");
  REQUIRE(run_command(root, "store", {"rules", "cat", "--", "view"}).output
          == content.output);
  // The parser rejects this positional; it must not turn it into help.
  const auto result =
      run_command(root, "store", {"cat", "--type", "rules", "--", "--help"});
  REQUIRE(result.status != 0);
  REQUIRE(result.error.contains("unexpected argument: --help"));
  REQUIRE(result.output.empty());
}

TEST_CASE("unified CLI forwards option-like store roots literally", "[cli]") {
  TemporaryDirectory directory;
  const auto &root = directory.path();
  std::error_code error;
  const auto original_directory = std::filesystem::current_path(error);
  REQUIRE_FALSE(error);
  const auto binary = std::filesystem::absolute(unified_binary(), error);
  REQUIRE_FALSE(error);
  const memix::scope_exit restore_directory([&original_directory] {
    std::error_code restore_error;
    std::filesystem::current_path(original_directory, restore_error);
  });
  std::filesystem::current_path(root, error);
  REQUIRE_FALSE(error);

  const auto source = root / "input.md";
  write_file(source, "selected option-like store\n");
  const auto invoke = [&](const std::vector<std::string> &arguments) {
    return run_binary(root, binary.string(), arguments);
  };
  for (const auto *store :
       {"-data", "--help", "--version", "--", "-data with spaces"}) {
    CAPTURE(store);
    const auto option = std::format("--store-root={}", store);
    const auto ingest = invoke({
        "store",
        option,
        "ingest",
        "--type",
        "rules",
        "--dest",
        "entry.md",
        source.string(),
    });
    INFO(ingest.error);
    REQUIRE(ingest.status == 0);
    const auto content =
        invoke({option, "store", "cat", "--type", "rules", "entry.md"});
    INFO(content.error);
    REQUIRE(content.status == 0);
    REQUIRE(content.error.empty());
    REQUIRE(content.output == "selected option-like store\n");

    for (const auto *command : {"store", "view", "tui"}) {
      CAPTURE(command);
      const auto expected = invoke({command, option, "--help"});
      const auto forwarded = invoke({option, command, "--help"});
      REQUIRE(expected.status == 0);
      REQUIRE(expected.error.empty());
      REQUIRE(forwarded.status == 0);
      REQUIRE(forwarded.error.empty());
      REQUIRE(forwarded.output == expected.output);
    }
  }
}

TEST_CASE("unified CLI preserves store view and activation workflows",
          "[cli]") {
  TemporaryDirectory directory;
  const auto &root = directory.path();
  const auto source = root / "input.md";
  const auto attachment = root / "workspace" / ".memory";
  REQUIRE(fs::create_directories(attachment.parent_path()));
  const auto ingest = [&] {
    return run_command(
        root, "store",
        {"ingest", "--type", "rules", "--dest", "AGENTS.md", source.string()});
  };
  const auto build = [&] {
    return run_command(root, "view",
                       {"build", "--source", "rules/AGENTS.md", "--intent",
                        "CLI migration test", "--json"});
  };
  const auto activate = [&](const std::string &environment) {
    return run_command(root, "view",
                       {"activate", environment, "--into", attachment.string(),
                        "--mode", "copy"});
  };
  write_file(source, "first generation\n");
  REQUIRE(ingest().status == 0);
  REQUIRE(build().status == 0);
  const auto first = latest_environment(root);
  const auto view_output = [&](const std::vector<std::string> &arguments) {
    const auto result = run_command(root, "view", arguments);
    INFO(result.error);
    REQUIRE(result.status == 0);
    REQUIRE(result.error.empty());
    return result.output;
  };
  REQUIRE(view_output({"list", "--json"}).contains(first));
  REQUIRE(view_output({"show", first, "--json"}).contains(first));
  REQUIRE(view_output({"tree", first, "--json"}).contains("rules/AGENTS.md"));
  REQUIRE(view_output({"cat", first, "rules/AGENTS.md"})
          == "first generation\n");
  REQUIRE(activate(first).status == 0);
  REQUIRE(read_text(attachment / "rules" / "AGENTS.md")
          == "first generation\n");
  write_file(source, "second generation\n");
  REQUIRE(ingest().status == 0);
  REQUIRE(build().status == 0);
  const auto second = latest_environment(root);
  REQUIRE(first != second);
  REQUIRE(activate(second).status == 0);
  REQUIRE(read_text(attachment / "rules" / "AGENTS.md")
          == "second generation\n");
  const auto status = view_output({"status", "--into", attachment.string()});
  REQUIRE(status.contains("active: true\n"));
  REQUIRE(status.contains("generation: 2\n"));
  REQUIRE(status.contains(second));
  const auto generations =
      view_output({"generations", "--into", attachment.string()});
  constexpr std::size_t kDisplayIdLength = 12;
  REQUIRE(generations.contains(first.substr(0, kDisplayIdLength)));
  REQUIRE(generations.contains(second.substr(0, kDisplayIdLength)));
  REQUIRE(run_command(root, "view", {"rollback", "--into", attachment.string()})
              .status
          == 0);
  REQUIRE(read_text(attachment / "rules" / "AGENTS.md")
          == "first generation\n");
  REQUIRE(
      run_command(root, "view", {"deactivate", "--into", attachment.string()})
          .status
      == 0);
  REQUIRE(run_command(root, "view", {"status", "--into", attachment.string()})
              .output
          == "active: false\n");
  REQUIRE(
      run_command(root, "store", {"rm", "--type", "rules", "AGENTS.md"}).status
      == 0);
  REQUIRE(run_command(root, "store", {"tree", "--all", "--json"}).output
          == "[]\n");
  REQUIRE(run_command(root, "view", {"cat", first, "rules/AGENTS.md"}).output
          == "first generation\n");
}

TEST_CASE("unified binary runs without sibling executables", "[cli]") {
  TemporaryDirectory directory;
  const auto isolated = directory.path() / "isolated";
  REQUIRE(fs::create_directories(isolated));
#ifdef _WIN32
  const auto executable = isolated / "renamed memix.exe";
#else
  const auto executable = isolated / "renamed memix";
#endif
  std::error_code error;
  REQUIRE(std::filesystem::copy_file(unified_binary(), executable, error));
  REQUIRE_FALSE(error);
  for (const auto *command : {"store", "view", "tui"}) {
    const auto result =
        run_binary(directory.path(), executable.string(), {command, "--help"});
    REQUIRE(result.status == 0);
    REQUIRE(result.output.contains(std::format("Usage: memix {}", command)));
  }
  const auto store = (directory.path() / "absent").string();
  REQUIRE(run_binary(directory.path(), executable.string(),
                     {"store", "--store-root", store, "list", "--json"})
              .output
          == "[]\n");
  REQUIRE(run_binary(directory.path(), executable.string(),
                     {"view", "--store-root", store, "list", "--json"})
              .output
          == "[]\n");
}
