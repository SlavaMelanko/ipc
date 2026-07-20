#ifndef IPC_COMMON_UTIL_CLOCK_H_
#define IPC_COMMON_UTIL_CLOCK_H_

#include <cstdint>

namespace ipc::common {

// steady_clock ticks "now".
std::uint64_t CurrentTimestamp();

}  // namespace ipc::common

#endif  // IPC_COMMON_UTIL_CLOCK_H_
