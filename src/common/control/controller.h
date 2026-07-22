#ifndef IPC_COMMON_CONTROL_CONTROLLER_H_
#define IPC_COMMON_CONTROL_CONTROLLER_H_

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace ipc::common {

enum class RunState : std::uint8_t {
  kRunning,
  kPaused,
  kStopped,
};

// In-process Running/Paused/Stopped state, shared by a stdin control thread,
// a signal handler thread, and the app's own send/receive loop. Cross-process
// signaling (the ring's freeSlots/availableMessages semaphores) is a separate
// mechanism.
class Controller {
 public:
  void Pause();
  void Resume();
  void Stop();

  [[nodiscard]] RunState State() const;

  // Blocks while Paused. Returns false once Stopped (whether already stopped
  // on entry, or woken by a later Stop()); true means the caller may proceed.
  [[nodiscard]] bool WaitIfPaused();

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  RunState state_ = RunState::kRunning;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_CONTROL_CONTROLLER_H_
