# IPC

A test project for exploring high-performance inter-process communication (IPC)
in modern C++.

A producer and a consumer run as separate processes on the same machine,
communicating over shared memory through a transport interface, so other IPC
mechanisms can be swapped in later without changing producer or consumer logic.
Initial scope is local IPC only.

## Quick start

```bash
./scripts/setup.sh      # install tooling and configure the build (macOS, apt-based Linux)
cmake --build build
./build/producer-cli --count 50000 &
./build/consumer-cli --count 50000
```

## Roadmap

Implementation is split into three iterations, each building on the last
without changing `ITransport`'s public shape:

- [x] Define the project architecture
- [x] Design `ITransport`
- [x] **v1 — vertical slice.** Two CLIs, one semaphore-backed SPSC ring
      with mutex-protected cursors, fixed-size sequenced messages,
      fail-fast `bool` errors, one end-to-end test.
- [ ] **v2 — robustness and observability.** Interactive control, signal
      handling, checksums, `sessionId`, crash recovery, stats reporting,
      richer result types.
- [ ] **v3 — performance.** Lock-free cross-process atomics, mutex off the
      per-message fast path — no producer/consumer-visible API change.
- [ ] Evaluate additional transport implementations

See [CLAUDE.md](CLAUDE.md) for the full architecture and the per-iteration
implementation plan.
