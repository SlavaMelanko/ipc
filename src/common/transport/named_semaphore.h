#ifndef IPC_COMMON_TRANSPORT_NAMED_SEMAPHORE_H_
#define IPC_COMMON_TRANSPORT_NAMED_SEMAPHORE_H_

#include <semaphore.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ipc::common {

// Owns one POSIX named semaphore; sem_unlink only if this side created it.
class NamedSemaphore {
 public:
  enum class WaitResult : std::uint8_t { kAcquired, kTimedOut, kError };

  // unlink: true if the caller has already confirmed no live process owns
  // an existing semaphore of this name (e.g. a crash left it behind).
  static std::optional<NamedSemaphore> Create(const std::string& name, unsigned int initialValue,
                                              bool unlink = false);
  static std::optional<NamedSemaphore> Attach(const std::string& name);

  NamedSemaphore(const NamedSemaphore&) = delete;
  NamedSemaphore& operator=(const NamedSemaphore&) = delete;
  NamedSemaphore(NamedSemaphore&& other) noexcept;
  NamedSemaphore& operator=(NamedSemaphore&& other) noexcept;
  ~NamedSemaphore();

  // Retries on EINTR -- sem_wait() isn't restarted by the kernel the way an
  // SA_RESTART-flagged syscall would be, so a caller-visible EINTR here would
  // otherwise misreport an ordinary signal interruption as semaphore failure.
  int Wait();

  // Bounded wait, so a caller can periodically re-check e.g. peer liveness
  // instead of blocking forever. Also retries on EINTR.
  WaitResult WaitFor(std::chrono::milliseconds bound);

  int Post() { return sem_post(sem_); }

 private:
  NamedSemaphore(std::string name, sem_t* sem, bool isOwner);

  std::string name_;
  sem_t* sem_;
  bool isOwner_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_NAMED_SEMAPHORE_H_
