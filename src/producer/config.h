#ifndef IPC_PRODUCER_CONFIG_H_
#define IPC_PRODUCER_CONFIG_H_

#include <cstddef>

namespace ipc::producer {

struct Config {
  static constexpr std::size_t payloadSize = 1024;
  static constexpr std::size_t ringCapacityBytes = std::size_t{8} * 1024 * 1024;
};

}  // namespace ipc::producer

#endif  // IPC_PRODUCER_CONFIG_H_
