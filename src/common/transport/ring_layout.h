#ifndef IPC_COMMON_TRANSPORT_RING_LAYOUT_H_
#define IPC_COMMON_TRANSPORT_RING_LAYOUT_H_

#include <cstddef>
#include <limits>

#include "common/message/header.h"
#include "common/transport/control_block.h"

namespace ipc::common {

// Rounds value up to the next multiple of alignment.
constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// Bytes one slot occupies, including padding so every slot starts aligned.
// Returns 0 if sizeof(Header) + payloadSize would overflow.
constexpr std::size_t SlotStride(std::size_t payloadSize) {
  if (payloadSize > std::numeric_limits<std::size_t>::max() - sizeof(Header)) {
    return 0;
  }

  return AlignUp(sizeof(Header) + payloadSize, alignof(std::max_align_t));
}

// Byte offset from the mapping's start to slot 0. sizeof(ControlBlock) only
// guarantees alignof(ControlBlock); reinterpreting raw bytes as a Header at an
// address weaker than alignof(max_align_t) is undefined behavior, so this
// rounds up to the same alignment SlotStride uses.
constexpr std::size_t SlotAreaOffset() {
  return AlignUp(sizeof(ControlBlock), alignof(std::max_align_t));
}

// How many whole slots fit after the control block. Returns 0 if the ring
// capacity can't even hold the control block, or not a single slot.
constexpr std::size_t SlotCount(std::size_t ringCapacityBytes,
                                std::size_t payloadSize) {
  std::size_t stride = SlotStride(payloadSize);
  if (stride == 0 || ringCapacityBytes <= SlotAreaOffset()) {
    return 0;
  }

  return (ringCapacityBytes - SlotAreaOffset()) / stride;
}

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_RING_LAYOUT_H_
