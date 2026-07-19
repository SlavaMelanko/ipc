// v1 end-to-end test (see AGENTS.md's "Testing strategy"): drives a real
// producer and consumer against the same shared-memory ring, in-process on
// two threads, and asserts every message arrives exactly once, in order.

#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "common/message/message.h"
#include "common/transport/shared_memory_transport.h"

namespace {

constexpr std::size_t kPayloadSize = 64;
constexpr std::size_t kRingCapacityBytes = std::size_t{8} * 1024;
constexpr std::uint64_t kMessageCount = 20000;

// v1 has no stale-resource recovery (that's v2 scope, see AGENTS.md's "Stale
// segments after a crash") -- CreateProducer's shm_open(O_CREAT|O_EXCL) fails
// outright if a prior run (crashed, killed, or asserted) left the segment or
// either semaphore behind. A test suite must still be repeatable regardless
// of iteration, so this test unlinks its own well-known names up front,
// exactly like scripts/clean-shm.sh does for a manual run.
void CleanStaleResources() {
  shm_unlink(ipc::common::SharedMemoryTransport::kSegmentName);
  sem_unlink("/ipc_ring_v1_free");
  sem_unlink("/ipc_ring_v1_avail");
}

bool RunProducer() {
  auto transport =
      ipc::common::SharedMemoryTransport::CreateProducer(kPayloadSize, kRingCapacityBytes);
  assert(transport != nullptr);

  std::vector<std::byte> payload(kPayloadSize);
  for (std::uint64_t sequenceNumber = 0; sequenceNumber < kMessageCount; ++sequenceNumber) {
    ipc::common::Message message{
        .header = {.sequenceNumber = sequenceNumber,
                   .payloadSize = static_cast<std::uint32_t>(kPayloadSize)},
        .payload = payload};

    if (!transport->Send(message)) {
      return false;
    }
  }

  return true;
}

bool RunConsumer() {
  auto transport =
      ipc::common::SharedMemoryTransport::AttachConsumer(kPayloadSize, kRingCapacityBytes);
  assert(transport != nullptr);

  std::vector<std::byte> payload(kPayloadSize);
  for (std::uint64_t expectedSequenceNumber = 0; expectedSequenceNumber < kMessageCount;
       ++expectedSequenceNumber) {
    ipc::common::Message message{.header = {}, .payload = payload};

    if (!transport->Receive(message)) {
      return false;
    }
    if (message.header.sequenceNumber != expectedSequenceNumber) {
      return false;
    }
  }

  return true;
}

}  // namespace

int main() {
  CleanStaleResources();

  bool producerOk = false;
  bool consumerOk = false;

  std::thread producerThread([&producerOk] { producerOk = RunProducer(); });

  // v1 has no attach-retry (see AGENTS.md): give the producer a head start to
  // create the segment before the consumer attaches.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::thread consumerThread([&consumerOk] { consumerOk = RunConsumer(); });

  producerThread.join();
  consumerThread.join();

  assert(producerOk);
  assert(consumerOk);

  return 0;
}
