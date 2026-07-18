#ifndef IPC_CONSUMER_CONFIG_H_
#define IPC_CONSUMER_CONFIG_H_

#include <cstddef>

namespace ipc::consumer {

// Must match producer::Config exactly -- both sides derive the same
// slotCount from these (see AttachConsumer/ring_layout.h), and v1 has no
// layoutVersion check to catch a mismatch.
struct Config {
  static constexpr std::size_t payloadSize = 1024;
  static constexpr std::size_t ringCapacityBytes = std::size_t{8} * 1024 * 1024;
};

}  // namespace ipc::consumer

#endif  // IPC_CONSUMER_CONFIG_H_
