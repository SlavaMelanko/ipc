#include "producer/app.h"

#include <cstddef>
#include <print>
#include <stdexcept>
#include <vector>

#include "common/message/message.h"
#include "common/transport/shared_memory_transport.h"
#include "producer/config.h"

namespace ipc::producer {

namespace {

CommandLineArgs ParseOrThrow(int argc, char** argv) {
  auto cmdArgs = ParseCommandLine(argc, argv);
  if (!cmdArgs) {
    throw std::runtime_error(cmdArgs.error());
  }

  return *cmdArgs;
}

// No IPayloadGenerator interface in v1 (see AGENTS.md) -- one implementation
// doesn't justify one yet. Deterministic pattern lets a consumer/test verify
// payload bytes, not just the header.
void FillPayload(std::span<std::byte> payload, std::uint64_t sequenceNumber) {
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::byte>((sequenceNumber + i) & 0xFF);
  }
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

  for (std::uint64_t sequenceNumber = 0; sequenceNumber < count; ++sequenceNumber) {
    FillPayload(payloadBuffer, sequenceNumber);

    ipc::common::Message message{
        .header = {.sequenceNumber = sequenceNumber,
                   .payloadSize = static_cast<std::uint32_t>(Config::payloadSize)},
        .payload = payloadBuffer};

    if (!transport->Send(message)) {
      std::println(stderr, "producer: send failed at sequenceNumber={}", sequenceNumber);

      return false;
    }
  }

  std::println("producer: sent {} messages", count);

  return true;
}

}  // namespace ipc::producer
