#ifndef IPC_COMMON_TRANSPORT_MAPPED_SEGMENT_H_
#define IPC_COMMON_TRANSPORT_MAPPED_SEGMENT_H_

#include <cstddef>
#include <optional>
#include <string>

namespace ipc::common {

// Owns a POSIX shared-memory segment: create-or-attach, the mmap'd view,
// and cleanup (munmap always; shm_unlink only if this side created it).
class MappedSegment {
 public:
  MappedSegment(const MappedSegment&) = delete;
  MappedSegment& operator=(const MappedSegment&) = delete;
  MappedSegment(MappedSegment&& other) noexcept;
  MappedSegment& operator=(MappedSegment&& other) noexcept;
  ~MappedSegment();

  // Creates a new segment (O_CREAT | O_EXCL) of the given size. Fails if a
  // segment with this name already exists.
  static std::optional<MappedSegment> Create(const std::string& name,
                                             std::size_t size);

  // Attaches to a segment another process has already created. Fails if the
  // segment doesn't exist.
  static std::optional<MappedSegment> Attach(const std::string& name,
                                             std::size_t size);

  void* data() { return mapping_; }
  [[nodiscard]] std::size_t size() const { return size_; }

 private:
  MappedSegment(std::string name, void* mapping, std::size_t size,
                bool isOwner);

  static std::optional<MappedSegment> Open(const std::string& name,
                                           std::size_t size, bool createNew);

  std::string name_;
  void* mapping_;
  std::size_t size_;
  bool isOwner_;
};

}  // namespace ipc::common

#endif  // IPC_COMMON_TRANSPORT_MAPPED_SEGMENT_H_
