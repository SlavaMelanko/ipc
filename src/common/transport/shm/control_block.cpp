#include "common/transport/shm/control_block.h"

namespace ipc::common {

void InitControlBlock(ControlBlock& control, std::int32_t producerPid) {
  control.producerPid = producerPid;
  StateManager(control).Initializing();

  control.writeCursor.store(0, std::memory_order_relaxed);
  control.readCursor.store(0, std::memory_order_relaxed);
}

}  // namespace ipc::common
