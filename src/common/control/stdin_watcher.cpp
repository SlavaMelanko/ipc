#include "common/control/stdin_watcher.h"

#include <unistd.h>

namespace ipc::common {

StdinWatcher::StdinWatcher(Controller& controller)
    : controller_(controller), thread_(&StdinWatcher::Run, this) {}

StdinWatcher::~StdinWatcher() {
  close(STDIN_FILENO);
  thread_.join();
}

void StdinWatcher::Run() {
  char command = 0;

  while (read(STDIN_FILENO, &command, 1) == 1) {
    switch (command) {
      case 'p':
        controller_.Pause();
        break;
      case 'r':
        controller_.Resume();
        break;
      case 'q':
        controller_.Stop();
        return;
      default:
        break;
    }
  }
}

}  // namespace ipc::common
