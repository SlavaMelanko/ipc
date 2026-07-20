#include "producer/app.h"

#include <cstddef>
#include <print>
#include <stdexcept>
#include <vector>

#include "common/message/checksum.h"
#include "common/message/message.h"
#include "common/transport/shared_memory_transport.h"
#include "common/util/clock.h"
#include "common/util/rand.h"
#include "producer/config.h"
#include "producer/deterministic_payload_generator.h"

namespace ipc::producer {

namespace {

CommandLineArgs ParseOrThrow(int argc, char** argv) {
  auto cmdArgs = ParseCommandLine(argc, argv);
  if (!cmdArgs) {
    throw std::runtime_error(cmdArgs.error());
  }

  return *cmdArgs;
}

}  // namespace

App::App(int argc, char** argv) : cmdArgs_(ParseOrThrow(argc, argv)) {}

bool App::Run() const {
  std::uint64_t count = cmdArgs_.count;

  auto transport = ipc::common::SharedMemoryTransport::CreateProducer(Config::payloadSize,
                                                                      Config::ringCapacityBytes);
  if (!transport) {
    std::println(stderr, "producer: failed to create shared-memory segment");

    return false;
  }

  std::vector<std::byte> payloadBuffer(Config::payloadSize);
  const auto sessionId = ipc::common::RandomNumber<std::uint64_t>();
  DeterministicPayloadGenerator payloadGenerator;

  for (std::uint64_t sequenceNumber = 0; sequenceNumber < count; ++sequenceNumber) {
    payloadGenerator.Generate(payloadBuffer, sequenceNumber);

    ipc::common::Header header{.sessionId = sessionId,
                               .timestamp = ipc::common::CurrentTimestamp(),
                               .sequenceNumber = sequenceNumber,
                               .payloadSize = static_cast<std::uint32_t>(Config::payloadSize),
                               .checksum = 0};
    header.checksum = ComputeChecksum(header, payloadBuffer);

    ipc::common::Message message{.header = header, .payload = payloadBuffer};

    if (!transport->Send(message)) {
      std::println(stderr, "producer: send failed at sequenceNumber={}", sequenceNumber);

      return false;
    }
  }

  transport->Close();
  std::println("producer: sent {} messages", count);

  return true;
}

}  // namespace ipc::producer
