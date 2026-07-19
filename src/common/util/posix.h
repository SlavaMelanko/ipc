#ifndef IPC_COMMON_UTIL_POSIX_H_
#define IPC_COMMON_UTIL_POSIX_H_

#include <sys/stat.h>

namespace ipc::common {

// POSIX calls (syscalls, pthread functions) return 0 on success, a nonzero
// errno value on failure.
constexpr bool Ok(int returnCode) { return returnCode == 0; }
constexpr bool Failed(int returnCode) { return returnCode != 0; }

// Owner read/write, no group/other access -- used for every named
// shm_open/sem_open resource this project creates.
constexpr mode_t kOwnerReadWrite = 0600;

}  // namespace ipc::common

#endif  // IPC_COMMON_UTIL_POSIX_H_
