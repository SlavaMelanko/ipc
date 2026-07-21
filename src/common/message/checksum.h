#ifndef IPC_COMMON_MESSAGE_CHECKSUM_H_
#define IPC_COMMON_MESSAGE_CHECKSUM_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "common/message/header.h"

namespace ipc::common {

// CRC32 (IEEE 802.3 polynomial) over header's
// sessionId/timestamp/sequenceNumber/payloadSize +
// payload -- header.checksum itself is excluded from its own input. Feeds
// the two spans through the CRC in sequence rather than concatenating them
// into one buffer first, since that buffer would otherwise be reallocated
// and copied on every call.
std::uint32_t ComputeChecksum(const Header& header, std::span<const std::byte> payload);

}  // namespace ipc::common

#endif  // IPC_COMMON_MESSAGE_CHECKSUM_H_
