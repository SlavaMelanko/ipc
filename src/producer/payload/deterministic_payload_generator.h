#ifndef IPC_PRODUCER_PAYLOAD_DETERMINISTIC_PAYLOAD_GENERATOR_H_
#define IPC_PRODUCER_PAYLOAD_DETERMINISTIC_PAYLOAD_GENERATOR_H_

#include "producer/payload/payload_generator.h"

namespace ipc::producer {

// payload[i] = (sequenceNumber + i) & 0xFF -- lets a consumer or test verify
// payload bytes, not just the header.
class DeterministicPayloadGenerator : public IPayloadGenerator {
 public:
  void Generate(std::span<std::byte> payload, std::uint64_t sequenceNumber) override;
};

}  // namespace ipc::producer

#endif  // IPC_PRODUCER_DETERMINISTIC_PAYLOAD_GENERATOR_H_
