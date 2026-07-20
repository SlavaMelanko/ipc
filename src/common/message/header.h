#ifndef IPC_COMMON_MESSAGE_HEADER_H_
#define IPC_COMMON_MESSAGE_HEADER_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ipc::common {

struct Header {
  // Lets the consumer tell a producer restart apart from real loss.
  std::uint64_t sessionId;
  std::uint64_t timestamp;
  // Ring already guarantees order; this only lets a gap be detected as a bug.
  std::uint64_t sequenceNumber;
  std::uint32_t payloadSize;
  // Crc32() over the other fields + payload.
  std::uint32_t checksum;
};

// Copied raw across the shared-memory boundary, so layout must be exact.
static_assert(std::is_trivially_copyable_v<Header>);
static_assert(sizeof(Header) == 32);
static_assert(offsetof(Header, sessionId) == 0);
static_assert(offsetof(Header, timestamp) == 8);
static_assert(offsetof(Header, sequenceNumber) == 16);
static_assert(offsetof(Header, payloadSize) == 24);
static_assert(offsetof(Header, checksum) == 28);

}  // namespace ipc::common

#endif  // IPC_COMMON_MESSAGE_HEADER_H_
