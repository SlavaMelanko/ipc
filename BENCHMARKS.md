# Benchmarks

Informal throughput measurements from manual runs, not an automated
benchmark suite. Numbers come from `StatsReporter`'s per-second
`total`/`pkts/s`/`bytes/s` output, averaged across the interval lines of
each run (see `CLAUDE.md`'s note on `pkts/s`/`bytes/s` measuring
whole-pipeline throughput, not a transport-only benchmark).

## Setup

- **Machine**: Apple M4, macOS (Darwin 25.2.0, arm64)
- `--ring-capacity 33554432` (32 MB) for every run.

## Debug mode results

- **Build**: `cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug` (unoptimized)
- **Command shape**:
  ```bash
  ./build-debug/producer-cli --count 1000000 --payload-size <N> --ring-capacity <N> &
  ./build-debug/consumer-cli --count 1000000 --payload-size <N> --ring-capacity <N>
  ```
- `--count 1000000` for every run.

| Payload | Ring Capacity | Avg pkts/s | Avg throughput |
|---|---|---|---|
| 1 KB  | 32 MB | ~119,000 | ~121.9 MB/s |
| 4 KB  | 32 MB | ~31,950  | ~130.9 MB/s |
| 8 KB  | 32 MB | ~15,870  | ~130.0 MB/s |
| 16 KB | 32 MB | ~7,950   | ~130.3 MB/s |

(1 KB was also run against the 8 MiB default ring capacity — result was
~119,660 pkts/s / ~122.5 MB/s, effectively the same as 32 MB. Ring capacity
did not noticeably affect throughput in these runs.)

## Release mode results

