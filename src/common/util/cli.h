#ifndef IPC_COMMON_UTIL_CLI_H_
#define IPC_COMMON_UTIL_CLI_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace ipc::common {

// Parses value as a positive (nonzero) uint64_t. flag names the option in
// error messages (e.g. "--count").
std::expected<std::uint64_t, std::string> ParsePositiveInt(std::string_view flag,
                                                           std::string_view value);

}  // namespace ipc::common

#endif  // IPC_COMMON_UTIL_CLI_H_
