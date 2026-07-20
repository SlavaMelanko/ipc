#ifndef IPC_COMMON_MESSAGE_CHECKSUM_H_
#define IPC_COMMON_MESSAGE_CHECKSUM_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "common/message/header.h"

namespace ipc::common {

// CRC32 (IEEE 802.3 polynomial), computed over arbitrary byte ranges.
std::uint32_t Crc32(std::span<const std::byte> data);

// Crc32() over header's sessionId/timestamp/sequenceNumber/payloadSize +
// payload -- header.checksum itself is excluded from its own input.
std::uint32_t ComputeChecksum(const Header& header, std::span<const std::byte> payload);

}  // namespace ipc::common

#endif  // IPC_COMMON_MESSAGE_CHECKSUM_H_
