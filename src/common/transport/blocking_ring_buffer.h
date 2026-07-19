#ifndef IPC_COMMON_TRANSPORT_BLOCKING_RING_BUFFER_H_
#define IPC_COMMON_TRANSPORT_BLOCKING_RING_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "common/transport/mapped_segment.h"
#include "common/transport/named_semaphore.h"

namespace ipc::common {

struct ControlBlock;

// Bounded SPSC ring of fixed-size slots over shared memory, blocking via
// named semaphores. Deals in raw slot bytes; framing is the caller's job.
class BlockingRingBuffer {
 public:
  static std::optional<BlockingRingBuffer> Create(const std::string& name,
                                                  std::size_t ringCapacityBytes,
                                                  std::size_t payloadSize);
  static std::optional<BlockingRingBuffer> Attach(const std::string& name,
                                                  std::size_t ringCapacityBytes,
                                                  std::size_t payloadSize);

  BlockingRingBuffer(const BlockingRingBuffer&) = delete;
  BlockingRingBuffer& operator=(const BlockingRingBuffer&) = delete;
  BlockingRingBuffer(BlockingRingBuffer&&) noexcept = default;
  BlockingRingBuffer& operator=(BlockingRingBuffer&&) noexcept = default;
  ~BlockingRingBuffer() = default;

  std::byte* AcquireWriteSlot();
  void CommitWrite();

  std::byte* AcquireReadSlot();
  void CommitRead();

 private:
  BlockingRingBuffer(MappedSegment segment, NamedSemaphore freeSlots,
                     NamedSemaphore availableMessages, std::size_t payloadSize,
                     std::size_t slotCount);

  ControlBlock& Control();
  std::byte* SlotAt(std::uint64_t index);

  MappedSegment segment_;
  NamedSemaphore freeSlots_;
  NamedSemaphore availableMessages_;
  std::size_t payloadSize_;
  std::size_t slotCount_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_BLOCKING_RING_BUFFER_H_
