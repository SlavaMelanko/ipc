# Benchmarks

Informal throughput measurements from manual runs, not an automated
benchmark suite. Numbers come from `StatsReporter`'s per-second
`total`/`msgs/s`/`bytes/s` output, averaged across the interval lines of
each run (see `CLAUDE.md`'s note on `msgs/s`/`bytes/s` measuring
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
- **Pattern-based payload generation** (replacing
  `DeterministicPayloadGenerator`'s byte-at-a-time loop with a single
  `memcpy` from a precomputed 256-byte pattern table plus prefix-doubling
  copies — output is byte-for-byte identical): **+28% to +82%** end-to-end
  throughput on the Ryzen, gain largest at small/mid payloads where the
  producer was the sole bottleneck. On the Apple M4 the gain is far larger
  (+123% at 1 KB to ~9× at 64 KB) — macOS's hardware-CRC32 libz meant
  generation, not CRC, was the dominant cost there; see the M4 results
  below. Stage profiling showed generation had
  been ~28% of producer CPU at 64 KB (5,578 ns → 550 ns per message).
  After this change the consumer becomes the pacer at large payloads, and
  CRC32 dominates both sides (~65–72% of per-message CPU) — the next
  optimization target.

## Results

### Apple M4, macOS (Darwin 25.2.0, arm64)

With pattern-based payload generation (throughput = msgs/s × frame size,
i.e. payload + 32-byte header). Each row averages the full interval lines
of two runs:

| Payload | Count      | Ring Capacity | Avg msgs/s | Avg throughput |
| ------- | ---------- | ------------- | ---------- | -------------- |
| 1 KB    | 10,000,000 | 32 MB         | ~1,467,300 | ~1.55 GB/s     |
| 4 KB    | 5,000,000  | 32 MB         | ~1,096,500 | ~4.53 GB/s     |
| 8 KB    | 2,500,000  | 32 MB         | ~471,000   | ~3.87 GB/s     |
| 16 KB   | 1,000,000  | 32 MB         | ~326,100   | ~5.35 GB/s     |
| 32 KB   | 1,000,000  | 32 MB         | ~208,400   | ~6.84 GB/s     |
| 64 KB   | 1,000,000  | 32 MB         | ~262,000   | ~17.18 GB/s    |

The gains over the previous (byte-at-a-time generator) numbers are far
larger than the Ryzen's — +123% at 1 KB up to roughly 9× at 64 KB —
because Apple's system libz computes CRC32 with ARMv8 hardware
instructions, so on macOS the byte-at-a-time generator, not CRC32, was the
dominant producer cost (the old 64 KB result, ~1.9 GB/s, matches a
~1 byte/cycle generation loop almost exactly). With generation reduced to
a `memcpy`, macOS has no software-CRC plateau like the Ryzen's
~4.0–4.5 GB/s. The curve is non-monotonic (dip at 8 KB, jump at 64 KB);
both points reproduce across runs — likely cache/`memcpy`-strategy
effects, not measurement noise.

### AMD Ryzen 7 7700X, Ubuntu 26.04 LTS (x86_64)

With pattern-based payload generation (throughput = msgs/s × frame size,
i.e. payload + 32-byte header):

| Payload | Count      | Ring Capacity | Avg msgs/s | Avg throughput |
| ------- | ---------- | ------------- | ---------- | -------------- |
| 1 KB    | 10,000,000 | 32 MB         | ~2,462,600 | ~2.60 GB/s     |
| 4 KB    | 5,000,000  | 32 MB         | ~1,000,000 | ~4.13 GB/s     |
| 8 KB    | 2,500,000  | 32 MB         | ~487,100   | ~4.01 GB/s     |
| 16 KB   | 1,000,000  | 32 MB         | ~263,800   | ~4.33 GB/s     |
| 32 KB   | 1,000,000  | 32 MB         | ~134,600   | ~4.41 GB/s     |
| 64 KB   | 1,000,000  | 32 MB         | ~68,700    | ~4.51 GB/s     |

## Profiling (Ryzen, after pattern-based generation)

`/usr/bin/time -v` on real runs plus a per-stage microbenchmark.
Whole-run view (voluntary context switches ≈ blocking waits):

|              | 1 KB × 8M               | 64 KB × 300k                            |
| ------------ | ----------------------- | --------------------------------------- |
| Throughput   | ~2.5M msgs/s            | ~69.7k msgs/s (4.56 GB/s)               |
| Producer CPU | 97%, 585 blocks — pacer | 96%, 48,587 blocks — waits on full ring |
| Consumer CPU | 92%, 10,770 blocks      | 95%, 11 blocks — never waits, pacer     |

Per-stage cost per message (share vs. measured ~14.1 µs/msg producer CPU):

| Stage              | 1 KB    | 64 KB     | Share of producer CPU @ 64 KB   |
| ------------------ | ------- | --------- | ------------------------------- |
| CRC32 (zlib)       | 259 ns  | 10,235 ns | ~72%                            |
| Payload generation | 13 ns   | 550 ns    | ~4%                             |
| memcpy (in-cache)  | 8 ns    | 789 ns    | ~6% (real shm copy ~3 µs, ~20%) |
| sem post+wait pair | 4.3 ns  | 4.3 ns    | ~0%                             |
| CurrentTimestamp   | 16.5 ns | 16.5 ns   | ~0%                             |

CRC32 dominates both processes (~65% of per-message CPU at 1 KB, ~72% at
64 KB) and is the ~4.0–4.5 GB/s plateau: the consumer's CRC recomputation
sets the pace while the producer blocks on a full ring. Faster CRC32
(zlib-ng or hardware CRC32C) is the next target; everything else is
negligible on Linux.
