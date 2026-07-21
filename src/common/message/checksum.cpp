#include "common/message/checksum.h"

#include <array>
#include <cstring>

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

// Runs the CRC over data starting from crc's current (pre-inversion) state,
// so multiple buffers can be fed through in sequence -- crc is finalized
// (inverted) only once, by the caller, after the last buffer.
std::uint32_t Crc32Update(std::uint32_t crc, std::span<const std::byte> data) {
  for (std::byte b : data) {
    crc = kTable[(crc ^ static_cast<std::uint8_t>(b)) & 0xFF] ^ (crc >> 8);
  }

  return crc;
}

}  // namespace

std::uint32_t Crc32(std::span<const std::byte> data) {
  return Crc32Update(0xFFFFFFFF, data) ^ 0xFFFFFFFF;
}

std::uint32_t ComputeChecksum(const Header& header, std::span<const std::byte> payload) {
  std::array<std::byte, offsetof(Header, checksum)> headerBytes;
  std::memcpy(headerBytes.data(), &header, headerBytes.size());

  std::uint32_t crc = Crc32Update(0xFFFFFFFF, headerBytes);
  crc = Crc32Update(crc, payload);

  return crc ^ 0xFFFFFFFF;
}

}  // namespace ipc::common
