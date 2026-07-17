#ifndef IPC_COMMON_MESSAGE_MESSAGE_H_
#define IPC_COMMON_MESSAGE_MESSAGE_H_

#include <cstddef>
#include <span>

#include "common/message/header.h"

namespace ipc::common {

struct Message {
  Header header;
  std::span<std::byte> payload;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_MESSAGE_MESSAGE_H_
