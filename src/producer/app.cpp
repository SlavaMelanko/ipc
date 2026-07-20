#include "producer/app.h"

#include <print>
#include <stdexcept>
#include <utility>

#include "common/transport/shared_memory_transport.h"
#include "common/util/rand.h"
#include "producer/config.h"
#include "producer/payload/payload_generator_factory.h"
#include "producer/transfer_engine.h"

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

App::App(int argc, char** argv)
    : cmdArgs_(ParseOrThrow(argc, argv)), sessionId_(ipc::common::RandomNumber<std::uint64_t>()) {}

bool App::Run() const {
  auto transport = ipc::common::SharedMemoryTransport::CreateProducer(Config::payloadSize,
                                                                      Config::ringCapacityBytes);
  if (!transport) {
    std::println(stderr, "producer: failed to create shared-memory segment");

    return false;
  }

  TransferEngine engine(std::move(transport), CreatePayloadGenerator(), Config::payloadSize,
                        sessionId_);

  while (engine.SentCount() < cmdArgs_.count) {
    if (!engine.SendNext()) {
      std::println(stderr, "producer: send failed at sequenceNumber={}", engine.SentCount());

      return false;
    }
  }

  engine.Close();

  std::println("producer: sent {} messages", engine.SentCount());

  return true;
}

}  // namespace ipc::producer
