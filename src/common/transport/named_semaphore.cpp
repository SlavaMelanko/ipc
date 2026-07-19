#include "common/transport/named_semaphore.h"

#include <fcntl.h>

#include <utility>

#include "common/util/posix.h"

namespace ipc::common {

std::optional<NamedSemaphore> NamedSemaphore::Create(const std::string& name,
                                                     unsigned int initialValue) {
  sem_unlink(name.c_str());

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

NamedSemaphore::NamedSemaphore(std::string name, sem_t* sem, bool isOwner)
    : name_(std::move(name)), sem_(sem), isOwner_(isOwner) {}

}  // namespace ipc::common
