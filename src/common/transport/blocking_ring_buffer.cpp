#include "common/transport/blocking_ring_buffer.h"

#include <sys/mman.h>

#include <chrono>
#include <cstdio>
#include <print>
#include <thread>
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
  LifecycleState state = StateManager(*control).GetState();
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

// Blocks until the segment reaches state == Ready, retrying indefinitely on
// ENOENT (producer not up yet) or state != Ready (still initializing) --
// neither is an error, since a consumer may legitimately start before its
// producer. Returns false only on a genuine layoutVersion mismatch, which
// is a hard failure, not something to retry past.
bool WaitUntilReady(const std::string& name) {
  bool loggedWaiting = false;

  for (;;) {
    auto existing = MappedSegment::Attach(name, sizeof(ControlBlock));
    if (existing) {
      auto* control = static_cast<ControlBlock*>(existing->data());

      if (StateManager(*control).GetState() == LifecycleState::kReady) {
        return control->layoutVersion == kWireFormatVersion;
      }
    }

    if (!loggedWaiting) {
      std::println(stderr, "consumer: waiting for producer to be ready");
      loggedWaiting = true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
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
  InitControlBlock(*control, CurrentProcessId());

  auto freeSlots = NamedSemaphore::Create(FreeSlotsName(name), static_cast<unsigned int>(slotCount),
                                          /*unlink=*/true);
  if (!freeSlots) {
    return std::nullopt;
  }

  auto availableMessages = NamedSemaphore::Create(AvailableMessagesName(name), 0, /*unlink=*/true);
  if (!availableMessages) {
    return std::nullopt;
  }

  StateManager(*control).Ready(kWireFormatVersion);

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

  if (!WaitUntilReady(name)) {
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

  // relaxed: freeSlots_/availableMessages_ sem_wait/sem_post already provide
  // the cross-process release/acquire fence around the slot bytes -- no
  // other process ever loads this cursor, so it needs no ordering of its own.
  std::uint64_t writePos = Control().writeCursor.fetch_add(1, std::memory_order_relaxed);

  return SlotAt(writePos);
}

bool BlockingRingBuffer::CommitWrite() { return Ok(availableMessages_.Post()); }

std::byte* BlockingRingBuffer::AcquireReadSlot() {
  lastReadFailure_ = ReadFailure::kNone;

  constexpr std::chrono::milliseconds kWaitBound{150};
  for (;;) {
    auto result = availableMessages_.WaitFor(kWaitBound);
    if (result == NamedSemaphore::WaitResult::kAcquired) {
      break;
    }
    if (result == NamedSemaphore::WaitResult::kError) {
      lastReadFailure_ = ReadFailure::kError;
      return nullptr;
    }

    // Timed out: no message yet. The producer finishing normally takes
    // priority over a liveness check -- see "Waking a blocked
    // send()/receive()" in AGENTS.md.
    if (StateManager(Control()).IsStoppingOrClosed()) {
      lastReadFailure_ = ReadFailure::kEndOfStream;
      return nullptr;
    }
    if (!IsProcessAlive(Control().producerPid)) {
      lastReadFailure_ = ReadFailure::kPeerClosed;
      return nullptr;
    }
  }

  // relaxed: same reasoning as AcquireWriteSlot() -- the semaphores carry
  // the fence, this cursor is never read cross-process.
  std::uint64_t readPos = Control().readCursor.fetch_add(1, std::memory_order_relaxed);

  return SlotAt(readPos);
}

bool BlockingRingBuffer::CommitRead() { return Ok(freeSlots_.Post()); }

void BlockingRingBuffer::Close() {
  // No availableMessages_.Post() here: that would let a consumer's
  // AcquireReadSlot() advance readCursor past a slot nothing ever wrote.
  // AcquireReadSlot()'s bounded wait already rechecks state on every
  // timeout, so EndOfStream is detected within one poll interval instead of
  // instantly.
  StateManager manager(Control());
  manager.Stop();
  manager.Close();
}

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
