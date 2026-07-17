#ifndef IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_
#define IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_

#include <pthread.h>

#include <cstdint>
#include <type_traits>

namespace ipc::common {

struct ControlBlock {
  // Guards the cursors below and the full/empty check on every send/receive.
  pthread_mutex_t mutex;
  // Producer waits here when the ring is full; consumer signals it after
  // freeing a slot.
  pthread_cond_t slotFreeCond;
  // Consumer waits here when the ring is empty; producer signals it after
  // writing a slot.
  pthread_cond_t messageAvailableCond;
  // Next slot the producer will write. Producer-owned.
  std::uint64_t writeCursor;
  // Next slot the consumer will read. Consumer-owned.
  std::uint64_t readCursor;

  // Initializes the mutex, condvars, and cursors in place. Producer-only,
  // called once on a freshly mmap'd segment before any send()/receive().
  static bool Init(ControlBlock& control);
};

// Placed raw in shared memory, so no assumptions beyond trivial-copyability
// hold across platforms — pthread_mutex_t/pthread_cond_t sizes differ
// between macOS and Linux.
static_assert(std::is_trivially_copyable_v<ControlBlock>);

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_CONTROL_BLOCK_H_
