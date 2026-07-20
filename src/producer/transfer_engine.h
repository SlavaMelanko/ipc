#ifndef IPC_PRODUCER_TRANSFER_ENGINE_H_
#define IPC_PRODUCER_TRANSFER_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/message/message.h"
#include "common/transport/transport.h"
#include "producer/payload/payload_generator.h"

namespace ipc::producer {

// Owns everything needed to send a sequenced stream: the transport,
// generator, payload buffer, and sequenceNumber. App owns argv/logging/exit
// codes; this owns everything below "build and send the next message".
class TransferEngine {
 public:
  TransferEngine(std::unique_ptr<ipc::common::ITransport> transport,
                 std::unique_ptr<IPayloadGenerator> generator, std::size_t payloadSize,
                 std::uint64_t sessionId);

  // Builds and sends the next sequenced message. False on a null/failed
  // transport.
  bool SendNext();

  void Close() {
    if (transport_) {
      transport_->Close();
    }
  }

  [[nodiscard]] std::uint64_t SentCount() const { return sequenceNumber_; }

 private:
  ipc::common::Message PrepareMessage();

  std::unique_ptr<ipc::common::ITransport> transport_;
  std::unique_ptr<IPayloadGenerator> generator_;
  std::size_t payloadSize_;
  std::uint64_t sessionId_;
  std::uint64_t sequenceNumber_ = 0;
  std::vector<std::byte> payloadBuffer_;
};

}  // namespace ipc::producer

#endif  // IPC_PRODUCER_TRANSFER_ENGINE_H_
