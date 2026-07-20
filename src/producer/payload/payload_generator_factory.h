#ifndef IPC_PRODUCER_PAYLOAD_PAYLOAD_GENERATOR_FACTORY_H_
#define IPC_PRODUCER_PAYLOAD_PAYLOAD_GENERATOR_FACTORY_H_

#include <memory>

#include "producer/payload/payload_generator.h"

namespace ipc::producer {

// Only one IPayloadGenerator implementation exists, so this always returns a
// DeterministicPayloadGenerator -- no selection logic until a second one
// shows up.
std::unique_ptr<IPayloadGenerator> CreatePayloadGenerator();

}  // namespace ipc::producer

#endif  // IPC_PRODUCER_PAYLOAD_PAYLOAD_GENERATOR_FACTORY_H_
