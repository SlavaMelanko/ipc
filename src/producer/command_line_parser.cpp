#include "producer/command_line_parser.h"

#include <charconv>
#include <string_view>

namespace {

std::expected<std::uint64_t, std::string> ParsePositiveInt(
    std::string_view flag, std::string_view value) {
  std::uint64_t result = 0;
  auto [ptr, ec] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (ec != std::errc{} || ptr != value.data() + value.size()) {
    return std::unexpected(
        std::string(flag) +
        " must be a positive integer, got: " + std::string(value));
  }
  if (result == 0) {
    return std::unexpected(std::string(flag) + " must be greater than zero");
  }
  return result;
}

}  // namespace

namespace ipc::producer {

std::expected<CommandLineArgs, std::string> ParseCommandLine(int argc,
                                                             char** argv) {
  CommandLineArgs args;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    if (arg == "--count") {
      if (i + 1 >= argc) {
        return std::unexpected("--count requires a value");
      }
      auto count = ParsePositiveInt("--count", argv[++i]);
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
