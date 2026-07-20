#ifndef IPC_COMMON_MESSAGE_MESSAGE_VALIDATOR_H_
#define IPC_COMMON_MESSAGE_MESSAGE_VALIDATOR_H_

#include <cstdint>

#include "common/message/message.h"

namespace ipc::common {

// Checksum + sessionId-aware sequence checking. A defect (checksum mismatch,
// in-session sequence gap) increments errorCount rather than stopping --
// see AGENTS.md's "Defect handling (consumer side)".
class MessageValidator {
 public:
  void Validate(const Message& message);

  [[nodiscard]] std::uint64_t ErrorCount() const { return errorCount_; }

 private:
  // True if this message starts a new producer session -- a reset
  // sequenceNumber is then expected, not a gap.
  [[nodiscard]] bool IsNewSession(const Header& header) const;
  void CheckSequence(const Header& header);
  void CheckChecksum(const Message& message);

  std::uint64_t lastSessionId_ = 0;
  std::uint64_t lastSequenceNumber_ = 0;
  std::uint64_t errorCount_ = 0;
  bool sessionSeen_ = false;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_MESSAGE_MESSAGE_VALIDATOR_H_
