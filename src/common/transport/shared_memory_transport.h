#ifndef IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_
#define IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_

#include <cstddef>
#include <memory>

#include "common/transport/blocking_ring_buffer.h"
#include "common/transport/transport.h"

namespace ipc::common {

class SharedMemoryTransport : public ITransport {
 public:
  static constexpr const char* kSegmentName = "/ipc_ring_v1";

  static std::unique_ptr<ITransport> CreateProducer(std::size_t payloadSize,
                                                    std::size_t ringCapacityBytes);
  static std::unique_ptr<ITransport> AttachConsumer(std::size_t payloadSize,
                                                    std::size_t ringCapacityBytes);

  SharedMemoryTransport(const SharedMemoryTransport&) = delete;
  SharedMemoryTransport& operator=(const SharedMemoryTransport&) = delete;
  ~SharedMemoryTransport() override = default;

  bool Send(const Message& message) override;
  bool Receive(Message& message) override;

 private:
  SharedMemoryTransport(BlockingRingBuffer ring, std::size_t payloadSize);

  BlockingRingBuffer ring_;
  std::size_t payloadSize_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_SHARED_MEMORY_TRANSPORT_H_
