#ifndef IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_
#define IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace ipc::common {

// Width matches the atomic<uint32_t> it's stored in, not a plain enum class.
enum class LifecycleState : std::uint32_t {  // NOLINT(performance-enum-size)
  kInitializing = 0,
  kReady = 1,
  kStopping = 2,
  kClosed = 3,
};

struct ControlBlock {
  // Not mutex-guarded; readers acquire-load it directly.
  std::atomic<std::uint32_t> state;
  std::uint32_t layoutVersion;
  // Must be valid at or before the Initializing transition, not Ready.
  std::int32_t producerPid;

  // Guards only the cursor increment; the slot copy itself needs no lock.
  pthread_mutex_t cursorMutex;
  std::uint64_t writeCursor;
  std::uint64_t readCursor;
};

// Placed raw in shared memory: must stay trivially copyable across platforms.
static_assert(std::is_trivially_copyable_v<ControlBlock>);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

bool InitControlBlock(ControlBlock& control);

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_
