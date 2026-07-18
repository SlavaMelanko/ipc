#include "common/transport/shared_memory_transport.h"

#include <cstring>
#include <print>

#include "common/transport/ring_layout.h"
#include "common/util/scope_exit.h"

namespace ipc::common {

std::unique_ptr<ITransport> SharedMemoryTransport::CreateProducer(
    std::size_t payloadSize, std::size_t ringCapacityBytes) {
  std::size_t slotCount = SlotCount(ringCapacityBytes, payloadSize);
  if (slotCount == 0) {
    return nullptr;
  }

  auto segment = MappedSegment::Create(kSegmentName, ringCapacityBytes);
  if (!segment) {
    return nullptr;
  }

  auto* control = static_cast<ControlBlock*>(segment->data());
  if (!ControlBlock::Init(*control)) {
    return nullptr;
  }

  return std::unique_ptr<SharedMemoryTransport>(
      new SharedMemoryTransport(std::move(*segment), payloadSize, slotCount));
}

std::unique_ptr<ITransport> SharedMemoryTransport::AttachConsumer(
    std::size_t payloadSize, std::size_t ringCapacityBytes) {
  std::size_t slotCount = SlotCount(ringCapacityBytes, payloadSize);
  if (slotCount == 0) {
    return nullptr;
  }

  auto segment = MappedSegment::Attach(kSegmentName, ringCapacityBytes);
  if (!segment) {
    return nullptr;
  }

  return std::unique_ptr<SharedMemoryTransport>(
      new SharedMemoryTransport(std::move(*segment), payloadSize, slotCount));
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
  if (pthread_mutex_lock(&control.mutex) != 0) {
    return false;
  }
  ScopeExit unlock(
      [&control]() noexcept { pthread_mutex_unlock(&control.mutex); });

  bool isFull = control.writeCursor - control.readCursor == slotCount_;
  if (isFull) {
    // No StatsReporter until v2 -- without this, a full ring with no
    // consumer draining it looks identical to a genuine hang.
    std::println(stderr,
                 "producer: ring full, blocking until consumer drains it");
  }
  while (isFull) {
    if (pthread_cond_wait(&control.slotFreeCond, &control.mutex) != 0) {
      return false;
    }
    isFull = control.writeCursor - control.readCursor == slotCount_;
  }

  std::byte* slot = SlotAt(control.writeCursor);
  std::memcpy(slot, &message.header, sizeof(Header));
  std::memcpy(slot + sizeof(Header), message.payload.data(), payloadSize_);
  ++control.writeCursor;

  pthread_cond_signal(&control.messageAvailableCond);

  return true;
}

bool SharedMemoryTransport::Receive(Message& message) {
  ControlBlock& control = Control();
  if (pthread_mutex_lock(&control.mutex) != 0) {
    return false;
  }
  ScopeExit unlock(
      [&control]() noexcept { pthread_mutex_unlock(&control.mutex); });

  bool isEmpty = control.writeCursor == control.readCursor;
  if (isEmpty) {
    // No StatsReporter until v2 -- without this, an empty ring with no
    // producer sending looks identical to a genuine hang.
    std::println(stderr, "consumer: ring empty, waiting for producer");
  }
  while (isEmpty) {
    if (pthread_cond_wait(&control.messageAvailableCond, &control.mutex) != 0) {
      return false;
    }
    isEmpty = control.writeCursor == control.readCursor;
  }

  std::byte* slot = SlotAt(control.readCursor);
  std::memcpy(&message.header, slot, sizeof(Header));
  if (message.header.payloadSize != payloadSize_) {
    return false;
  }
  std::memcpy(message.payload.data(), slot + sizeof(Header), payloadSize_);
  ++control.readCursor;

  pthread_cond_signal(&control.slotFreeCond);

  return true;
}

}  // namespace ipc::common
