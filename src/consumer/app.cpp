#include "consumer/app.h"

#include <print>
#include <stdexcept>
#include <utility>

#include "common/control/control_panel.h"
#include "common/message/header.h"
#include "common/message/message_validator.h"
#include "common/transport/shared_memory_transport.h"
#include "consumer/config.h"
#include "consumer/stats_reporter.h"
#include "consumer/transfer_engine.h"

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

  TransferEngine engine(std::move(transport), ipc::common::MessageValidator(), Config::payloadSize);

  ipc::common::ControlPanel controlPanel;
  StatsReporter statsReporter;

  for (;;) {
    if (!controlPanel.WaitIfPaused()) {
      break;
    }

    auto [result, message] = engine.ReceiveNext();
    if (result == ipc::common::ReceiveResult::kEndOfStream) {
      break;
    }
    if (result == ipc::common::ReceiveResult::kMalformed) {
      std::println(stderr, "consumer: malformed frame at receivedCount={}", engine.ReceivedCount());

      return false;
    }

    statsReporter.RecordMessage(sizeof(ipc::common::Header) + message.header.payloadSize);
  }

  if (engine.ReceivedCount() != cmdArgs_.count) {
    std::println(stderr, "consumer: expected {} messages, received {}", cmdArgs_.count,
                 engine.ReceivedCount());

    return false;
  }

  if (engine.ErrorCount() != 0) {
    std::println(stderr, "consumer: {} defect(s) detected", engine.ErrorCount());

    return false;
  }

  std::println("consumer: received {} messages", engine.ReceivedCount());

  return true;
}

}  // namespace ipc::consumer
