# CLAUDE.md

Guidance for working in this repository. See [README.md](README.md) for the
project overview.

## Requirements

- C++23
- GCC or Clang with C++23 support
- CMake 3.20 or newer
- `clang-format`, `clang-tidy`, and `clangd` for the code-quality gate and editor
- A POSIX-compatible operating system for the initial implementation

## Setup

`./scripts/setup.sh` is the one-shot entry point on macOS: it installs the
tooling via Homebrew, symlinks `clang-tidy` and `clangd` onto `PATH`, and runs
the first CMake configure.

- The system **Apple Clang** stays the compiler. The Homebrew `llvm` tools
  (`clang-tidy`, `clangd`) are used only for linting, formatting, and editor
  tooling — do not put Homebrew `llvm` first on `PATH`, or its `clang++` would
  shadow Apple's and require extra libc++/libunwind flags.

## Build and run

```bash
cmake -S . -B build     # configure (also enables the pre-commit hook)
cmake --build build     # build
./build/ipc             # run
```

## Code-quality gate

Format and lint run as CMake targets, and as a pre-commit hook that the first
CMake configure enables automatically (`core.hooksPath .githooks`).

```bash
cmake --build build --target format        # rewrite in place
cmake --build build --target format-check  # verify, no changes
cmake --build build --target tidy          # lint, warnings treated as errors
```

Lint and format are **not** wired into the build itself, so `cmake --build`
stays fast. The pre-commit hook is the enforcing gate; it runs the same checks
on staged files only.

**macOS toolchain note:** `clang-tidy` is invoked with `-- -std=c++23` rather
than `-p build`. Homebrew `clang-tidy` must use its own libc++ headers; reading
the compile database (compiler `/usr/bin/c++`) makes it search Apple's SDK and
fail to find C++23 headers like `<print>`. Keep the standard in sync between
`cmake/CodeQuality.cmake` and `.githooks/pre-commit`.

## Coding convention

- Follow the Google C++ Style Guide as closely as practical. `.clang-format` and
  `.clang-tidy` define the shared baseline — treat them as the source of truth.
- Consistency takes priority over custom formatting. Add project-specific
  overrides only when the Google configuration conflicts with C++23 or a project
  requirement.
- Prefer a good name over a comment. Comment only the complex or non-obvious.

## Dependencies

Third-party libraries, including Boost, are allowed. Libraries and frameworks
that provide or abstract application-level data transport are **excluded**:

- Boost.Interprocess
- ZeroMQ
- gRPC
- D-Bus
- Qt IPC
- Redis
- RabbitMQ

## Design principles

- Clean architecture
- SOLID principles
- High cohesion and low coupling
- Testability
- Cross-platform design
- Minimal dependencies
- Native operating-system IPC primitives

## Architecture

The system is organized around a transport interface (`ITransport`) so the
producer and consumer logic is independent of the underlying IPC mechanism.
Synchronization is a separate concern, built on OS primitives rather than folded
into the transport. When adding a transport, implement the same interface and
keep synchronization decoupled.

## Editor

`.vscode/` configures the clangd language server (reading
`build/compile_commands.json`) with the Microsoft C/C++ IntelliSense engine
disabled. Install the recommended extensions when prompted. Keep `build/` fresh
(`cmake -S . -B build`) after adding files so clangd sees them.
