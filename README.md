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

- Define the project architecture
- Design `ITransport`
- Implement the shared-memory transport
- Implement synchronization primitives
- Add unit tests
- Add a benchmark suite
- Evaluate additional transport implementations

## Contributing

See [CLAUDE.md](CLAUDE.md) for build, tooling, coding conventions, and
architectural constraints.
