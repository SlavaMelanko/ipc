#ifndef IPC_PRODUCER_APP_H_
#define IPC_PRODUCER_APP_H_

#include "producer/command_line_parser.h"

namespace ipc::producer {

class App {
 public:
  // Throws std::runtime_error if argv fails to parse.
  App(int argc, char** argv);

  // Sends the parsed --count of fixed-size sequenced messages through a
  // freshly created ring, blocking on backpressure as needed. Returns false
  // on any transport failure.
  [[nodiscard]] bool Run() const;

 private:
  CommandLineArgs cmdArgs_;
};

}  // namespace ipc::producer

#endif  // IPC_PRODUCER_APP_H_
