#ifndef IPC_COMMON_MESSAGE_HEADER_H_
#define IPC_COMMON_MESSAGE_HEADER_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ipc::common {

struct Header {
  // Doesn't provide ordering (the ring already guarantees that) — lets the
  // consumer detect a gap or reorder as a bug, not routine data.
  std::uint64_t sequenceNumber;
  std::uint32_t payloadSize;
};

// Producer and consumer compile Header independently and copy it raw across
// the shared-memory boundary, so both must agree on its exact byte layout.
static_assert(std::is_trivially_copyable_v<Header>);
static_assert(sizeof(Header) == 16);
static_assert(offsetof(Header, sequenceNumber) == 0);
static_assert(offsetof(Header, payloadSize) == 8);

}  // namespace ipc::common

#endif  // IPC_COMMON_MESSAGE_HEADER_H_
