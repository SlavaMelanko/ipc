#include "consumer/transfer_engine.h"

#include <utility>

#include "common/message/message.h"

namespace ipc::consumer {

TransferEngine::TransferEngine(std::unique_ptr<ipc::common::ITransport> transport,
                               ipc::common::MessageValidator& validator, std::size_t payloadSize)
    : transport_(std::move(transport)), validator_(validator), payloadBuffer_(payloadSize) {}

ipc::common::ReceiveResult TransferEngine::ReceiveNext() {
  if (!transport_) {
    return ipc::common::ReceiveResult::kEndOfStream;
  }

  ipc::common::Message message{.header = {}, .payload = payloadBuffer_};

  auto result = transport_->Receive(message);
  if (result != ipc::common::ReceiveResult::kReceived) {
    return result;
  }

  validator_.Validate(message);
  ++receivedCount_;

  return result;
}

}  // namespace ipc::consumer
