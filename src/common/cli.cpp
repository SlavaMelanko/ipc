#include "common/cli.h"

#include <CLI/CLI.hpp>
#include <string>

namespace ipc::common {

namespace {

void AddOptions(CLI::App& cli, AppConfig& config) {
  cli.add_option("--count", config.count, "Number of messages to transfer")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  cli.add_option("--payload-size", config.payloadSize, "Exact payload size in bytes")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  cli.add_option("--ring-capacity", config.ringCapacityBytes, "Shared-memory segment size in bytes")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
}

}  // namespace

std::expected<AppConfig, int> ParseCommandLineArguments(int argc, char** argv,
                                                        std::string_view appName) {
  CLI::App cli{std::string(appName)};
  AppConfig config;
  AddOptions(cli, config);

  try {
    cli.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return std::unexpected(cli.exit(e));
  }

  return config;
}

}  // namespace ipc::common
