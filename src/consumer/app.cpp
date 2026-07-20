#include "consumer/app.h"

#include <cstddef>
#include <print>
#include <stdexcept>
#include <vector>

#include "common/message/message.h"
#include "common/transport/shared_memory_transport.h"
#include "consumer/config.h"

namespace ipc::consumer {

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
  auto transport = ipc::common::SharedMemoryTransport::AttachConsumer(Config::payloadSize,
                                                                      Config::ringCapacityBytes);
  if (!transport) {
    std::println(stderr, "consumer: failed to attach to shared-memory segment");

    return false;
  }

  std::vector<std::byte> payloadBuffer(Config::payloadSize);
  std::uint64_t expectedSequenceNumber = 0;

  for (;;) {
    ipc::common::Message message{.header = {}, .payload = payloadBuffer};

    auto result = transport->Receive(message);
    if (result == ipc::common::ReceiveResult::kEndOfStream) {
      break;
    }
    if (result == ipc::common::ReceiveResult::kMalformed) {
      std::println(stderr, "consumer: malformed frame at sequenceNumber={}",
                   expectedSequenceNumber);

      return false;
    }

    if (message.header.sequenceNumber != expectedSequenceNumber) {
      std::println(stderr, "consumer: sequence gap, expected={} actual={}", expectedSequenceNumber,
                   message.header.sequenceNumber);

      return false;
    }

    ++expectedSequenceNumber;
  }

  if (expectedSequenceNumber != cmdArgs_.count) {
    std::println(stderr, "consumer: expected {} messages, received {}", cmdArgs_.count,
                 expectedSequenceNumber);

    return false;
  }

  std::println("consumer: received {} messages", expectedSequenceNumber);

  return true;
}

}  // namespace ipc::consumer
