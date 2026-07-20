#ifndef IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_
#define IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace ipc::common {

// Bump by hand whenever Header's or ControlBlock's byte layout changes.
inline constexpr std::uint32_t kWireFormatVersion = 1;

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

// Reads and transitions ControlBlock::state, hiding the atomic cast and
// memory ordering. Producer drives every transition; consumer only reads.
class StateManager {
 public:
  explicit StateManager(ControlBlock& control) : control_(control) {}

  [[nodiscard]] LifecycleState GetState() const {
    return static_cast<LifecycleState>(control_.state.load(std::memory_order_acquire));
  }

  void Initializing() { Store(LifecycleState::kInitializing); }

  // Call once construction has fully succeeded.
  void Ready(std::uint32_t layoutVersion) {
    control_.layoutVersion = layoutVersion;
    Store(LifecycleState::kReady);
  }

  void Stop() { Store(LifecycleState::kStopping); }
  void Close() { Store(LifecycleState::kClosed); }

  [[nodiscard]] bool IsStoppingOrClosed() const {
    LifecycleState state = GetState();

    return state == LifecycleState::kStopping || state == LifecycleState::kClosed;
  }

 private:
  void Store(LifecycleState state) {
    control_.state.store(static_cast<std::uint32_t>(state), std::memory_order_release);
  }

  ControlBlock& control_;
};

// Writes producerPid before publishing state = Initializing.
bool InitControlBlock(ControlBlock& control, std::int32_t producerPid);

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_
