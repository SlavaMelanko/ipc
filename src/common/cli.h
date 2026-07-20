#ifndef IPC_COMMON_CLI_H_
#define IPC_COMMON_CLI_H_

#include <expected>
#include <string_view>

#include "common/app_config.h"

namespace ipc::common {

// Parses --count/--payload-size/--ring-capacity into an AppConfig. appName
// customizes the CLI's program name (producer/consumer). On --help or a bad
// argument, CLI11 has already printed the message/usage; the returned int is
// the process exit code to use as-is (0 for --help, nonzero otherwise).
std::expected<AppConfig, int> ParseCommandLineArguments(int argc, char** argv,
                                                        std::string_view appName);

}  // namespace ipc::common

#endif  // IPC_COMMON_CLI_H_
