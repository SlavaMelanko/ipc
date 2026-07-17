#include "common/transport/shared_memory_transport.h"

#include <cassert>

#include "common/transport/ring_layout.h"

namespace ipc::common {

std::optional<SharedMemoryTransport> SharedMemoryTransport::CreateProducer(
    const std::string& name, std::size_t payloadSize,
    std::size_t ringCapacityBytes) {
  std::size_t slotCount = SlotCount(ringCapacityBytes, payloadSize);
  if (slotCount == 0) {
    return std::nullopt;
  }

  auto segment = MappedSegment::Create(name, ringCapacityBytes);
  if (!segment) {
    return std::nullopt;
  }

  auto* control = static_cast<ControlBlock*>(segment->data());
  if (!ControlBlock::Init(*control)) {
    return std::nullopt;
  }

  return SharedMemoryTransport(std::move(*segment), payloadSize, slotCount);
}

std::optional<SharedMemoryTransport> SharedMemoryTransport::AttachConsumer(
    const std::string& name, std::size_t payloadSize,
    std::size_t ringCapacityBytes) {
  std::size_t slotCount = SlotCount(ringCapacityBytes, payloadSize);
  if (slotCount == 0) {
    return std::nullopt;
  }

  auto segment = MappedSegment::Attach(name, ringCapacityBytes);
  if (!segment) {
    return std::nullopt;
  }

  return SharedMemoryTransport(std::move(*segment), payloadSize, slotCount);
}

SharedMemoryTransport::SharedMemoryTransport(MappedSegment segment,
                                             std::size_t payloadSize,
                                             std::size_t slotCount)
    : segment_(std::move(segment)),
      payloadSize_(payloadSize),
      slotCount_(slotCount) {}

ControlBlock& SharedMemoryTransport::Control() {
  return *static_cast<ControlBlock*>(segment_.data());
}

std::byte* SharedMemoryTransport::SlotAt(std::uint64_t index) {
  std::size_t stride = SlotStride(payloadSize_);
  auto* base = static_cast<std::byte*>(segment_.data()) + SlotAreaOffset();
  return base + ((index % slotCount_) * stride);
}

bool SharedMemoryTransport::Send(const Message& /*message*/) {
  assert(false && "Send() is implemented in v1 build step 4");
  return false;
}

bool SharedMemoryTransport::Receive(Message& /*message*/) {
  assert(false && "Receive() is implemented in v1 build step 4");
  return false;
}

}  // namespace ipc::common
