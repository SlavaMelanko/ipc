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

bool InitControlBlock(ControlBlock& control, std::int32_t producerPid) {
  control.producerPid = producerPid;
  control.state.store(static_cast<std::uint32_t>(LifecycleState::kInitializing),
                      std::memory_order_release);

  if (!InitProcessSharedMutex(control.cursorMutex)) {
    return false;
  }

  control.writeCursor = 0;
  control.readCursor = 0;

  return true;
}

void PublishReady(ControlBlock& control, std::uint32_t layoutVersion) {
  control.layoutVersion = layoutVersion;
  control.state.store(static_cast<std::uint32_t>(LifecycleState::kReady),
                      std::memory_order_release);
}

}  // namespace ipc::common
