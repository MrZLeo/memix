#include "memix/version.hpp"

#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

int main(int argc, char **argv) {
  CLI::App app{"memix view utilities."};
  std::vector<std::string> show_items;

  app.set_version_flag("--version", std::string("memix ") + MEMIX_VERSION);
  app.require_subcommand(1);

  auto *list_cmd = app.add_subcommand("list", "List the available views.");
  list_cmd->callback(
      [&]() { std::cout << "list is not implemented yet." << '\n'; });

  auto *show_cmd =
      app.add_subcommand("show", "Show one or more items in the view.");
  show_cmd->add_option("item", show_items, "Item(s) to display in the view.")
      ->required();
  show_cmd->callback([&]() {
    std::cout << "show is not implemented yet for:";
    for (const auto &item : show_items) {
      std::cout << ' ' << item;
    }
    std::cout << '\n';
  });

  CLI11_PARSE(app, argc, argv);
  return 0;
}
