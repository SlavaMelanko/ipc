#ifndef IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_
#define IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_

#include <cstddef>
#include <optional>
#include <string>

#include "common/transport/control_block.h"
#include "common/transport/mapped_segment.h"
#include "common/transport/transport.h"

namespace ipc::common {

class SharedMemoryTransport : public ITransport {
 public:
  // Creates a new segment and initializes the control block: mutex,
  // condvars, and both cursors zeroed. Fails if a segment with this name
  // already exists.
  static std::optional<SharedMemoryTransport> CreateProducer(
      const std::string& name, std::size_t payloadSize,
      std::size_t ringCapacityBytes);

  // Attaches to a segment a producer has already created. Fails if the
  // segment doesn't exist.
  static std::optional<SharedMemoryTransport> AttachConsumer(
      const std::string& name, std::size_t payloadSize,
      std::size_t ringCapacityBytes);

  bool Send(const Message& message) override;
  bool Receive(Message& message) override;

 private:
  SharedMemoryTransport(MappedSegment segment, std::size_t payloadSize,
                        std::size_t slotCount);

  ControlBlock& Control();
  std::byte* SlotAt(std::uint64_t index);

  MappedSegment segment_;
  std::size_t payloadSize_;
  std::size_t slotCount_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_
