#include "memix/version.hpp"

#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

int main(int argc, char **argv) {
  CLI::App app{"memix store utilities."};
  std::vector<std::string> ingest_paths;

  app.set_version_flag("--version", std::string("memix ") + MEMIX_VERSION);
  app.require_subcommand(1);

  auto *list_cmd =
      app.add_subcommand("list", "List the contents of the store.");
  list_cmd->callback(
      [&]() { std::cout << "list is not implemented yet." << '\n'; });

  auto *ingest_cmd =
      app.add_subcommand("ingest", "Ingest one or more paths into the store.");
  ingest_cmd
      ->add_option("path", ingest_paths, "Path(s) to ingest into the store.")
      ->required();
  ingest_cmd->callback([&]() {
    std::cout << "ingest is not implemented yet for:";
    for (const auto &path : ingest_paths) {
      std::cout << ' ' << path;
    }
    std::cout << '\n';
  });

  CLI11_PARSE(app, argc, argv);
  return 0;
}
