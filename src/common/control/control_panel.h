#ifndef IPC_COMMON_CONTROL_CONTROL_PANEL_H_
#define IPC_COMMON_CONTROL_CONTROL_PANEL_H_

#include "common/control/controller.h"
#include "common/control/signal_handler.h"
#include "common/control/stdin_watcher.h"

namespace ipc::common {

// Bundles the three pieces every app wires up identically: the Running/
// Paused/Stopped state, the stdin 'p'/'r'/'q' thread, and the SIGINT/SIGTERM
// thread. Construct once at the top of App::Run(); the send/receive loop
// only needs WaitIfPaused().
class ControlPanel {
 public:
  ControlPanel() : signalHandler_(controller_), stdinWatcher_(controller_) {}

  [[nodiscard]] bool WaitIfPaused() { return controller_.WaitIfPaused(); }

 private:
  Controller controller_;
  SignalHandler signalHandler_;
  StdinWatcher stdinWatcher_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_CONTROL_CONTROL_PANEL_H_
