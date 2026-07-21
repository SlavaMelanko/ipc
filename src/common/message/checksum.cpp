#include "common/message/checksum.h"

#include <zlib.h>

#include <array>
#include <cstring>

namespace ipc::common {

namespace {

std::uint32_t Crc32(std::uint32_t crc, std::span<const std::byte> data) {
  return static_cast<std::uint32_t>(
      crc32(crc, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
}

}  // namespace

std::uint32_t ComputeChecksum(const Header& header, std::span<const std::byte> payload) {
  std::array<std::byte, offsetof(Header, checksum)> headerBytes;
  std::memcpy(headerBytes.data(), &header, headerBytes.size());

  std::uint32_t crc = Crc32(0, headerBytes);
  crc = Crc32(crc, payload);

  return crc;
}

}  // namespace ipc::common
