#include "common/transport/mapped_segment.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

#include "common/util/posix.h"
#include "common/util/scope_exit.h"

namespace ipc::common {

namespace {

void* Map(int fd, std::size_t size) {
  return mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

}  // namespace

MappedSegment::MappedSegment(std::string name, void* mapping, std::size_t size, bool isOwner)
    : name_(std::move(name)), mapping_(mapping), size_(size), isOwner_(isOwner) {}

MappedSegment::MappedSegment(MappedSegment&& other) noexcept
    : name_(std::move(other.name_)),
      mapping_(other.mapping_),
      size_(other.size_),
      isOwner_(other.isOwner_) {
  other.mapping_ = nullptr;
  other.isOwner_ = false;
}

MappedSegment& MappedSegment::operator=(MappedSegment&& other) noexcept {
  if (this != &other) {
    if (mapping_ != nullptr) {
      munmap(mapping_, size_);
    }
    if (isOwner_) {
      shm_unlink(name_.c_str());
    }

    name_ = std::move(other.name_);
    mapping_ = other.mapping_;
    size_ = other.size_;
    isOwner_ = other.isOwner_;

    other.mapping_ = nullptr;
    other.isOwner_ = false;
  }

  return *this;
}

MappedSegment::~MappedSegment() {
  if (mapping_ != nullptr) {
    munmap(mapping_, size_);
  }
  if (isOwner_) {
    shm_unlink(name_.c_str());
  }
}

std::optional<MappedSegment> MappedSegment::Create(const std::string& name, std::size_t size) {
  int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, kOwnerReadWrite);
  if (fd == -1) {
    return std::nullopt;
  }
  ScopeExit closeFd([fd]() noexcept { close(fd); });
  ScopeExit unlinkOnFailure([&name]() noexcept { shm_unlink(name.c_str()); });

  if (Failed(ftruncate(fd, static_cast<off_t>(size)))) {
    return std::nullopt;
  }

  void* mapping = Map(fd, size);
  if (mapping == MAP_FAILED) {
    return std::nullopt;
  }

  unlinkOnFailure.Dismiss();

  return MappedSegment(name, mapping, size, /*isOwner=*/true);
}

std::optional<MappedSegment> MappedSegment::Attach(const std::string& name, std::size_t size) {
  int fd = shm_open(name.c_str(), O_RDWR, kOwnerReadWrite);
  if (fd == -1) {
    return std::nullopt;
  }
  ScopeExit closeFd([fd]() noexcept { close(fd); });

  // ftruncate() rounds the segment up to a whole page, so the real segment
  // is often larger than size, never smaller. A smaller actual segment means
  // a different slotCount than the producer computed, or mmap accessing
  // pages past the real segment (SIGBUS) -- reject that, but not the benign
  // page-rounding case.
  struct stat st;
  if (Failed(fstat(fd, &st)) || std::cmp_less(st.st_size, size)) {
    return std::nullopt;
  }

  void* mapping = Map(fd, size);
  if (mapping == MAP_FAILED) {
    return std::nullopt;
  }

  return MappedSegment(name, mapping, size, /*isOwner=*/false);
}

}  // namespace ipc::common
