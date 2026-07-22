#ifndef IPC_COMMON_TRANSPORT_SHM_SHARED_MEMORY_TRANSPORT_H_
#define IPC_COMMON_TRANSPORT_SHM_SHARED_MEMORY_TRANSPORT_H_

#include <cstddef>
#include <memory>

#include "common/transport/shm/blocking_ring_buffer.h"
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
  ReceiveResult Receive(Message& message) override;
  void Close() override;

 private:
  SharedMemoryTransport(BlockingRingBuffer ring, std::size_t payloadSize);

  BlockingRingBuffer ring_;
  std::size_t payloadSize_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_SHM_SHARED_MEMORY_TRANSPORT_H_
