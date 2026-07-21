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
