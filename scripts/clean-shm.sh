#!/usr/bin/env bash
# Removes a leftover /ipc_ring_v1 shared-memory segment and its two named
# semaphores (e.g. after a producer was kill -9'd or a test asserted, either
# of which skips MappedSegment/NamedSemaphore's unlink-on-destruct cleanup).
set -euo pipefail

readonly SEGMENT_NAME="/ipc_ring_v1"
readonly FREE_SLOTS_SEM="/ipc_ring_v1_free"
readonly AVAILABLE_MESSAGES_SEM="/ipc_ring_v1_avail"

case "$(uname)" in
  Linux)
    rm -f "/dev/shm${SEGMENT_NAME}"
    rm -f "/dev/shm${FREE_SLOTS_SEM}" "/dev/shm${AVAILABLE_MESSAGES_SEM}"
    ;;
  Darwin)
    # macOS doesn't expose POSIX shm/semaphore objects under a filesystem
    # path, so shm_unlink()/sem_unlink() must be called directly.
    cc -x c -o /tmp/ipc_clean_shm - <<'EOF'
#include <sys/mman.h>
#include <semaphore.h>
int main(void) {
  shm_unlink("/ipc_ring_v1");
  sem_unlink("/ipc_ring_v1_free");
  sem_unlink("/ipc_ring_v1_avail");
  return 0;
}
EOF
    /tmp/ipc_clean_shm || true
    rm -f /tmp/ipc_clean_shm
    ;;
  *)
    echo "Unsupported platform: $(uname)" >&2
    exit 1
    ;;
esac

echo "cleaned ${SEGMENT_NAME}, ${FREE_SLOTS_SEM}, ${AVAILABLE_MESSAGES_SEM}"
