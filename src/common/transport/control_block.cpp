#include "common/transport/control_block.h"

#include "common/util/posix.h"
#include "common/util/scope_exit.h"

namespace {

bool InitProcessSharedMutex(pthread_mutex_t& mutex) {
  pthread_mutexattr_t mutexAttr;
  if (ipc::common::Failed(pthread_mutexattr_init(&mutexAttr))) {
    return false;
  }
  ipc::common::ScopeExit destroyAttr(
      [&mutexAttr]() noexcept { pthread_mutexattr_destroy(&mutexAttr); });

  return ipc::common::Ok(pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED)) &&
         ipc::common::Ok(pthread_mutex_init(&mutex, &mutexAttr));
}

}  // namespace

namespace ipc::common {

bool ControlBlock::Init(ControlBlock& control) {
  if (!InitProcessSharedMutex(control.cursorMutex)) {
    return false;
  }

  control.writeCursor = 0;
  control.readCursor = 0;

  return true;
}

}  // namespace ipc::common
