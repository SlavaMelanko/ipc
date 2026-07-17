#ifndef IPC_COMMON_TRANSPORT_TRANSPORT_H_
#define IPC_COMMON_TRANSPORT_TRANSPORT_H_

#include "common/message/message.h"

namespace ipc::common {

class ITransport {
 public:
  virtual ~ITransport() = default;

  virtual bool Send(const Message& message) = 0;
  virtual bool Receive(Message& message) = 0;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_TRANSPORT_H_
