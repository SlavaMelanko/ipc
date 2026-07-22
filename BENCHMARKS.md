# Benchmarks

Informal throughput measurements from manual runs, not an automated
benchmark suite. Numbers come from `StatsReporter`'s per-second
`total`/`pkts/s`/`bytes/s` output, averaged across the interval lines of
each run (see `CLAUDE.md`'s note on `pkts/s`/`bytes/s` measuring
whole-pipeline throughput, not a transport-only benchmark).

## Setup

- **Build**: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`
  (CMake's default Release config: `-O3 -DNDEBUG`)
- **Command shape**:

  ```bash
  ./build-release/producer-cli --count <N> --payload-size <P> --ring-capacity <R> &
  ./build-release/consumer-cli --count <N> --payload-size <P> --ring-capacity <R>
  ```

- All results below use the current zlib-based CRC32 (`checksum.cpp`) and
  lock-free (`std::atomic`-cursor) `SharedMemoryTransport`.

## Optimization history

- **Slicing-by-8** (replacing byte-at-a-time table lookup with 8 parallel
  table lookups per iteration): **+9% to +83%** throughput vs. the
  original hand-rolled CRC32, gain increasing with payload size.
- **zlib `crc32()`** (replacing the hand-rolled slicing-by-8
  implementation): a further **+42% at 1 KB** and **+95% at 16 KB** vs.
  slicing-by-8, crossing 1 GB/s and approaching ~1.9 GB/s at larger
  payloads on the Apple M4.
- **Lock-free cursors** (replacing the `pthread_mutex_t`-guarded
  write/read cursors with cache-line-separated `std::atomic<uint64_t>`,
  removing the mutex from the `send()`/`receive()` fast path entirely):
  throughput unchanged within run-to-run noise at every payload size
  tested on the Apple M4 — the mutex was already held only for the
  claim-and-increment, not the copy, so the bottleneck at these payload
  sizes is the per-message `memcpy`/CRC32 work, not cursor contention.
  The change is a correctness/scalability win (no lock on the fast path,
  safe under real cross-core contention) rather than a throughput one at
  this payload range.

## Results

### Apple M4, macOS (Darwin 25.2.0, arm64)

| Payload | Count     | Ring Capacity | Avg pkts/s | Avg throughput |
| ------- | --------- | ------------- | ---------- | -------------- |
| 1 KB    | 4,000,000 | 32 MB         | ~657,200   | ~673.0 MB/s    |
| 4 KB    | 3,000,000 | 32 MB         | ~400,110   | ~1638.8 MB/s   |
| 8 KB    | 2,000,000 | 32 MB         | ~212,150   | ~1738.0 MB/s   |
| 16 KB   | 1,000,000 | 32 MB         | ~111,110   | ~1820.4 MB/s   |
| 32 KB   | 1,000,000 | 32 MB         | ~57,120    | ~1871.8 MB/s   |
| 64 KB   | 1,000,000 | 32 MB         | ~28,940    | ~1896.6 MB/s   |

### AMD Ryzen 7 7700X, Ubuntu 26.04 LTS (x86_64)

| Payload | Count      | Ring Capacity | Avg pkts/s | Avg throughput |
| ------- | ---------- | ------------- | ---------- | -------------- |
| 1 KB    | 10,000,000 | 32 MB         | ~1,428,571 | ~1.51 GB/s     |
| 4 KB    | 5,000,000  | 32 MB         | ~550,648   | ~2.27 GB/s     |
| 8 KB    | 2,500,000  | 32 MB         | ~343,815   | ~2.83 GB/s     |
| 16 KB   | 1,000,000  | 32 MB         | ~196,066   | ~3.22 GB/s     |
| 32 KB   | 1,000,000  | 32 MB         | ~101,951   | ~3.34 GB/s     |
| 64 KB   | 1,000,000  | 32 MB         | ~53,641    | ~3.52 GB/s     |

The Ryzen machine is consistently faster than the M4 at every payload
size tested (~1.4–2.2x), attributed to hardware (faster CPU/memory
subsystem) rather than a code difference — both runs use the same build.
