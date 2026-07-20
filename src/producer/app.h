#ifndef IPC_PRODUCER_APP_H_
#define IPC_PRODUCER_APP_H_

#include <cstdint>

#include "common/app_config.h"

namespace ipc::producer {

class App {
 public:
  explicit App(ipc::common::AppConfig config);

  // Sends the parsed --count of fixed-size sequenced messages through a
  // freshly created ring, blocking on backpressure as needed. Responds to
  // 'p'/'r'/'q' on stdin and SIGINT/SIGTERM by pausing/resuming/stopping
  // early. Returns false on any transport failure.
  [[nodiscard]] bool Run() const;

 private:
  ipc::common::AppConfig config_;
  // One per process lifetime -- lets a consumer distinguish a producer
  // restart from data loss (see AGENTS.md's "Session ID").
  std::uint64_t sessionId_;
};

}  // namespace ipc::producer

#endif  // IPC_PRODUCER_APP_H_