- **Build**: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`
  (CMake's default Release config: `-O3 -DNDEBUG`)
- **Command shape**:
  ```bash
  ./build-release/producer-cli --count 4000000 --payload-size <N> --ring-capacity <N> &
  ./build-release/consumer-cli --count 4000000 --payload-size <N> --ring-capacity <N>
  ```
- `--count 4000000` for every run.

| Payload | Ring Capacity | Avg pkts/s | Avg throughput |
|---|---|---|---|
| 1 KB  | 32 MB | ~408,900 | ~418.7 MB/s |
| 4 KB  | 32 MB | ~120,600 | ~494.0 MB/s |
| 8 KB  | 32 MB | ~61,100  | ~500.6 MB/s |
| 16 KB | 32 MB | ~31,050  | ~508.7 MB/s |

## Observations

- **Debug**: throughput plateaus around **~130 MB/s** once payload size is
  4 KB or larger — `pkts/s` scales roughly as `1 / payloadSize`, while
  `bytes/s` stays flat.
- **Release**: same plateau shape, but ceiling is **~500 MB/s** — roughly
  3.5–4x higher than Debug across all payload sizes, consistent with `-O3`
  optimizing the per-message memcpy/checksum/wait-wake path rather than
  changing its shape.
- Ring capacity (8 MiB vs. 32 MiB) showed no measurable effect at 1 KB
  payload, in either build mode.
- These are manual, single-machine runs, not a controlled benchmark suite
  — useful for relative comparison (Debug vs. Release, payload-size
  scaling), not for absolute capacity planning.

## CRC32 slicing-by-8 fix

Profiling `ComputeChecksum` (see `checksum.cpp`) isolated `Crc32Update` as
the dominant per-message cost — ~93-96% of combined payload-generation +
checksum time, byte-at-a-time table lookup with a loop-carried dependency
that blocks instruction-level parallelism. Replaced with slicing-by-8:
8 bytes folded into the CRC per iteration via 8 parallel table lookups
instead of 8 sequential dependent ones. Verified bit-identical output to
the original algorithm (end-to-end test passes; same polynomial, same
wire format).

- **Build**: `build-release` (`-O3 -DNDEBUG`), same as the Release results
  above.
- **Command shape**: identical to the Release section above,
  `--count 4000000`, `--ring-capacity 33554432`.

| Payload | Ring Capacity | Avg pkts/s | Avg throughput | vs. pre-fix Release |
|---|---|---|---|---|
| 1 KB  | 32 MB | ~444,400 | ~455.1 MB/s | +9%  |
| 4 KB  | 32 MB | ~192,300 | ~787.6 MB/s | +59% |
| 8 KB  | 32 MB | ~104,100 | ~852.5 MB/s | +70% |
| 16 KB | 32 MB | ~56,900  | ~932.0 MB/s | +83% |
| 32 KB | 32 MB | ~29,730  | ~974.1 MB/s | n/a (testing only, no pre-fix baseline) |
| 64 KB | 32 MB | ~15,170  | ~994.0 MB/s | n/a (testing only, no pre-fix baseline) |

Gain grows with payload size, as expected — CRC cost (and therefore the
slicing speedup) scales with payload size, while fixed per-message costs
(syscalls, semaphore wait/wake) don't. The ~130 MB/s Debug-mode plateau
and the ~500 MB/s pre-fix Release plateau documented above no longer hold
for payloads ≥ 4 KB in Release mode.

32 KB and 64 KB were run for testing purposes only (no corresponding
pre-fix baseline exists at these sizes). Throughput growth flattens past
16 KB — 16→32 KB gained ~4.5%, 32→64 KB only ~2% — consistent with CRC's
share of per-message cost approaching its ceiling: beyond a certain
payload size, fixed per-message overhead becomes negligible and
throughput converges toward the CPU's raw slicing-by-8 CRC rate.

## zlib CRC32

Benchmarked our hand-rolled slicing-by-8 `Crc32Update` against zlib's
`crc32()` in isolation — zlib was ~4-4.5x faster at every payload size
tested, with output verified bit-identical (same IEEE 802.3 polynomial,
same result on empty/small/large inputs and on the sequential
header-then-payload two-call pattern `ComputeChecksum` actually uses).
Swapped `checksum.cpp` to call zlib's `crc32()` directly and removed the
hand-rolled byte-table and slicing-by-8 tables entirely
(`find_package(ZLIB REQUIRED)` + `ZLIB::ZLIB` added to `CMakeLists.txt`).
End-to-end test passes; wire format unchanged.

- **Build**: `build-release` (`-O3 -DNDEBUG`).
- **Command shape**: payload size increased and `--count` decreased
  proportionally per run, so each run moves roughly the same total number
  of bytes rather than the same message count:
  ```bash
  ./build-release/producer-cli --count <N> --payload-size <P> --ring-capacity 33554432 &
  ./build-release/consumer-cli --count <N> --payload-size <P> --ring-capacity 33554432
  ```

| Payload | Count | Ring Capacity | Avg pkts/s | Avg throughput |
|---|---|---|---|---|
| 1 KB  | 4,000,000 | 32 MB | ~628,850 | ~643.9 MB/s |
| 4 KB  | 3,000,000 | 32 MB | ~391,700 | ~1604.4 MB/s |
| 8 KB  | 2,000,000 | 32 MB | ~211,470 | ~1732.4 MB/s |
| 16 KB | 1,000,000 | 32 MB | ~111,110 | ~1820.4 MB/s |
| 32 KB | 1,000,000 | 32 MB | ~56,540  | ~1852.7 MB/s |
| 64 KB | 1,000,000 | 32 MB | ~28,820  | ~1889.0 MB/s |

Compared to slicing-by-8 at matching payload sizes (1 KB and 16 KB
overlap with the earlier table): **+42%** at 1 KB (~455.1 → ~643.9 MB/s)
and **+95%** at 16 KB (~932.0 → ~1820.4 MB/s), crossing the 1 GB/s mark
and approaching ~1.9 GB/s at larger payloads. Same flattening pattern as
before — most of the gain over slicing-by-8 shows up once CRC dominates
per-message cost, i.e. at larger payloads.
