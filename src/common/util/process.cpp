#include "common/util/process.h"

#include <unistd.h>

#include <cerrno>
#include <csignal>

namespace ipc::common {

std::int32_t CurrentProcessId() { return static_cast<std::int32_t>(getpid()); }

bool IsProcessAlive(std::int32_t pid) {
  if (pid <= 0) {
    return false;
  }

  // EPERM means it exists but we can't signal it -- still alive.
  return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

}  // namespace ipc::common
