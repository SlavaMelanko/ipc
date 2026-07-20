#include "common/message/message_validator.h"

#include <print>

#include "common/message/checksum.h"

namespace ipc::common {

void MessageValidator::Validate(const Message& message) {
  const Header& header = message.header;

  if (IsNewSession(header)) {
    sessionSeen_ = true;
    lastSessionId_ = header.sessionId;
    lastSequenceNumber_ = header.sequenceNumber;
  } else {
    CheckSequence(header);
    lastSequenceNumber_ = header.sequenceNumber;
  }

  CheckChecksum(message);
}

bool MessageValidator::IsNewSession(const Header& header) const {
  return !sessionSeen_ || header.sessionId != lastSessionId_;
}

void MessageValidator::CheckSequence(const Header& header) {
  if (header.sequenceNumber == lastSequenceNumber_ + 1) {
    return;
  }

  ++errorCount_;
  std::println(stderr, "sequence gap in session {}, expected={} actual={}", header.sessionId,
               lastSequenceNumber_ + 1, header.sequenceNumber);
}

void MessageValidator::CheckChecksum(const Message& message) {
  Header headerWithoutChecksum = message.header;
  headerWithoutChecksum.checksum = 0;
  if (ComputeChecksum(headerWithoutChecksum, message.payload) == message.header.checksum) {
    return;
  }

  ++errorCount_;
  std::println(stderr, "checksum mismatch in session {} at sequenceNumber={}",
               message.header.sessionId, message.header.sequenceNumber);
}

}  // namespace ipc::common
