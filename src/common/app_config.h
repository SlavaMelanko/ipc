#ifndef IPC_COMMON_APP_CONFIG_H_
#define IPC_COMMON_APP_CONFIG_H_

#include <cstddef>
#include <cstdint>

namespace ipc::common {

// --payload-size/--ring-capacity must match between producer and consumer
// exactly -- both sides derive the same slotCount from these (see
// AttachConsumer/ring_layout.h), and there's no cross-process check catching
// a mismatch beyond layoutVersion (wire-format shape, not these two values).
struct AppConfig {
  std::uint64_t count = 1000;
  std::size_t payloadSize = 1024;
  std::size_t ringCapacityBytes = std::size_t{8} * 1024 * 1024;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_APP_CONFIG_H_
