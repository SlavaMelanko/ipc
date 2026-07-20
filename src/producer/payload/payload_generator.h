#ifndef IPC_PRODUCER_PAYLOAD_PAYLOAD_GENERATOR_H_
#define IPC_PRODUCER_PAYLOAD_PAYLOAD_GENERATOR_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace ipc::producer {

class IPayloadGenerator {
 public:
  virtual ~IPayloadGenerator() = default;

  virtual void Generate(std::span<std::byte> payload, std::uint64_t sequenceNumber) = 0;
};

}  // namespace ipc::producer

#endif  // IPC_PRODUCER_PAYLOAD_PAYLOAD_GENERATOR_H_
