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

  return ipc::common::Ok(pthread_mutexattr_setpshared(
             &mutexAttr, PTHREAD_PROCESS_SHARED)) &&
         ipc::common::Ok(pthread_mutex_init(&mutex, &mutexAttr));
}

bool InitProcessSharedConds(pthread_cond_t& slotFreeCond,
                            pthread_cond_t& messageAvailableCond) {
  pthread_condattr_t condAttr;
  if (ipc::common::Failed(pthread_condattr_init(&condAttr))) {
    return false;
  }
  ipc::common::ScopeExit destroyAttr(
      [&condAttr]() noexcept { pthread_condattr_destroy(&condAttr); });

  if (ipc::common::Failed(
          pthread_condattr_setpshared(&condAttr, PTHREAD_PROCESS_SHARED))) {
    return false;
  }

  return ipc::common::Ok(pthread_cond_init(&slotFreeCond, &condAttr)) &&
         ipc::common::Ok(pthread_cond_init(&messageAvailableCond, &condAttr));
}

}  // namespace

namespace ipc::common {

bool ControlBlock::Init(ControlBlock& control) {
  if (!InitProcessSharedMutex(control.mutex)) {
    return false;
  }

  if (!InitProcessSharedConds(control.slotFreeCond,
                              control.messageAvailableCond)) {
    return false;
  }

  // Ring starts empty: producer and consumer begin at the same slot.
  control.writeCursor = 0;
  control.readCursor = 0;

  return true;
}

}  // namespace ipc::common
