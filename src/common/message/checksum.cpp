#include "common/message/checksum.h"

#include <array>
#include <cstring>
#include <vector>

namespace ipc::common {

namespace {

constexpr std::uint32_t kPolynomial = 0xEDB88320;

constexpr std::array<std::uint32_t, 256> BuildTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) != 0 ? (crc >> 1) ^ kPolynomial : crc >> 1;
    }
    table[i] = crc;
  }

  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = BuildTable();

}  // namespace

std::uint32_t Crc32(std::span<const std::byte> data) {
  std::uint32_t crc = 0xFFFFFFFF;
  for (std::byte b : data) {
    crc = kTable[(crc ^ static_cast<std::uint8_t>(b)) & 0xFF] ^ (crc >> 8);
  }

  return crc ^ 0xFFFFFFFF;
}

std::uint32_t ComputeChecksum(const Header& header, std::span<const std::byte> payload) {
  std::array<std::byte, offsetof(Header, checksum)> headerBytes;
  std::memcpy(headerBytes.data(), &header, headerBytes.size());

  std::vector<std::byte> buffer(headerBytes.begin(), headerBytes.end());
  buffer.insert(buffer.end(), payload.begin(), payload.end());

  return Crc32(buffer);
}

}  // namespace ipc::common
