#ifndef IPC_CONSUMER_COMMAND_LINE_PARSER_H_
#define IPC_CONSUMER_COMMAND_LINE_PARSER_H_

#include <cstdint>
#include <expected>
#include <string>

namespace ipc::consumer {

struct CommandLineArgs {
  std::uint64_t count = 1000;
};

// Parses argv. Returns an error message on invalid input (e.g. non-numeric
// or non-positive --count).
std::expected<CommandLineArgs, std::string> ParseCommandLine(int argc, char** argv);

}  // namespace ipc::consumer

#endif  // IPC_CONSUMER_COMMAND_LINE_PARSER_H_
