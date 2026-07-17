#include "common/transport/control_block.h"

namespace {

bool InitProcessSharedMutex(pthread_mutex_t& mutex) {
  pthread_mutexattr_t mutexAttr;
  if (pthread_mutexattr_init(&mutexAttr) != 0) {
    return false;
  }

  bool ok =
      pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED) == 0 &&
      pthread_mutex_init(&mutex, &mutexAttr) == 0;

  pthread_mutexattr_destroy(&mutexAttr);

  return ok;
}

bool InitProcessSharedConds(pthread_cond_t& slotFreeCond,
                            pthread_cond_t& messageAvailableCond) {
  pthread_condattr_t condAttr;
  if (pthread_condattr_init(&condAttr) != 0) {
    return false;
  }

  if (pthread_condattr_setpshared(&condAttr, PTHREAD_PROCESS_SHARED) != 0) {
    pthread_condattr_destroy(&condAttr);
    return false;
  }

  bool ok = pthread_cond_init(&slotFreeCond, &condAttr) == 0 &&
            pthread_cond_init(&messageAvailableCond, &condAttr) == 0;

  pthread_condattr_destroy(&condAttr);

  return ok;
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
