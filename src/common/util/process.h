#ifndef IPC_COMMON_UTIL_PROCESS_H_
#define IPC_COMMON_UTIL_PROCESS_H_

#include <cstdint>

namespace ipc::common {

// The calling process's PID.
std::int32_t CurrentProcessId();

// True if a process with this PID is currently alive.
bool IsProcessAlive(std::int32_t pid);

}  // namespace ipc::common

#endif  // IPC_COMMON_UTIL_PROCESS_H_
