#ifndef IPC_COMMON_TRANSPORT_TRANSPORT_H_
#define IPC_COMMON_TRANSPORT_TRANSPORT_H_

#include <cstdint>

#include "common/message/message.h"

namespace ipc::common {

// Stopped/PeerClosed wait for Controller.
enum class ReceiveResult : std::uint8_t {
  kReceived,
  kMalformed,
  kEndOfStream,
};

class ITransport {
 public:
  virtual ~ITransport() = default;

  virtual bool Send(const Message& message) = 0;
  virtual ReceiveResult Receive(Message& message) = 0;

  // Producer-only; no-op for a consumer's transport. Marks that no more
  // messages are coming so a blocked Receive() returns kEndOfStream.
  virtual void Close() {}
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_TRANSPORT_H_
