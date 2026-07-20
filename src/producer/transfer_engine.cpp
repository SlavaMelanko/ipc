#include "producer/transfer_engine.h"

#include <utility>

#include "common/message/checksum.h"
#include "common/message/message.h"
#include "common/util/clock.h"

namespace ipc::producer {

TransferEngine::TransferEngine(std::unique_ptr<ipc::common::ITransport> transport,
                               std::unique_ptr<IPayloadGenerator> generator,
                               std::size_t payloadSize, std::uint64_t sessionId)
    : transport_(std::move(transport)),
      generator_(std::move(generator)),
      payloadSize_(payloadSize),
      sessionId_(sessionId),
      payloadBuffer_(payloadSize) {}

bool TransferEngine::SendNext() {
  if (!transport_) {
    return false;
  }

  if (!transport_->Send(PrepareMessage())) {
    return false;
  }

  ++sequenceNumber_;

  return true;
}

ipc::common::Message TransferEngine::PrepareMessage() {
  generator_->Generate(payloadBuffer_, sequenceNumber_);

  ipc::common::Header header{.sessionId = sessionId_,
                             .timestamp = ipc::common::CurrentTimestamp(),
                             .sequenceNumber = sequenceNumber_,
                             .payloadSize = static_cast<std::uint32_t>(payloadSize_),
                             .checksum = 0};
  header.checksum = ComputeChecksum(header, payloadBuffer_);

  return {.header = header, .payload = payloadBuffer_};
}

}  // namespace ipc::producer
