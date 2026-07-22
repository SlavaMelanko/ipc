#ifndef IPC_COMMON_TRANSPORT_SHM_BLOCKING_RING_BUFFER_H_
#define IPC_COMMON_TRANSPORT_SHM_BLOCKING_RING_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "common/transport/shm/mapped_segment.h"
#include "common/transport/shm/named_semaphore.h"

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

  // Why AcquireReadSlot() last returned nullptr. Best-effort for kPeerClosed
  // -- see AGENTS.md's "Waking a blocked send()/receive()" for the narrower
  // gap that remains.
  enum class ReadFailure : std::uint8_t {
    kNone,
    kError,
    kPeerClosed,
    kEndOfStream,
  };

  std::byte* AcquireWriteSlot();
  bool CommitWrite();

  // Blocks until a message is available, retrying in bounded steps so the
  // producer's PID and lifecycle state can be periodically checked. Returns
  // nullptr on any ReadFailure other than kNone; callers that care why should
  // check LastReadFailure() after a null return.
  std::byte* AcquireReadSlot();
  bool CommitRead();

  [[nodiscard]] ReadFailure LastReadFailure() const { return lastReadFailure_; }

  // Producer-only: marks no more messages are coming, then Closed. A
  // consumer blocked in AcquireReadSlot() notices within one poll interval
  // -- see AGENTS.md's "Clean producer shutdown and the receive predicate".
  void Close();

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
  ReadFailure lastReadFailure_ = ReadFailure::kNone;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_SHM_BLOCKING_RING_BUFFER_H_
