#include "commands.hpp"

#include "agent_guides.hpp"
#include "cli_common.hpp"
#include "memix/version.hpp"

#include <argum/data.h>
#include <argum/parser.h>

#include <algorithm>
#include <print>
#include <string>
#include <string_view>
#include <vector>

int memix::cli::run_agent(const std::vector<std::string> &arguments) {
  Argum::Parser parser;
  cli_detail::CommonOptions options;
  cli_detail::add_common_options(parser, options);
  std::string topic;
  parser.add(Argum::Positional("topic")
                 .occurs(Argum::neverOrOnce)
                 .help("Print bundled Markdown: guide, recall, save, compose, "
                       "attach, or recovery. Does not open a Store.")
                 .handler([&topic](std::string_view value) { topic = value; }));
  const auto logger = cli_detail::make_logger("memix agent");
  const auto parsed = parser.parse(arguments);
  if (!parsed) {
    logger->error("{}", parsed.error()->message());
    return 1;
  }
  const auto *guide = std::ranges::find(cli_detail::agent_guides, topic,
                                        &cli_detail::AgentGuide::topic);
  if (!topic.empty() && guide == cli_detail::agent_guides.end()) {
    logger->error("unknown agent topic `{}`; expected "
                  "guide|recall|save|compose|attach|recovery",
                  topic);
    return 1;
  }
  if (options.version_requested) {
    cli_detail::print_version();
    return 0;
  }
  if (options.help_requested || topic.empty()) {
    std::print("{}", parser.formatHelp("memix agent"));
    std::println("\nStart with 'memix agent guide', then load only the topics "
                 "needed for the task. --store-root is accepted but unused.");
    return 0;
  }
  std::print("<!-- memix {} agent {} -->\n\n{}", MEMIX_VERSION, topic,
             guide->text);
  return 0;
}
