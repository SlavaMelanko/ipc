#!/usr/bin/env bash
# Removes a leftover /ipc_ring_v1 shared-memory segment (e.g. after a
# producer was kill -9'd, which skips MappedSegment's shm_unlink cleanup).
set -euo pipefail

readonly SEGMENT_NAME="/ipc_ring_v1"

case "$(uname)" in
  Linux)
    rm -f "/dev/shm${SEGMENT_NAME}"
    ;;
  Darwin)
    # macOS doesn't expose POSIX shm objects under a filesystem path, so
    # shm_unlink() must be called directly.
    cc -x c -o /tmp/ipc_clean_shm - <<'EOF'
#include <sys/mman.h>
int main(void) { return shm_unlink("/ipc_ring_v1") == 0 ? 0 : 1; }
EOF
    /tmp/ipc_clean_shm || true
    rm -f /tmp/ipc_clean_shm
    ;;
  *)
    echo "Unsupported platform: $(uname)" >&2
    exit 1
    ;;
esac

echo "cleaned ${SEGMENT_NAME}"
