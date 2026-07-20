#ifndef IPC_COMMON_CONTROL_STDIN_WATCHER_H_
#define IPC_COMMON_CONTROL_STDIN_WATCHER_H_

#include <thread>

#include "common/control/controller.h"

namespace ipc::common {

// Reads single-character commands from stdin on a dedicated thread --
// 'p' pauses, 'r' resumes, 'q' stops -- and drives Controller accordingly.
//
// A blocking read() can't be woken by a condition variable, so shutdown
// closes stdin's file descriptor to force the read to return (see AGENTS.md's
// "Control loop and signal safety") rather than leaving the thread stuck
// until process exit.
class StdinWatcher {
 public:
  explicit StdinWatcher(Controller& controller);
  ~StdinWatcher();

  StdinWatcher(const StdinWatcher&) = delete;
  StdinWatcher& operator=(const StdinWatcher&) = delete;

 private:
  void Run();

  Controller& controller_;
  std::thread thread_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_CONTROL_STDIN_WATCHER_H_
