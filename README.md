# IPC

A test project for exploring high-performance inter-process communication (IPC)
in modern C++.

## Goal

Communicate between a producer and a consumer running as separate processes on
the same machine, organized around a transport interface so different IPC
mechanisms can be swapped without changing the producer or consumer logic.

The initial scope is local IPC only. Communication between different machines is
outside the first iteration.

## Quick start

```bash
./scripts/setup.sh      # install tooling and configure the build (macOS, apt-based Linux)
cmake --build build
./build/ipc
```

## Transport strategy

The first implementation uses shared memory: both processes access the same
physical memory pages, giving low latency, high throughput, and near-zero-copy
data transfer for high-frequency producer-consumer workloads.

Synchronization is treated as a separate concern, implemented with operating
system synchronization primitives.

Other transports may follow, each implementing the same interface:

- Memory-mapped file
- Named pipe (Windows)
- Unix domain socket (POSIX)
- TCP (optional, if remote communication becomes necessary)

## Roadmap

Implementation is split into three iterations, each building on the last
without changing `ITransport`'s public shape:

- [x] Define the project architecture
- [x] Design `ITransport`
- [ ] **v1 — vertical slice.** Two CLIs, one mutex-protected SPSC ring,
      fixed-size sequenced messages, fail-fast `bool` errors, one
      end-to-end test.
- [ ] **v2 — robustness and observability.** Interactive control, signal
      handling, checksums, `sessionId`, crash recovery, stats reporting,
      richer result types.
- [ ] **v3 — performance.** Lock-free cross-process atomics, mutex off the
      per-message fast path — no producer/consumer-visible API change.
- [ ] Evaluate additional transport implementations

See [CLAUDE.md](CLAUDE.md) for the full architecture and the per-iteration
implementation plan.

## Contributing

See [CLAUDE.md](CLAUDE.md) for build, tooling, coding conventions, and
architectural constraints.
