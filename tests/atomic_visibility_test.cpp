// Proves atomic<uint64_t> in shared memory is actually visible across
// process boundaries, not just lock-free within one process.
// is_always_lock_free (see control_block.h) rules out an internal,
// process-local fallback lock, but says nothing about whether a
// release-store in one process's mapping is observed by an acquire-load in
// a different process's mapping of the same physical pages. That is a
// platform property, not something the standard guarantees -- verified here
// with a real fork() and two independent address spaces.
//
// Checks below use explicit aborts, not assert(), so they still run under a
// Release (-DNDEBUG) build -- this test only means something if it can fail.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace {

constexpr const char* kSegmentName = "/ipc_atomic_visibility_test";
constexpr std::uint64_t kSentinel = 0xDEADBEEFCAFEULL;
constexpr std::chrono::milliseconds kTimeout{2000};

struct Shared {
  std::atomic<std::uint64_t> value;
};

void Check(bool condition) {
  if (!condition) {
    std::abort();
  }
}

}  // namespace

int main() {
  shm_unlink(kSegmentName);

  int fd = shm_open(kSegmentName, O_CREAT | O_EXCL | O_RDWR, 0600);
  Check(fd >= 0);
  Check(ftruncate(fd, sizeof(Shared)) == 0);

  auto* shared = static_cast<Shared*>(
      mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  Check(shared != MAP_FAILED);
  close(fd);

  new (shared) Shared{};

  pid_t pid = fork();
  Check(pid >= 0);

  if (pid == 0) {
    // Child: busy-poll acquire-load until it observes the parent's
    // release-store, bounded so a broken platform fails fast instead of
    // hanging CI.
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (shared->value.load(std::memory_order_acquire) != kSentinel) {
      if (std::chrono::steady_clock::now() > deadline) {
        std::_Exit(1);
      }
    }
    std::_Exit(0);
  }

  // Parent: give the child a moment to enter its poll loop, then publish.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  shared->value.store(kSentinel, std::memory_order_release);

  int status = 0;
  Check(waitpid(pid, &status, 0) == pid);

  munmap(shared, sizeof(Shared));
  shm_unlink(kSegmentName);

  Check(WIFEXITED(status));
  Check(WEXITSTATUS(status) == 0);

  return 0;
}
