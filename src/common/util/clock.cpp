#include "common/util/clock.h"

#include <chrono>

namespace ipc::common {

std::uint64_t CurrentTimestamp() {
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace ipc::common
