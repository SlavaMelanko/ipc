#include "producer/command_line_parser.h"

#include <string_view>

#include "common/util/cli.h"

namespace ipc::producer {

std::expected<CommandLineArgs, std::string> ParseCommandLine(int argc, char** argv) {
  CommandLineArgs args;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    if (arg == "--count") {
      if (i + 1 >= argc) {
        return std::unexpected("--count requires a value");
      }
      auto count = ipc::common::ParsePositiveInt("--count", argv[++i]);
      if (!count) {
        return std::unexpected(count.error());
      }
      args.count = *count;
    } else {
      return std::unexpected("unrecognized argument: " + std::string(arg));
    }
  }

  return args;
}

}  // namespace ipc::producer
