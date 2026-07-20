#ifndef IPC_COMMON_CONTROL_SIGNAL_HANDLER_H_
#define IPC_COMMON_CONTROL_SIGNAL_HANDLER_H_

#include <thread>

#include "common/control/controller.h"

namespace ipc::common {

// Blocks SIGINT/SIGTERM on the calling thread (so they can never run an
// async-signal-unsafe handler -- see AGENTS.md's "Control loop and signal
// safety") and waits for either on a dedicated thread via sigwait(), calling
// controller.Stop() from that thread's ordinary context once one arrives.
//
// Must be constructed before any other thread that should also have these
// signals blocked (signal masks are inherited by threads spawned afterward).
class SignalHandler {
 public:
  explicit SignalHandler(Controller& controller);
  ~SignalHandler();

  SignalHandler(const SignalHandler&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;

 private:
  void Run();

  Controller& controller_;
  std::thread thread_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_CONTROL_SIGNAL_HANDLER_H_
