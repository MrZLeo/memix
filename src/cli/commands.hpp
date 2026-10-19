#pragma once

#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace memix::cli {

// Drop argv[0]; entrypoints supply a canonical help/log name independently
// of where the executable was installed or what the release asset is called.
inline std::vector<std::string> arguments_from_main(int argc, char **argv) {
  return std::span(argv, static_cast<std::size_t>(argc)).subspan(1)
       | std::ranges::to<std::vector<std::string>>();
}

int run(const std::vector<std::string> &arguments);
int run_store(const std::vector<std::string> &arguments,
              std::string_view program_name);
int run_view(const std::vector<std::string> &arguments,
             std::string_view program_name);
int run_tui(const std::vector<std::string> &arguments,
            std::string_view program_name);

} // namespace memix::cli
