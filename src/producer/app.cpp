#include "producer/app.h"

#include <print>
#include <utility>

#include "common/control/control_panel.h"
#include "common/transport/shm/shared_memory_transport.h"
#include "common/util/rand.h"
#include "producer/payload/payload_generator_factory.h"
#include "producer/transfer_engine.h"

namespace ipc::producer {

App::App(ipc::common::AppConfig config)
    : config_(config), sessionId_(ipc::common::RandomNumber<std::uint64_t>()) {}

bool App::Run() const {
  auto transport = ipc::common::SharedMemoryTransport::CreateProducer(config_.payloadSize,
                                                                      config_.ringCapacityBytes);
  if (!transport) {
    std::println(stderr, "producer: failed to create shared-memory segment");

    return false;
  }

  TransferEngine engine(std::move(transport), CreatePayloadGenerator(), config_.payloadSize,
                        sessionId_);

  ipc::common::ControlPanel controlPanel;

  while (engine.SentCount() < config_.count) {
    if (!controlPanel.WaitIfPaused()) {
      break;
    }

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
