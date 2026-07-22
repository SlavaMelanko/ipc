#include "common/transport/named_semaphore.h"

#include <fcntl.h>

#include <cerrno>
#include <utility>

#ifdef __linux__
#include <ctime>
#else
#include <thread>
#endif

#include "common/util/posix.h"

namespace ipc::common {

std::optional<NamedSemaphore> NamedSemaphore::Create(const std::string& name,
                                                     unsigned int initialValue, bool unlink) {
  if (unlink) {
    sem_unlink(name.c_str());
  }

  sem_t* sem = sem_open(name.c_str(), O_CREAT | O_EXCL, kOwnerReadWrite, initialValue);
  if (sem == SEM_FAILED) {
    return std::nullopt;
  }

  return NamedSemaphore(name, sem, /*isOwner=*/true);
}

std::optional<NamedSemaphore> NamedSemaphore::Attach(const std::string& name) {
  sem_t* sem = sem_open(name.c_str(), 0);
  if (sem == SEM_FAILED) {
    return std::nullopt;
  }

  return NamedSemaphore(name, sem, /*isOwner=*/false);
}

NamedSemaphore::NamedSemaphore(NamedSemaphore&& other) noexcept
    : name_(std::move(other.name_)), sem_(other.sem_), isOwner_(other.isOwner_) {
  other.sem_ = nullptr;
  other.isOwner_ = false;
}

NamedSemaphore& NamedSemaphore::operator=(NamedSemaphore&& other) noexcept {
  if (this != &other) {
    if (sem_ != nullptr) {
      sem_close(sem_);
    }
    if (isOwner_) {
      sem_unlink(name_.c_str());
    }

    name_ = std::move(other.name_);
    sem_ = other.sem_;
    isOwner_ = other.isOwner_;

    other.sem_ = nullptr;
    other.isOwner_ = false;
  }

  return *this;
}

NamedSemaphore::~NamedSemaphore() {
  if (sem_ != nullptr) {
    sem_close(sem_);
  }
  if (isOwner_) {
    sem_unlink(name_.c_str());
  }
}

int NamedSemaphore::Wait() {
  int result;
  do {
    result = sem_wait(sem_);
  } while (result != 0 && errno == EINTR);

  return result;
}

#ifdef __linux__

// sem_timedwait() takes an absolute CLOCK_REALTIME deadline, not a duration.
NamedSemaphore::WaitResult NamedSemaphore::WaitFor(std::chrono::milliseconds bound) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += static_cast<time_t>(bound.count() / 1000);
  deadline.tv_nsec += static_cast<decltype(deadline.tv_nsec)>((bound.count() % 1000) * 1000000);
  if (deadline.tv_nsec >= 1000000000) {
    deadline.tv_nsec -= 1000000000;
    deadline.tv_sec += 1;
  }

  int result;
  do {
    result = sem_timedwait(sem_, &deadline);
  } while (result != 0 && errno == EINTR);

  if (result == 0) {
    return WaitResult::kAcquired;
  }

  return errno == ETIMEDOUT ? WaitResult::kTimedOut : WaitResult::kError;
}

#else

// macOS has no sem_timedwait() at all (confirmed: undeclared, no man page --
// Apple never implemented it, unlike Linux/glibc). Polling sem_trywait() is
// the portable fallback; a non-blocking helper-thread design would avoid the
// poll interval's CPU cost but adds a persistent thread and an in-process
// queue per semaphore just to work around one platform's gap -- not worth it
// at this poll interval, and this bounded path is only used for periodic
// peer-liveness checks, not the hot per-message wait (which still uses the
// zero-CPU blocking Wait() above).
NamedSemaphore::WaitResult NamedSemaphore::WaitFor(std::chrono::milliseconds bound) {
  constexpr std::chrono::milliseconds kPollInterval{20};

  auto deadline = std::chrono::steady_clock::now() + bound;
  for (;;) {
    if (sem_trywait(sem_) == 0) {
      return WaitResult::kAcquired;
    }
    if (errno != EAGAIN && errno != EINTR) {
      return WaitResult::kError;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return WaitResult::kTimedOut;
    }

    std::this_thread::sleep_for(kPollInterval);
  }
}

#endif

NamedSemaphore::NamedSemaphore(std::string name, sem_t* sem, bool isOwner)
    : name_(std::move(name)), sem_(sem), isOwner_(isOwner) {}

}  // namespace ipc::common
