#ifndef IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_
#define IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_

#include <pthread.h>

#include <cstdint>
#include <type_traits>

namespace ipc::common {

struct ControlBlock {
  // Guards only the cursor increment; the slot copy itself needs no lock.
  pthread_mutex_t cursorMutex;
  std::uint64_t writeCursor;
  std::uint64_t readCursor;

  static bool Init(ControlBlock& control);
};

// Placed raw in shared memory: must stay trivially copyable across platforms.
static_assert(std::is_trivially_copyable_v<ControlBlock>);

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_
