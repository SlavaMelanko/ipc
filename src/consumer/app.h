#ifndef IPC_CONSUMER_APP_H_
#define IPC_CONSUMER_APP_H_

#include "consumer/command_line_parser.h"

namespace ipc::consumer {

class App {
 public:
  // Throws std::runtime_error if argv fails to parse.
  App(int argc, char** argv);

  // Attaches to the producer's shared-memory segment and receives until
  // EndOfStream, validating each message via PacketValidator and asserting
  // the final count matches --count. Returns false if attaching fails, a
  // malformed frame is seen, the count doesn't match, or any defect was
  // detected (PacketValidator::ErrorCount() != 0).
  [[nodiscard]] bool Run() const;

 private:
  CommandLineArgs cmdArgs_;
};

}  // namespace ipc::consumer

#endif  // IPC_CONSUMER_APP_H_
