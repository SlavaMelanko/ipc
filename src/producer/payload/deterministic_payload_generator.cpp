#include "producer/payload/deterministic_payload_generator.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace ipc::producer {

namespace {

// The pattern repeats every 256 bytes; any Generate() output is a window
// into this doubled table starting at (sequenceNumber & 0xFF).
constexpr std::size_t kPeriod = 256;

constexpr auto kPattern = [] {
  std::array<std::byte, kPeriod * 2> table{};
  for (std::size_t i = 0; i < table.size(); ++i) {
    table[i] = static_cast<std::byte>(i & 0xFF);
  }
  return table;
}();

}  // namespace

void DeterministicPayloadGenerator::Generate(std::span<std::byte> payload,
                                             std::uint64_t sequenceNumber) {
  std::size_t filled = std::min(payload.size(), kPeriod);
  std::memcpy(payload.data(), kPattern.data() + (sequenceNumber % kPeriod), filled);

  while (filled < payload.size()) {
    std::memcpy(payload.data() + filled, payload.data(), std::min(filled, payload.size() - filled));
    filled *= 2;
  }
}

}  // namespace ipc::producer
