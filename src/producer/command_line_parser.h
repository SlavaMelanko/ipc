#ifndef IPC_PRODUCER_COMMAND_LINE_PARSER_H_
#define IPC_PRODUCER_COMMAND_LINE_PARSER_H_

#include <cstdint>
#include <expected>
#include <string>

namespace ipc::producer {

struct CommandLineArgs {
  std::uint64_t count = 1000;
};

// Parses argv. Returns an error message on invalid input (e.g. non-numeric
// or non-positive --count).
std::expected<CommandLineArgs, std::string> ParseCommandLine(int argc, char** argv);

}  // namespace ipc::producer

#endif  // IPC_PRODUCER_COMMAND_LINE_PARSER_H_
