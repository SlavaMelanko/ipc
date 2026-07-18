#include "common/transport/shared_memory_transport.h"

#include <cstring>

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

bool SharedMemoryTransport::Send(const Message& message) {
  if (message.header.payloadSize != payloadSize_) {
    return false;
  }

  ControlBlock& control = Control();
  pthread_mutex_lock(&control.mutex);

  while (control.writeCursor - control.readCursor == slotCount_) {  // is full
    pthread_cond_wait(&control.slotFreeCond, &control.mutex);
  }

  std::byte* slot = SlotAt(control.writeCursor);
  std::memcpy(slot, &message.header, sizeof(Header));
  std::memcpy(slot + sizeof(Header), message.payload.data(), payloadSize_);
  ++control.writeCursor;

  pthread_cond_signal(&control.messageAvailableCond);
  pthread_mutex_unlock(&control.mutex);

  return true;
}

bool SharedMemoryTransport::Receive(Message& message) {
  ControlBlock& control = Control();
  pthread_mutex_lock(&control.mutex);

  while (control.writeCursor == control.readCursor) {  // is empty
    pthread_cond_wait(&control.messageAvailableCond, &control.mutex);
  }

  std::byte* slot = SlotAt(control.readCursor);
  std::memcpy(&message.header, slot, sizeof(Header));
  if (message.header.payloadSize != payloadSize_) {
    pthread_mutex_unlock(&control.mutex);

    return false;
  }
  std::memcpy(message.payload.data(), slot + sizeof(Header), payloadSize_);
  ++control.readCursor;

  pthread_cond_signal(&control.slotFreeCond);
  pthread_mutex_unlock(&control.mutex);

  return true;
}

}  // namespace ipc::common
