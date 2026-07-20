#include "consumer/app.h"

#include <cstddef>
#include <print>
#include <stdexcept>
#include <vector>

#include "common/message/message.h"
#include "common/message/message_validator.h"
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
  std::uint64_t receivedCount = 0;
  ipc::common::MessageValidator validator;

  for (;;) {
    ipc::common::Message message{.header = {}, .payload = payloadBuffer};

    auto result = transport->Receive(message);
    if (result == ipc::common::ReceiveResult::kEndOfStream) {
      break;
    }
    if (result == ipc::common::ReceiveResult::kMalformed) {
      std::println(stderr, "consumer: malformed frame at receivedCount={}", receivedCount);

      return false;
    }

    validator.Validate(message);
    ++receivedCount;
  }

  if (receivedCount != cmdArgs_.count) {
    std::println(stderr, "consumer: expected {} messages, received {}", cmdArgs_.count,
                 receivedCount);

    return false;
  }

  if (validator.ErrorCount() != 0) {
    std::println(stderr, "consumer: {} defect(s) detected", validator.ErrorCount());

    return false;
  }

  std::println("consumer: received {} messages", receivedCount);

  return true;
}

}  // namespace ipc::consumer
