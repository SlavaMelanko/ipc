#include "producer/payload/payload_generator_factory.h"

#include "producer/payload/deterministic_payload_generator.h"

namespace ipc::producer {

std::unique_ptr<IPayloadGenerator> CreatePayloadGenerator() {
  return std::make_unique<DeterministicPayloadGenerator>();
}

}  // namespace ipc::producer
