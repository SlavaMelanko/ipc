#ifndef IPC_CONSUMER_APP_H_
#define IPC_CONSUMER_APP_H_

#include "common/app_config.h"

namespace ipc::consumer {

class App {
 public:
  explicit App(ipc::common::AppConfig config);

  // Attaches to the producer's shared-memory segment and receives until
  // EndOfStream, validating each message via MessageValidator and asserting
  // the final count matches --count. Responds to 'p'/'r'/'q' on stdin and
  // SIGINT/SIGTERM by pausing/resuming/stopping early. Returns false if
  // attaching fails, a malformed frame is seen, the count doesn't match, or
  // any defect was detected (MessageValidator::ErrorCount() != 0).
  [[nodiscard]] bool Run() const;

 private:
  ipc::common::AppConfig config_;
};

}  // namespace ipc::consumer

#endif  // IPC_CONSUMER_APP_H_
