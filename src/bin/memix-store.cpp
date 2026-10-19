#include "cli/commands.hpp"

int main(int argc, char **argv) {
  return memix::cli::run_store(memix::cli::arguments_from_main(argc, argv),
                               "memix-store");
}
