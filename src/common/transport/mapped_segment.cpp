#include "common/transport/mapped_segment.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

#include "common/util/posix.h"
#include "common/util/scope_exit.h"

namespace ipc::common {

std::optional<MappedSegment> MappedSegment::Open(const std::string& name,
                                                 std::size_t size,
                                                 bool createNew) {
  int flags = O_RDWR | (createNew ? (O_CREAT | O_EXCL) : 0);
  int fd = shm_open(name.c_str(), flags, 0600);
  if (fd == -1) {
    return std::nullopt;
  }
  ScopeExit closeFd([fd]() noexcept { close(fd); });

  if (createNew) {
    if (Failed(ftruncate(fd, static_cast<off_t>(size)))) {
      shm_unlink(name.c_str());
      return std::nullopt;
    }
  } else {
    // A mismatched size here means a different slotCount than the producer
    // computed, or mmap accessing pages past the real segment (SIGBUS).
    struct stat st;
    if (Failed(fstat(fd, &st)) || std::cmp_not_equal(st.st_size, size)) {
      return std::nullopt;
    }
  }

  void* mapping =
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    if (createNew) {
      shm_unlink(name.c_str());
    }
    return std::nullopt;
  }

  return MappedSegment(name, mapping, size, createNew);
}

std::optional<MappedSegment> MappedSegment::Create(const std::string& name,
                                                   std::size_t size) {
  return Open(name, size, /*createNew=*/true);
}

std::optional<MappedSegment> MappedSegment::Attach(const std::string& name,
                                                   std::size_t size) {
  return Open(name, size, /*createNew=*/false);
}

MappedSegment::MappedSegment(std::string name, void* mapping, std::size_t size,
                             bool isOwner)
    : name_(std::move(name)),
      mapping_(mapping),
      size_(size),
      isOwner_(isOwner) {}

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

}  // namespace ipc::common
