#ifndef IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_
#define IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_

#include <cstddef>
#include <memory>
#include <string>

#include "common/transport/control_block.h"
#include "common/transport/mapped_segment.h"
#include "common/transport/transport.h"

namespace ipc::common {

class SharedMemoryTransport : public ITransport {
 public:
  // Fixed at every iteration (see AGENTS.md's "Naming"), not caller-supplied:
  // the producer and consumer must agree on it without either side passing
  // it around.
  static constexpr const char* kSegmentName = "/ipc_ring_v1";

  // Creates a new segment and initializes the control block: mutex,
  // condvars, and both cursors zeroed. Fails (returns nullptr) if a segment
  // with this name already exists.
  static std::unique_ptr<ITransport> CreateProducer(std::size_t payloadSize,
                                                    std::size_t ringCapacityBytes);

  // Attaches to a segment a producer has already created. Fails (returns
  // nullptr) if the segment doesn't exist.
  static std::unique_ptr<ITransport> AttachConsumer(std::size_t payloadSize,
                                                    std::size_t ringCapacityBytes);

  bool Send(const Message& message) override;
  bool Receive(Message& message) override;

 private:
  SharedMemoryTransport(MappedSegment segment, std::size_t payloadSize, std::size_t slotCount);

  ControlBlock& Control();
  std::byte* SlotAt(std::uint64_t index);

  MappedSegment segment_;
  std::size_t payloadSize_;
  std::size_t slotCount_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_
