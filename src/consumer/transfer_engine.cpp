#include "consumer/transfer_engine.h"

#include <utility>

namespace ipc::consumer {

TransferEngine::TransferEngine(std::unique_ptr<ipc::common::ITransport> transport,
                               ipc::common::MessageValidator validator, std::size_t payloadSize)
    : transport_(std::move(transport)), validator_(validator), payloadBuffer_(payloadSize) {}

ReceivedMessage TransferEngine::ReceiveNext() {
  ipc::common::Message message{.header = {}, .payload = payloadBuffer_};

  if (!transport_) {
    return {.result = ipc::common::ReceiveResult::kEndOfStream, .message = message};
  }

  auto result = transport_->Receive(message);
  if (result != ipc::common::ReceiveResult::kReceived) {
    return {.result = result, .message = message};
  }

  validator_.Validate(message);

  ++receivedCount_;

  return {.result = result, .message = message};
}

}  // namespace ipc::consumer
