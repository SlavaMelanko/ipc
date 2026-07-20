#ifndef IPC_CONSUMER_TRANSFER_ENGINE_H_
#define IPC_CONSUMER_TRANSFER_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/message/message_validator.h"
#include "common/transport/transport.h"

namespace ipc::consumer {

// Owns everything needed to receive a sequenced stream: the transport,
// validator, payload buffer, and receivedCount. App owns argv/logging/exit
// codes; this owns everything below "receive and validate the next message".
class TransferEngine {
 public:
  TransferEngine(std::unique_ptr<ipc::common::ITransport> transport,
                 ipc::common::MessageValidator validator, std::size_t payloadSize);

  // Blocks for the next message and validates it on success. kEndOfStream on
  // a null transport, matching how other non-kReceived/kMalformed failures
  // are folded in until Controller exists (see AGENTS.md's v2 build order).
  ipc::common::ReceiveResult ReceiveNext();

  [[nodiscard]] std::uint64_t ReceivedCount() const { return receivedCount_; }
  [[nodiscard]] std::uint64_t ErrorCount() const { return validator_.ErrorCount(); }

 private:
  std::unique_ptr<ipc::common::ITransport> transport_;
  ipc::common::MessageValidator validator_;
  std::uint64_t receivedCount_ = 0;
  std::vector<std::byte> payloadBuffer_;
};

}  // namespace ipc::consumer

#endif  // IPC_CONSUMER_TRANSFER_ENGINE_H_
