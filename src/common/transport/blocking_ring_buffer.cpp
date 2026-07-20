#include "common/transport/blocking_ring_buffer.h"

#include <sys/mman.h>

#include <utility>

#include "common/transport/control_block.h"
#include "common/transport/ring_layout.h"
#include "common/util/posix.h"
#include "common/util/process.h"

namespace ipc::common {

namespace {

// Kept short: macOS caps semaphore names well under shm_open's 255 bytes.
std::string FreeSlotsName(const std::string& name) { return name + "_free"; }
std::string AvailableMessagesName(const std::string& name) { return name + "_avail"; }

// True only if another producer is genuinely still running; otherwise
// unlinks any stale segment left behind and returns false. Attaches with
// sizeof(ControlBlock), not ringCapacityBytes, so a stale segment from a
// differently-sized run is still detected.
bool RefuseIfLive(const std::string& name) {
  auto existing = MappedSegment::Attach(name, sizeof(ControlBlock));
  if (!existing) {
    return false;
  }

  auto* control = static_cast<ControlBlock*>(existing->data());
  auto state = static_cast<LifecycleState>(control->state.load(std::memory_order_acquire));
  std::int32_t producerPid = control->producerPid;

  bool looksLive = state == LifecycleState::kInitializing || state == LifecycleState::kReady ||
                   state == LifecycleState::kStopping;
  if (looksLive && IsProcessAlive(producerPid)) {
    return true;
  }

  existing.reset();
  shm_unlink(name.c_str());

  return false;
}

}  // namespace

std::optional<BlockingRingBuffer> BlockingRingBuffer::Create(const std::string& name,
                                                             std::size_t ringCapacityBytes,
                                                             std::size_t payloadSize) {
  std::size_t slotCount = SlotCount(ringCapacityBytes, payloadSize);
  if (slotCount == 0) {
    return std::nullopt;
  }

  if (RefuseIfLive(name)) {
    return std::nullopt;
  }

  auto segment = MappedSegment::Create(name, ringCapacityBytes);
  if (!segment) {
    return std::nullopt;
  }

  auto* control = static_cast<ControlBlock*>(segment->data());
  if (!InitControlBlock(*control, CurrentProcessId())) {
    return std::nullopt;
  }

  auto freeSlots = NamedSemaphore::Create(FreeSlotsName(name), static_cast<unsigned int>(slotCount),
                                          /*unlink=*/true);
  if (!freeSlots) {
    return std::nullopt;
  }

  auto availableMessages = NamedSemaphore::Create(AvailableMessagesName(name), 0, /*unlink=*/true);
  if (!availableMessages) {
    return std::nullopt;
  }

  PublishReady(*control, kWireFormatVersion);

  return BlockingRingBuffer(std::move(*segment), std::move(*freeSlots),
                            std::move(*availableMessages), payloadSize, slotCount);
}

std::optional<BlockingRingBuffer> BlockingRingBuffer::Attach(const std::string& name,
                                                             std::size_t ringCapacityBytes,
                                                             std::size_t payloadSize) {
  std::size_t slotCount = SlotCount(ringCapacityBytes, payloadSize);
  if (slotCount == 0) {
    return std::nullopt;
  }

  auto segment = MappedSegment::Attach(name, ringCapacityBytes);
  if (!segment) {
    return std::nullopt;
  }

  auto freeSlots = NamedSemaphore::Attach(FreeSlotsName(name));
  if (!freeSlots) {
    return std::nullopt;
  }

  auto availableMessages = NamedSemaphore::Attach(AvailableMessagesName(name));
  if (!availableMessages) {
    return std::nullopt;
  }

  return BlockingRingBuffer(std::move(*segment), std::move(*freeSlots),
                            std::move(*availableMessages), payloadSize, slotCount);
}

std::byte* BlockingRingBuffer::AcquireWriteSlot() {
  if (Failed(freeSlots_.Wait())) {
    return nullptr;
  }

  ControlBlock& control = Control();
  if (Failed(pthread_mutex_lock(&control.cursorMutex))) {
    return nullptr;
  }
  std::uint64_t writePos = control.writeCursor++;
  if (Failed(pthread_mutex_unlock(&control.cursorMutex))) {
    return nullptr;
  }

  return SlotAt(writePos);
}

bool BlockingRingBuffer::CommitWrite() { return Ok(availableMessages_.Post()); }

std::byte* BlockingRingBuffer::AcquireReadSlot() {
  if (Failed(availableMessages_.Wait())) {
    return nullptr;
  }

  ControlBlock& control = Control();
  if (Failed(pthread_mutex_lock(&control.cursorMutex))) {
    return nullptr;
  }
  std::uint64_t readPos = control.readCursor++;
  if (Failed(pthread_mutex_unlock(&control.cursorMutex))) {
    return nullptr;
  }

  return SlotAt(readPos);
}

bool BlockingRingBuffer::CommitRead() { return Ok(freeSlots_.Post()); }

BlockingRingBuffer::BlockingRingBuffer(MappedSegment segment, NamedSemaphore freeSlots,
                                       NamedSemaphore availableMessages, std::size_t payloadSize,
                                       std::size_t slotCount)
    : segment_(std::move(segment)),
      freeSlots_(std::move(freeSlots)),
      availableMessages_(std::move(availableMessages)),
      payloadSize_(payloadSize),
      slotCount_(slotCount) {}

ControlBlock& BlockingRingBuffer::Control() { return *static_cast<ControlBlock*>(segment_.data()); }

std::byte* BlockingRingBuffer::SlotAt(std::uint64_t index) {
  std::size_t stride = SlotStride(payloadSize_);
  auto* base = static_cast<std::byte*>(segment_.data()) + SlotAreaOffset();

  return base + ((index % slotCount_) * stride);
}

}  // namespace ipc::common
