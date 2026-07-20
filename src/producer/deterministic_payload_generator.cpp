#include "producer/deterministic_payload_generator.h"

namespace ipc::producer {

void DeterministicPayloadGenerator::Generate(std::span<std::byte> payload,
                                             std::uint64_t sequenceNumber) {
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::byte>((sequenceNumber + i) & 0xFF);
  }
}

}  // namespace ipc::producer
