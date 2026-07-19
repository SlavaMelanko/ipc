#include "common/transport/shared_memory_transport.h"

#include <cstring>
#include <utility>

namespace ipc::common {

std::unique_ptr<ITransport> SharedMemoryTransport::CreateProducer(std::size_t payloadSize,
                                                                  std::size_t ringCapacityBytes) {
  auto ring = BlockingRingBuffer::Create(kSegmentName, ringCapacityBytes, payloadSize);
  if (!ring) {
    return nullptr;
  }

  return std::unique_ptr<SharedMemoryTransport>(
      new SharedMemoryTransport(std::move(*ring), payloadSize));
}

std::unique_ptr<ITransport> SharedMemoryTransport::AttachConsumer(std::size_t payloadSize,
                                                                  std::size_t ringCapacityBytes) {
  auto ring = BlockingRingBuffer::Attach(kSegmentName, ringCapacityBytes, payloadSize);
  if (!ring) {
    return nullptr;
  }

  return std::unique_ptr<SharedMemoryTransport>(
      new SharedMemoryTransport(std::move(*ring), payloadSize));
}

bool SharedMemoryTransport::Send(const Message& message) {
  if (message.header.payloadSize != payloadSize_) {
    return false;
  }

  std::byte* slot = ring_.AcquireWriteSlot();
  if (slot == nullptr) {
    return false;
  }

  std::memcpy(slot, &message.header, sizeof(Header));
  std::memcpy(slot + sizeof(Header), message.payload.data(), payloadSize_);

  return ring_.CommitWrite();
}

bool SharedMemoryTransport::Receive(Message& message) {
  std::byte* slot = ring_.AcquireReadSlot();
  if (slot == nullptr) {
    return false;
  }

  std::memcpy(&message.header, slot, sizeof(Header));
  if (message.header.payloadSize != payloadSize_) {
    return false;
  }
  std::memcpy(message.payload.data(), slot + sizeof(Header), payloadSize_);

  return ring_.CommitRead();
}

SharedMemoryTransport::SharedMemoryTransport(BlockingRingBuffer ring, std::size_t payloadSize)
    : ring_(std::move(ring)), payloadSize_(payloadSize) {}

}  // namespace ipc::common
