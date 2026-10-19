#include "cli/commands.hpp"

int main(int argc, char **argv) {
  return memix::cli::run(memix::cli::arguments_from_main(argc, argv));
}
