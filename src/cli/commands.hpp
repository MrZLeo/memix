#pragma once

#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace memix::cli {

// Drop argv[0]; command names stay independent of the executable's path
// or the platform-specific release asset name.
inline std::vector<std::string> arguments_from_main(int argc, char **argv) {
  return std::span(argv, static_cast<std::size_t>(argc)).subspan(1)
       | std::ranges::to<std::vector<std::string>>();
}

int run(const std::vector<std::string> &arguments);
int run_agent(const std::vector<std::string> &arguments);
int run_skill(const std::vector<std::string> &arguments);
int run_store(const std::vector<std::string> &arguments);
int run_view(const std::vector<std::string> &arguments);
int run_tui(const std::vector<std::string> &arguments);

} // namespace memix::cli
