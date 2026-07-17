# Agent Instructions

Guidance for working in this repository. See [README.md](README.md) for the
project overview.

## Requirements

- C++23
- GCC or Clang with C++23 support
- CMake 3.20 or newer
- `clang-format`, `clang-tidy`, and `clangd` for the code-quality gate and editor
- A POSIX-compatible operating system for the initial implementation

## Setup

`./scripts/setup.sh` is the one-shot entry point on macOS and apt-based Linux:
it installs the tooling and runs the first CMake configure.

- **macOS**: installs via Homebrew and symlinks `clang-tidy`/`clangd` onto
  `PATH`. The system **Apple Clang** stays the compiler — the Homebrew `llvm`
  tools are used only for linting, formatting, and editor tooling. Do not put
  Homebrew `llvm` first on `PATH`, or its `clang++` would shadow Apple's and
  require extra libc++/libunwind flags.
- **Linux**: installs via `apt` and pins `gcc-15`/`g++-15` as the compiler
  (`CC`/`CXX` env vars passed to the CMake configure), since the distro default
  `gcc`/`g++` may be older and lack C++23 features like `<print>`.

CI (`.github/workflows/ci-ubuntu.yml`) runs the same `setup.sh` on
`ubuntu-26.04`, so local Linux setup and CI stay identical.

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
fail to find C++23 headers like `<print>`. This also works unchanged on Linux,
so the same invocation is used everywhere. Keep the standard in sync between
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

Architecture, protocol, and implementation plan for the producer/consumer
pair: architecture first (the shared contract both processes must honor),
then the producer plan, then the consumer plan — each plan covers only what's
specific to that app and links back into the architecture sections above it
rather than restating them.

### Three iterations, not one

The design below went through an extended review process that kept surfacing
real gaps — a lost-wakeup claim that didn't hold, a PID-liveness check with an
initialization race, malformed-frame handling that ran a bound check too late,
missing crash-recovery scoping, and more. Each fix added scope: cross-process
atomics, `sessionId`, `PeerClosed` detection, crash recovery, rich result
enums, `Controller`/signal-safety machinery, stats reporting. The cumulative
result was closer to a production-hardened IPC library than a first
implementation pass — every mechanism below is real and stays, but none of it
should all land in one uninterrupted build.

So implementation is split into three iterations. **Nothing below is
deleted or weakened by this split** — every mechanism this document has
worked out (`sessionId`, `PeerClosed`, crash recovery, cross-process atomics,
rich enums, `Controller`, `StatsReporter`, the whole lot) is still fully
specified here, just tagged with the iteration that introduces it instead of
being presented as v1's immediate scope:

- **v1 — vertical slice.** Prove the one piece of real architecture that
  matters most: a bounded SPSC ring with genuine backpressure (no drops, no
  busy-waiting), moving fixed-size sequenced messages between two processes,
  end to end, with an automated test proving it. Synchronization is
  deliberately the simplest thing that works — one process-shared mutex, two
  condvars, ordinary mutex-protected cursors — not the fully-optimized
  version. No interactivity, no crash recovery, no rich diagnostics. See
  "v1 build order" in Part II/III below.
- **v2 — robustness and observability.** Layered on top of v1's mutex-based
  ring without changing `ITransport`'s public shape or the ring's core
  concept: `Controller`/signal handling/interactive control, `StatsReporter`,
  checksums and `PacketValidator`, `sessionId`, `PeerClosed` detection, crash
  recovery and stale-segment handling, configurable payload/ring-capacity,
  malformed-frame handling, and the richer `SendResult`/`ReceiveResult`
  enums this document already specifies in full below.
- **v3 — performance.** Purely internal: replace v1's mutex-protected
  cursors with lock-free cross-process atomics, move the mutex off the
  per-message fast path (locked only at the full/empty boundary), add the
  bounded `pthread_cond_timedwait` wait with its accepted missed-notification
  recovery, and the mutex-held lifecycle-transition signal that guarantees a
  prompt wake on clean shutdown. No producer- or consumer-visible API change
  from v2 — v3 optimizes what's already inside `SharedMemoryTransport`.

Part I below is written as the **durable architectural reference** — the
full, final design, with each subsection labeled by the iteration that
introduces it. Where v1's actual behavior differs from the final design
(e.g. plain mutex-protected cursors instead of atomics), that's stated
plainly before describing the v2/v3 upgrade, so v1's own implementers aren't
reading v3-level content as if it's what they're about to build.

### Part I: Architecture

#### Transport guarantees

What `ITransport`/`SharedMemoryTransport` actually promises, stated once so
it isn't left to infer from scattered sections. Unless marked otherwise,
these guarantees hold from v1 onward — they describe the transport's
contract, not a specific iteration's internals:

- **Message-oriented.** Each `send()`/`receive()` transfers exactly one
  framed message (`Header` + fixed-size payload) — never a partial message,
  never multiple messages coalesced into one call. This falls directly out
  of the fixed-slot ring (see "Shared-memory ring layout" below): one
  message per slot. Unlike a byte-stream transport (e.g. raw TCP), the
  caller never has to do its own framing. **[v1]**
- **Ordered — for this single producer/consumer pair, by construction, not
  by resequencing logic.** The SPSC ring's write-cursor/read-cursor
  discipline (producer only appends at the tail, consumer only drains from
  the head — see "Shared-memory ring layout" below) makes reordering
  structurally impossible inside the ring itself. A per-message sequence
  number does not *provide* ordering; it's what lets the consumer *detect*
  a violation of it as a defect (see "Defect handling" below). This
  guarantee is specific to the 1:1 SPSC case — it says nothing about
  ordering across multiple producers or consumers, which is out of scope
  (see each plan's `Explicitly deferred`). **[v1]**
- **Reliable while both processes stay alive — not fault-tolerant across a
  crash.** In the normal running path, no message is silently dropped or
  duplicated: a full ring makes the producer block (backpressure) rather
  than drop (see "Pause semantics" below), and the ring's ordering guarantee
  above rules out duplication. **[v1]** This stops being true across a crash
  or forced kill: an in-flight or buffered message can be lost if either
  process dies mid-transfer, and there is no acknowledgment or
  retransmission layer. **[v2]** The `Stopped`/`PeerClosed` wake results (see
  "Waking a blocked `send()`/`receive()`" below) let a caller *detect* that
  the peer is gone — `Stopped` reliably, `PeerClosed` only best-effort (it
  can miss a producer crash that orphans the process-shared mutex, per that
  section) — and neither recovers or resends anything that was lost. In
  short: reliable-once-delivered, not fault-tolerant. v1 has no `PeerClosed`
  at all (see "Waking a blocked `send()`/`receive()`" below) — a v1 consumer
  blocked on an empty ring, faced with a dead producer, blocks until locally
  told to stop.
- **Blocking, not async/non-blocking — a deliberate design choice.**
  `send()`/`receive()` block the calling thread rather than returning
  immediately with an EAGAIN-style "would block" result. There is no
  polling API, no callbacks, no async completion planned at any iteration.
  This matches the stated efficiency priority of waiting on a condition
  variable instead of spinning — see "Efficiency definition" in Part II.
  **[v1]** The *mechanism* behind the block changes across iterations (plain
  `pthread_cond_wait` under a mutex in v1; bounded `pthread_cond_timedwait`
  with peer-liveness checks in v2; the same bounded wait over a lock-free
  fast path in v3) but the observable behavior — the calling thread blocks,
  it doesn't spin or return early — is a v1 commitment that holds throughout.

#### Two distinct sync mechanisms — do not conflate

This project needs synchronization at two different scopes; they use
different primitives and must not be blurred together. This split is a
**[v1]** decision — it doesn't change across iterations:

| Scope | Used for | Primitive |
|---|---|---|
| **In-process** (threads within one app) | `Controller`'s `Running/Paused/Stopped` state, e.g. control thread signaling the producer/consumer loop — **[v2]**, v1 has no `Controller` | `std::mutex` + `std::condition_variable` — works only within one process, backed by that process's own kernel object |
| **Cross-process** (producer ↔ consumer, separate processes) | Shared-memory ring "slot free" / "data ready" signaling — **[v1]** | A primitive that lives *in shared memory itself* or is otherwise kernel-namespaced across processes — see options below |

A plain `std::condition_variable` is **not** valid for the cross-process case
— it's a per-process abstraction over process-local kernel primitives with no
guarantee of working when placed in shared memory (implementation-defined,
and broken in practice on several platforms). The ring buffer needs one of:

- **Process-shared pthread primitives**: **one** `pthread_mutex_t` and
  **two** `pthread_cond_t` (one per signal — see "Cursor synchronization
  model" below), all three allocated *inside* the shared-memory segment,
  initialized with `pthread_mutexattr_setpshared(PTHREAD_PROCESS_SHARED)` /
  `pthread_condattr_setpshared(PTHREAD_PROCESS_SHARED)` — chosen and
  **verified** (see below), not assumed. **[v1]**
- **Named POSIX semaphores** (`sem_open`), or two unnamed `sem_t` placed in
  the segment: **considered, and technically valid** — this is the standard
  bounded-buffer pattern, two counting semaphores (`freeSlots` initialized
  to `N`, `availableMessages` initialized to `0`), which models exactly the
  two conditions this ring needs (see "Shared-memory ring layout" below). An
  earlier version of this document rejected semaphores on the claim that "a
  plain counter can't express two independent wait conditions" — that claim
  is wrong and is retracted; two semaphores express it directly, one per
  condition. **Every iteration uses process-shared pthread mutex + condvar**,
  but not because semaphores are technically inferior — they're not. The
  actual tradeoff: mutex+condvar carries the same mutex-owner-death exposure
  documented in "Crash recovery scope" below (a process dying while holding
  the mutex wedges it, non-robust), which two semaphores would avoid
  entirely (a semaphore has no "owner" to die mid-hold — `sem_post`/
  `sem_wait` don't lock anything for a process to die while holding). For a
  clean-shutdown-only v1 (per that same section), either primitive is a
  valid choice; mutex+condvar is kept here mainly for symmetry with the
  in-process `Controller`'s `std::mutex` + `std::condition_variable` API
  shape, not because it was proven simpler or safer. Revisit if the
  mutex-owner-death gap needs closing before robust-mutex support lands —
  switching to two semaphores is a legitimate way to close it without
  `PTHREAD_MUTEX_ROBUST`.
- Linux `futex` directly: explicitly **rejected** — Linux-only, and this
  project's initial target is POSIX/macOS-first (per this document). If
  ever needed, it would sit behind a platform adapter, not appear in shared
  application code.

##### Cross-platform verification

`PTHREAD_PROCESS_SHARED` support for `pthread_cond_t` has historically been a
real risk on macOS specifically — glibc (Linux) has supported it reliably for
a long time, but Darwin's libc support is a genuine platform question, not
something to assume from the man page alone. This was verified directly
rather than taken on faith — this verification is a **[v1]** prerequisite,
since v1 already depends on process-shared pthread primitives:

- **macOS** (verified on this machine — macOS 26.2, Darwin 25.2.0, arm64): a
  standalone C test program created a `shm_open`/`mmap` segment, initialized
  `pthread_mutex_t`/`pthread_cond_t` with `PTHREAD_PROCESS_SHARED` inside it,
  `fork()`'d, and had the child block in `pthread_cond_timedwait` while the
  parent slept 200 ms then called `pthread_cond_signal`. The child woke
  promptly with the expected value — a real cross-process wakeup, not just
  successful attribute initialization. Passed.
- **Linux**: not independently re-verified in this pass (glibc's support for
  process-shared pthread mutex/condvar is long-standing and well-documented),
  but should get the same smoke test in CI once the real
  `SharedMemoryTransport` exists, rather than resting on reputation alone.

`SharedMemoryTransport` picks **process-shared pthread mutex + condvar** as
the concrete mechanism, realized as the two independent signals in
"Shared-memory ring layout" below. This keeps the API shape (`wait`/`notify`)
identical to the in-process `Controller` once v2 adds it, so the two are
easy to reason about even though they're backed by different kernel objects.

##### Cross-process atomics: an explicit platform requirement — **[v3]**

v1 does **not** use cross-process atomics — v1's cursors are ordinary
integers, read and written only while holding the mutex (see "Cursor
synchronization model" below for v1's actual baseline). This subsection
describes the v3 upgrade: replacing those mutex-protected plain cursors
with lock-free `std::atomic<uint64_t>`, which is what enables v3's
mutex-free fast path.

The v3 cursors are `std::atomic<uint64_t>` placed in shared memory, and the
entire lock-free fast-path argument (no mutex on `send()`/`receive()`)
depends on one thing the standard does not universally guarantee: that
`std::atomic<uint64_t>`'s operations are **lock-free** on the target
platform. If the standard library's implementation instead backs
`std::atomic<uint64_t>` with an internal lock (legal per the standard when
the platform lacks a native atomic instruction for that width —
`std::atomic<T>::is_lock_free()` exists specifically because this is
implementation-defined), that internal lock is typically a **process-local**
primitive (e.g. a lock striped by object address in the process's own
runtime), not one allocated in and shared via the segment. Two processes
each taking their own process-local lock around "the same" shared memory
word coordinates nothing across processes — each process would serialize
against itself, not against its peer, silently reintroducing a data race
the whole cursor design exists to prevent.

**Requirement, stated explicitly:** `std::atomic<uint64_t>::is_always_lock_free`
must be `true` on every platform this project targets. This is a
compile-time property, not a runtime maybe:

```cpp
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "cross-process cursor requires a lock-free std::atomic<uint64_t>");
```

Placed next to the cursor type definition in `shared_memory_transport.h`, so
a platform or toolchain that can't satisfy this fails the build immediately,
rather than compiling a design that silently doesn't coordinate across
processes. `is_always_lock_free` (not the runtime `is_lock_free()` member
function) is the right check here — it's a compile-time guarantee true for
every instance of the type, matching the static, compile-time nature of the
requirement itself; `is_lock_free()` would only tell you about one already-
constructed object, checked too late and too narrowly for a property the
whole design depends on universally.

**Verification, not just assertion:** the `static_assert` catches "does this
type use a lock internally," but not the more specific cross-process
question — "does a release-store in one process's mapping become visible,
via an acquire-load, in a *different* process's mapping of the same
physical pages." Standard `std::atomic` is specified in terms of the C++
memory model, which is defined over a single program's threads; nothing in
the standard explicitly extends its guarantees across process boundaries,
even though a lock-free atomic is, in practice, implemented as a plain CPU
load/store/RMW instruction with no process-local state, which does work
across shared physical pages on every mainstream platform. This project
does not rest on "in practice" alone — the same empirical-verification
standard already applied to `PTHREAD_PROCESS_SHARED` above applies here:

- **macOS and Linux**: extend the existing `fork()`-based smoke test (see
  "Cross-platform verification" above) with a second case — a
  `std::atomic<uint64_t>` placed in the same `shm_open`/`mmap` segment,
  release-stored by the parent after `fork()`, acquire-loaded in a busy-poll
  loop by the child (bounded, with a timeout, not an infinite spin) — assert
  the child observes the parent's value promptly. This is a cheap addition
  to a smoke test that already exists for the pthread primitives, not a new
  piece of test infrastructure. To be added alongside the pthread smoke test
  in "Testing strategy" below, as part of v3.

#### Pause semantics (cross-process) — **[v1]**

v1 is **1:1** — one producer, one consumer. Multi-consumer fan-out is
deferred indefinitely (see each plan's `Explicitly deferred`) — it isn't
slated for v2 or v3 either.

When the consumer pauses (v2: via interactive `p`/`r`/`q` or the equivalent
of simply not calling `receive()`; v1 has no pause control, but the same
backpressure applies any time the consumer isn't draining, e.g. because it
hasn't started yet or is slower than the producer), the producer **blocks**
(backpressure), not drops:

- Consumer stops draining the shared-memory ring.
- Ring fills up.
- Producer's `send()` blocks on the ring's process-shared condition
  variable (the "slot free" signal described above) — no busy-wait, no
  dropped packets.
- Producer resumes automatically once the consumer resumes and drains.

Rationale: single-reader case, so there's no "slowest consumer" tradeoff to
weigh yet — blocking is simplest and loses no data. Revisit when
multi-consumer fan-out is built, since broadcast-to-many changes the
tradeoff (one slow reader would stall everyone).

This is a **different mechanism** from the in-process `Controller` pause
(stdin/signal → `Running/Paused/Stopped`, **[v2]**), which uses a plain
`std::condition_variable` local to each process — see "Control loop and
signal safety" below. The cross-process ring signal and the in-process
controller signal never share a primitive.

#### Shared-memory ring layout

This is a design decision, not an implementation detail — it drives
throughput, memory layout, backpressure, and testing. The ring's existence
and core shape are **[v1]**; several of the fields below (`sessionId`,
runtime-configurable `payloadSize`/`ringCapacityBytes`) are **[v2]** and are
marked as such.

- **Bounded SPSC ring buffer** — single producer, single consumer (matches
  the 1:1 v1 scope). Producer appends packets at the **tail** (its write
  cursor) continuously, with no per-message wait for the consumer; consumer
  reads from the **head** (its read cursor) one packet at a time. Producer
  only blocks when it has caught up to the consumer and the ring is
  genuinely full — not on every send. **[v1]**

  Contrast with a single-slot handshake, which would force a wait on *every*
  message: producer writes the one slot, then must wait for the consumer to
  read it before writing the next — no tail/head separation, no overlap. The
  ring's N slots are what let producer keep appending while consumer is
  still reading an earlier slot. **This is the one piece of "real"
  architecture v1 exists to prove** — a single-slot handshake would produce
  a working demo, but would not establish the SPSC ring as the actual
  synchronization mechanism this project is built around, which is the
  entire point of doing a vertical slice instead of a toy.
- **Fixed slot capacity, chosen at startup, driven by `Config`** — no
  dynamic resize once the segment is created. Two independent config
  values, both **fixed constants in v1** (no CLI flags yet — see "v1
  simplifications" below), becoming runtime-configurable in **[v2]**:
  - `payloadSize` — the **exact** payload size of every message for the
    lifetime of a run, not a maximum a message may be under. **No
    iteration has variable-length messages**: every `send()`/`receive()`
    call moves exactly `payloadSize` bytes of payload, always. v1 hardcodes
    this to **1024 bytes**; v2 exposes it via `--payload-size` (validated
    `> 0`). Cannot change while a producer/consumer pair is running;
    changing it means restarting both processes (this is also why
    `layoutVersion`/attach-time checks matter, from v2 onward — a consumer
    attaching with a different `payloadSize` expectation than the producer
    used to size the segment is exactly the mismatch that check exists to
    catch). This is a deliberate naming/semantics decision, not an
    oversight: the alternative (`maxPayloadSize`, with `header.payloadSize`
    allowed to vary up to it) would need a real reason to exist — no
    caller at any iteration produces anything but a fixed-size payload —
    and would reopen the malformed-frame question below for a feature
    nothing here uses. If variable-length messages become a real
    requirement later, that's a new field and a new check, not a
    reinterpretation of this one.
  - `ringCapacityBytes` — total shared-memory segment size. v1 hardcodes
    this to **8 MiB**; v2 exposes it via `--ring-capacity`. The segment
    holds more than just slots: a **control block** at the front (in v1:
    a "does the segment exist and is it the right size" marker, the
    process-shared `pthread_mutex_t`/`pthread_cond_t` pair, and the two
    cursors; from v2 onward: also `state`, `layoutVersion`, producer PID —
    see "Lifecycle and ownership" below) precedes the slot array. Slot
    count is derived at startup as:
    ```text
    slotStride = alignUp(sizeof(Header) + payloadSize, alignof(std::max_align_t))
    slotCount  = (ringCapacityBytes - controlBlockSize) / slotStride
    ```
    Both terms matter: `controlBlockSize` (computed from `sizeof` of the
    actual control-block struct, not hand-counted) is subtracted before
    dividing, since those bytes hold no slots; and `slotStride` is the
    *aligned* per-slot size, not the raw `sizeof(Header) + payloadSize` —
    a slot must start at an alignment boundary each cursor advance can rely
    on, so any padding needed to reach that boundary is folded into the
    stride once, not left to accumulate misaligned offsets slot-to-slot.
    With v1's minimal `Header` (see "Wire format" below — just
    `sequenceNumber` + `payloadSize`, 12 bytes before alignment) and the
    1024-byte payload, `slotStride` is small overhead over 1024 bytes. At
    8 MiB and roughly 1024–1040 bytes/slot, that's still on the order of
    8000 slots, plenty of headroom to absorb producer/consumer speed
    mismatches without blocking constantly. Chosen over 16 MiB only because
    a smaller default footprint costs nothing and is trivially raised once
    benchmarking shows it's too small for a given payload size/rate — this
    is a tuning knob, not an architectural constraint. (v2's fuller
    `Header` — with `sessionId`, `timestamp`, `checksum` — is 32 bytes; see
    "Wire format" below for both shapes.)
  ```cpp
  // v1
  struct Config {
    static constexpr std::size_t payloadSize = 1024;
    static constexpr std::size_t ringCapacityBytes = 8 * 1024 * 1024;
  };
  ```
  ```cpp
  // v2: same fields, now runtime-configurable
  struct Config {
    std::size_t payloadSize = 1024;
    std::size_t ringCapacityBytes = 8 * 1024 * 1024;
  };
  ```
- **Producer advances the write cursor, consumer advances the read
  cursor** — during steady-state operation, each process only ever advances
  its own cursor and only ever reads the other's; no cursor is advanced by
  both sides. **[v1]** (Initial zeroing of *both* cursors is a
  producer-startup concern, not a steady-state one — see "Lifecycle and
  ownership" below.)
- **Ordered and lossless by construction** — the SPSC ring never reorders or
  drops a slot it has accepted. A sequence gap or reorder observed by the
  consumer therefore indicates an implementation or protocol defect (a bug
  in the ring, wraparound math, or the wire format), not expected operating
  data. **[v1]** v1's fail-fast policy (see "v1 error handling" below) treats
  any such gap as an immediate hard error; v2 adds the fuller
  `PacketValidator`/`errorCount`/keep-consuming policy — see "Defect
  handling" below.

##### Cursor synchronization model

This is the exact concurrency model — stated explicitly because "a mutex,"
"two condition variables," and (from v3) "atomic cursors" don't compose
into one unambiguous design on their own. Getting this wrong changes
visibility, risks a lost wakeup, and decides whether producer and consumer
can ever touch the same slot concurrently.

**v1 baseline — mutex-protected plain cursors, locked on every call:**

- **Cursors are ordinary `uint64_t`, not atomics.** Each cursor (write
  cursor, read cursor) lives in the control block as a plain integer. There
  is no lock-free fast path in v1 — this is a deliberate simplification,
  not a placeholder: v1 exists to prove the ring's *shape* (bounded SPSC,
  backpressure, no drops), not to prove its final performance
  characteristics, which is v3's job.
- **v1's `send()`/`receive()` take the mutex on *every* call, not only at
  the full/empty boundary.** Both cursors, and the full/empty predicate
  derived from them, are read and written only while holding the single
  `pthread_mutex_t`. This is a real, deliberate behavior difference from
  the v3 end state (see "v3: lock-free fast path" below), not merely
  "simpler internals that happen to behave the same" — v1's `send()` always
  locks, checks "is the ring full," writes the slot and advances the write
  cursor if not full (or waits on the "slot free" condvar if it is), then
  unlocks. `receive()` is symmetric. No atomics, no lock-free reasoning, no
  memory-order annotations needed at this iteration — the mutex is the only
  synchronization primitive the ring's data path relies on.
- **Two condition variables**, exactly as in the final design — this part
  doesn't change across iterations:
  - **"slot free"** (consumer → producer): producer waits on this when the
    ring is full; consumer signals it after advancing the read cursor.
  - **"message available"** (producer → consumer): consumer waits on this
    when the ring is empty; producer signals it after advancing the write
    cursor.

  Collapsing these into a single condvar/predicate works but forces every
  waiter to re-check the full ring state on each wake; two signals let each
  side wait on exactly the condition it cares about. One mutex is enough
  for both, since only one side is ever actually blocked waiting at a time
  in the steady-state 1:1 case — a second mutex would add nothing.
- **v1 has no lost-wakeup risk, by construction.** Because every cursor
  read/write and every signal happens while holding the same mutex, this is
  the textbook mutex-guarded-predicate pattern: a waiter's predicate check
  and its `pthread_cond_wait` call happen atomically with respect to the
  mutex, so a notifier's signal can never land in an unobservable gap
  between them. This guarantee is **free in v1** and is exactly what v3's
  lock-free fast path gives up (see below) in exchange for throughput —
  v1 doesn't need to make that trade yet.
- **Blocking only at full/empty** — producer blocks only when the ring is
  full (all `N` slots occupied); consumer blocks only when the ring is empty
  (write cursor caught up to read cursor). No busy-waiting: waits are
  ordinary `pthread_cond_wait` (v1) or, from v2 onward, bounded
  `pthread_cond_timedwait` — see "Waking a blocked `send()`/`receive()`"
  below.

##### v3: lock-free fast path — cross-process atomics

This subsection describes the v3 upgrade over the v1 baseline above; it is
**not** what v1 or v2 implement.

- **Cursors become `std::atomic<uint64_t>`, not plain integers guarded only
  by the mutex** — and this is only valid given the platform requirement in
  "Cross-process atomics: an explicit platform requirement" above
  (`is_always_lock_free`, verified, not assumed). Each cursor lives in the
  control block as an atomic, each on its own cache line (`alignas(64)`,
  padded apart — two hot atomics sharing a cache line would false-share
  between the producer and consumer processes, which run on different cores
  in the common case). The mutex is **no longer required** to read or
  advance a cursor on the fast path; see the publication rule below for
  what *is* required instead.
- **Fast path never takes the mutex.** `send()` writes its slot, then
  atomically stores the incremented write cursor — no lock. `receive()`
  reads (copies) the slot, then atomically stores the incremented read
  cursor — no lock. The mutex is only acquired around a
  `pthread_cond_timedwait` call, and only when the ring is actually
  full/empty — not on every single `send()`/`receive()`. This is the
  behavior change from v1/v2's "lock every call" baseline.
- **Publication rule — when a written slot becomes visible.** The producer
  writes the full slot (header + payload bytes) *before* it stores the
  incremented write cursor, and that store uses `std::memory_order_release`.
  The consumer loads the write cursor with `std::memory_order_acquire`
  before reading any slot bytes. This release/acquire pair is what makes
  the slot's contents visible to the consumer's read — without it, the
  consumer could observe the new cursor value while still seeing stale or
  torn slot bytes, since cursor and slot are otherwise independent memory
  locations with no ordering guarantee between them. A written slot is
  "visible" exactly when the consumer's acquire-load of the write cursor
  observes the producer's release-store.
- **Consumption rule — when a consumed slot becomes reusable.** Symmetric:
  the consumer finishes reading the slot *before* it stores the incremented
  read cursor with `memory_order_release`; the producer loads the read
  cursor with `memory_order_acquire` before treating that slot as free to
  overwrite. A slot is safe for the producer to reuse exactly when its
  acquire-load of the read cursor observes the consumer's release-store
  advancing past it — never before, or the producer could overwrite a slot
  the consumer is still mid-read on.
- **No concurrent access to the same slot.** Given the two rules above, the
  producer only ever writes a slot after its acquire-load of the read
  cursor shows the consumer is done with it, and the consumer only ever
  reads a slot after its acquire-load of the write cursor shows the
  producer finished writing it. The two processes never touch the same
  slot at the same time — this is what the cursor discipline is *for*, not
  an incidental property. (In v1/v2, the mutex provides this same guarantee
  more simply, by serializing all access rather than by ordered atomics.)
- **Missed notifications become possible and accepted — the timed wait is
  the recovery, not a backstop for an otherwise-airtight design.** An
  earlier version of this document claimed "no lost wakeup" for this
  lock-free design; that claim was wrong and is retracted. The standard
  mutex-guarded-predicate pattern only guarantees no lost wakeup when the
  *notifier* also holds the mutex across its state change and signal. v3's
  fast path deliberately does not: `send()`/`receive()` update the atomic
  cursor and call `pthread_cond_signal` **without** taking the mutex (that's
  the whole point of keeping cursors lock-free). That leaves a real window:
  a waiter can check the predicate (ring still full/empty), find nothing to
  do, and be preempted *before* entering `pthread_cond_timedwait` — if the
  notifier's cursor update and signal land in exactly that window, the
  signal is sent to no one currently waiting on the condvar, and is lost.
  This is a genuine consequence of keeping the notification unsynchronized
  with the mutex, not a corner case introduced by a bug. **v1 does not have
  this problem** (see "v1 has no lost-wakeup risk, by construction" above)
  — it's a cost v3 accepts specifically in exchange for the lock-free fast
  path.
- **Why this is fine anyway: the bound is already accepted elsewhere.**
  `send()`/`receive()` use `pthread_cond_timedwait` with a short bound
  (~100–200 ms — see "Waking a blocked `send()`/`receive()`" below), already
  adopted (from v2) for local-shutdown and peer-death detection, not for
  this race. A missed notification here costs at most one extra
  wait-timeout cycle: the waiter times out, re-checks the (by-then-updated)
  predicate outside the missed-signal window, and proceeds immediately if
  the condition is now met. Worst case added latency is one wait
  granularity (~100–200 ms), not a permanent stall.
- **Chosen contract: accept the miss, let the timed wait recover.** Of the
  three ways to close this gap — (a) have the notifier take the mutex
  around its cursor update and signal, restoring the classic no-lost-wakeup
  guarantee at the cost of putting the mutex back on the fast path for
  every `send()`/`receive()` (this is exactly v1/v2's behavior — "restoring"
  is really "not making the v3 trade at all"); (b) accept the miss and rely
  on the bounded timed wait, as described above; (c) switch to counting
  semaphores (`sem_post`/`sem_wait`), whose kernel-tracked count retains a
  post that arrives before a matching wait, closing the gap structurally
  rather than by timeout — v3 takes **(b)**. It costs nothing new (the
  bound already exists from v2 for other reasons) and preserves the
  lock-free fast path, which is the reason cursors become atomics in v3 in
  the first place. (a) would undo that; (c) is a legitimate future
  alternative — see "Named POSIX semaphores" in "Two distinct sync
  mechanisms" above, which already notes semaphores are technically valid
  and were kept out only for reasons unrelated to this specific gap.

This layout is shared code (`src/common/transport/shared_memory_transport.{h,cpp}`)
at every iteration — producer and consumer link the same ring implementation,
one opens/creates it, the other opens the existing segment.

##### Waking a blocked `send()`/`receive()` on shutdown or peer death

**v1 baseline:** a blocked `send()`/`receive()` uses a plain
`pthread_cond_wait` (no timeout) under the mutex. It wakes only when the
predicate is met (slot free / message available) or on a spurious wake (in
which case it re-checks the predicate and waits again — ordinary condvar
usage, no special handling needed). **v1 has no notion of "local shutdown
while blocked" or "peer death while blocked"** — there is no `Controller`
to signal a local stop (see "Control loop and signal safety" below —
**[v2]**), and no PID-liveness check for a dead peer. A v1 producer blocked
on a full ring with a dead or exited consumer, or a v1 consumer blocked on
an empty ring with a dead or exited producer, simply stays blocked — this
is accepted v1 scope (see "v1 error handling" below): v1's fail-fast
posture is about *malformed data*, not about *peer liveness*, which is a
v2 concern.

**From v2 onward**, this is no longer acceptable — v2 adds interactive
pause/resume/quit and needs to wake blocked calls on both a local stop
request and (best-effort) a dead peer:

A blind `pthread_cond_wait` only wakes on its one predicate (slot-free /
message-available). It has no way to observe "local shutdown was requested"
or "the peer process died" — both are real deadlock risks once interactive
control exists, not edge cases. **This mechanism is asymmetric by design**:
only the producer's PID and lifecycle state are stored in the segment (see
"Lifecycle and ownership" below), so only the *consumer* can detect a dead
or quit *producer* this way. The reverse — producer detecting a dead or quit
consumer — is out of scope even in v2; see "Crash recovery scope" below for
why, and the `SendResult` note beneath the enum definition for what this
means for `send()`'s return value.

- **Local shutdown while blocked**: producer's `send()` is waiting because
  the ring is full; user presses `q` / sends `SIGINT`. `Controller` flips to
  `Stopped`, but that's the *in-process* condition variable (see "Two
  distinct sync mechanisms" above) — nothing wakes the thread blocked on the
  *ring's* cross-process condvar. `send()` needs its own bounded-wait check
  for this (below), just like `receive()` does.
- **Peer death while blocked**: consumer's `receive()` is waiting for
  "message available"; producer crashes. A crash runs no cleanup —
  `pthread_cond_signal` never fires again. Consumer blocks forever without
  the check below.

**Fix: bounded wait, not blind wait — [v2].** `send()`/`receive()` use
`pthread_cond_timedwait` on a short bound (e.g. 100–200 ms), not
`pthread_cond_wait`. On every wake — whether the predicate was met, the
timeout elapsed, or a spurious wake occurred — check, in order:

1. Predicate met (slot free / message available)? → proceed, done.
2. **`receive()` only:** ring still empty (predicate not met) **and**
   segment `state` is `Stopping` or `Closed`? → return an `EndOfStream`
   result, stop waiting. See "Clean producer shutdown and the receive
   predicate" below — this is a distinct, expected outcome of a producer
   finishing normally, not an error, and is checked *before* the local-
   `Stopped` and `PeerClosed` checks below because it's the more specific,
   more informative condition when it applies (the producer isn't dead or
   locally told to stop — it published a graceful end).
3. Local `Controller` state is `Stopped`? → return a `Stopped` result,
   stop waiting.
4. **`receive()` only:** producer's PID (stored in the segment — see
   "Lifecycle and ownership" below) no longer alive (`kill(pid, 0)` fails
   with `ESRCH`)? → return a `PeerClosed` result, stop waiting. `send()`
   has no equivalent check — there is no stored consumer PID for it to test
   (see "Crash recovery scope" below) — so this step is skipped entirely
   on the producer side.
5. None of the above → recompute the deadline, wait again.

This reuses the PID-liveness check already designed for lifecycle/crash
recovery, just invoked periodically during a blocked wait instead of only at
process startup, on the consumer side only. Worst-case wake latency on
consumer-observed producer shutdown or crash is bounded by the wait
granularity (~100–200 ms) — **but only best-effort, not guaranteed, and
only for producer crashes that don't orphan the mutex.** A blocked
`send()` has no equivalent bound on peer death: it wakes on `Stopped` (its
own `Controller`) but not on a dead consumer, clean-quit or crashed alike.

**Why `PeerClosed` is best-effort, not guaranteed:** step 4 above only runs
if `pthread_cond_timedwait` actually returns control to the caller. That
function requires the mutex to be held on entry and **re-acquires it
internally** before returning — on timeout, on spurious wake, and on a real
signal alike (see the POSIX contract: `pthread_cond_timedwait` always
returns with the mutex locked). If the producer died while it held that
same mutex — e.g. between locking it and unlocking it inside its own
`send()` — the mutex is left permanently locked (see "The process-shared
mutex is not robust" in "Crash recovery scope" below). A consumer's
`pthread_cond_timedwait` call, needing to re-lock that same orphaned mutex
on its way back out, blocks on the *mutex* — never reaching step 4, never
returning `PeerClosed`, regardless of the bounded timeout requested. The
bound only holds when the wait unblocks cleanly; it does not hold across
every crash shape. So: `ReceiveResult::PeerClosed` is reachable and correct
when the producer dies without orphaning the mutex (e.g. between messages,
not mid-`send()`), but is not a guarantee for every producer crash — a
crash that orphans the mutex wedges the consumer with no bounded escape,
at any iteration. Full detection needs either `PTHREAD_MUTEX_ROBUST`
(`EOWNERDEAD` handling on relock) or a different cross-process primitive
that doesn't have this failure mode (e.g. two counting semaphores — see
"Two distinct sync mechanisms" above); neither is implemented at any
planned iteration — this is future work beyond v3.

This is also why, from v2 onward, `ITransport::send()`/`receive()` return a
small result enum, not a plain `bool` — but the two enums are **not
symmetric**, reflecting the asymmetric check above. **v1 uses plain `bool`**
(see "v1 error handling" below); the enum shape below is a v2 addition:

```cpp
// v2
enum class SendResult { Sent, Stopped };
enum class ReceiveResult { Received, Stopped, PeerClosed, Malformed, EndOfStream };
```

`Sent`/`Received` mean the predicate was met and the operation completed
normally; `Stopped` means the local `Controller` transitioned to `Stopped`
while waiting; `PeerClosed` (receive-side only, **best-effort** — see above)
means the producer's PID liveness check failed while waiting and the wait
call was actually able to return and run that check. `Malformed`
(receive-side only) means the slot's `header.payloadSize` did not equal the
configured exact payload size — see "Malformed-frame policy" in
"Shared-memory ring layout" above for the full rationale; it is called out
here only to keep the enum definition and its cases in one place.
`EndOfStream` (receive-side only) means the ring was empty and the segment's
lifecycle `state` was `Stopping` or `Closed` — the producer finished
normally and there is nothing left to drain — see "Clean producer shutdown
and the receive predicate" in "Lifecycle and ownership" below. Unlike
`PeerClosed`, `EndOfStream` is not a failure or a best-effort detection: it
is the expected, designed-for outcome of a consumer draining a producer
that exited cleanly. `send()` has no `PeerClosed` case: a producer blocked
on a full ring, with the consumer already dead or cleanly exited, wakes
only on its own `Controller` state — not automatically on consumer absence.
This is a real limitation at every iteration (see "Crash recovery scope"
below), not an oversight papered over by the enum shape. Both enums are
returned by value, fixed underlying `int`, no allocation — consistent with
the fixed-size, no-heap-per-call constraints the rest of the transport
already holds itself to. Neither enum covers OS-level failures
(`shm_open`/`mmap` errors, an unexpected `pthread_cond_timedwait` errno) or
the `Malformed` case's escalation to process exit — see "Fatal-error
policy" below for how those are handled instead of being folded into these
two blocking-outcome enums.

##### Fatal-error policy

**v1: `send()`/`receive()` return plain `bool`** — `true` on success,
`false` on any fatal condition (transport construction failure, a
sequence-number gap, an OS-level error). `ProducerApp`/`ConsumerApp` (see
"Shared directory layout" below) check the return value, log a clear
message, and exit non-zero — no exceptions thrown across the `ITransport`
boundary, no result enum, no structured error type. This is a deliberate
minimal starting point, not a placeholder that happens to be small: the
richer policy below is what v2 replaces it with, once there's an actual
need (interactive control, best-effort peer detection, malformed-frame
recovery) to distinguish more than "worked" / "didn't."

**From v2 onward:** `SendResult`/`ReceiveResult` exist to describe
**blocking-wait outcomes** — what happened while a call was waiting on a
condition — not the full space of things that can go wrong in the
transport. Two other categories of failure exist and need a stated policy,
or the two enums above quietly read as covering more than they do:

- **OS-level failures**: `shm_open`/`mmap`/`ftruncate` returning an error,
  `pthread_mutex_init`/`pthread_cond_init` failing (e.g. the platform
  silently not supporting `PTHREAD_PROCESS_SHARED` despite the check in
  "Cross-platform verification" above), or `pthread_cond_timedwait`
  returning an errno other than `ETIMEDOUT` (a genuine EINVAL/EPERM-class
  misuse, not a normal wake path). These are **programmer or environment
  errors**, not conditions a caller should be expected to branch on and
  recover from at the call site.
- **Protocol failures**: `layoutVersion` mismatch at attach (see "Lifecycle
  and ownership" below) and the mismatched-`payloadSize` `Malformed` case
  above. These indicate the two processes disagree about the wire format or
  the ring is corrupted — not something the running process can safely
  paper over and continue.

**Policy from v2 onward: both categories are fatal — log clearly, then exit
non-zero — but the transport itself never calls `exit()`/`abort()`/
`std::terminate()`, and the split between "detect" and "act on" is
explicit:**

- **`SharedMemoryTransport` (and any other `ITransport` implementation)
  reports failure through an ordinary return value, never by unilaterally
  ending the process.** OS-level failures during construction/attach (a
  constructor that can't succeed, or an explicit `open()`/`attach()`-style
  factory returning failure) and protocol failures like `layoutVersion`
  mismatch are surfaced as a simple result the caller inspects — e.g. a
  `std::expected<SharedMemoryTransport, TransportError>`-shaped construction
  path, or an explicit `enum class TransportError { OsFailure,
  LayoutMismatch }` alongside a logged detail string — not as a thrown
  exception (consistent with "no exceptions thrown across the `ITransport`
  boundary" below) and never as a direct `exit()` call from inside transport
  code. This keeps the transport unit-testable in isolation: a test can
  construct a transport against a deliberately-broken segment and assert on
  the returned error, without that assertion requiring a subprocess to
  observe an `exit()` call that already tore down the process under test.
  This same "the transport reports a value, never exits itself" principle
  applies in v1 too, even with only a `bool` to report it — v1's `false`
  return is the same detect/act-on split, just with a coarser signal.
- **`ProducerApp`/`ConsumerApp` own logging and the final non-zero exit, at
  every iteration.** These are the orchestration layer — the thing
  `producer_main.cpp`/`consumer_main.cpp` actually construct and run — and
  are where "log clearly, then exit non-zero" actually happens: they call
  into the transport, inspect the result, and on failure log the detail and
  return a non-zero status from `main()` (or call `std::exit` themselves, at
  the top of the call stack, not buried inside infrastructure code). Same
  split applies to the `Malformed` `ReceiveResult` case above (v2):
  `receive()` returns `Malformed` (a value, not a thrown error and not a
  process exit), and it is `ConsumerApp`'s consumer loop — not
  `SharedMemoryTransport` — that logs it and exits non-zero.
- **The exact error type can stay simple** — this is not a request for a
  rich exception hierarchy or an `std::error_code`-style extensible
  registry. A small enum plus a logged string is enough to preserve the
  detect/act-on split; the split itself is the requirement, not the
  sophistication of the error type carrying it.

No exceptions thrown across the `ITransport` boundary, no `errno`-style
out-parameter threaded through every call, no attempt to retry or
self-heal, at any iteration. This is deliberately the same shape as the
existing `layoutVersion`-mismatch handling and the `Malformed` handling
above — this is the minimum needed so "what happens on a malformed frame or
an OS failure" has one clear, stated answer instead of being left to
whatever an unhandled exception or an ignored return code happens to do.

#### Wire format: `Header`/`Message` layout

`Header` and the per-slot layout are copied byte-for-byte between processes
via shared memory — that only works if the layout is explicitly controlled,
not left to "whatever the compiler happens to do." **v1 uses a minimal
`Header`**; v2 adds the fields described further below.

- **`Header` must be trivially copyable**, fixed-size, and use only
  fixed-width integer types (`uint64_t`/`uint32_t`, no `size_t`, no `bool`,
  no enums without an explicit underlying type). Enforce with
  `static_assert(std::is_trivially_copyable_v<Header>)` right next to the
  definition, so a future edit that breaks this fails to compile instead of
  failing silently at runtime. This rule holds at every iteration.

  **v1 `Header`** — just enough to prove ordering and framing:
  ```cpp
  struct Header {
    std::uint64_t sequenceNumber;
    std::uint32_t payloadSize;
  };
  static_assert(std::is_trivially_copyable_v<Header>);
  static_assert(sizeof(Header) == 16);  // 8 + 4, padded to 8-byte alignment
  static_assert(offsetof(Header, sequenceNumber) == 0);
  static_assert(offsetof(Header, payloadSize) == 8);
  ```
  No checksum, no `sessionId`, no `timestamp` in v1 — see "v1
  simplifications" below for why each is deferred.

  **v2 `Header`** — adds session disambiguation, a timestamp field, and a
  checksum:
  ```cpp
  struct Header {
    std::uint64_t sessionId;
    std::uint64_t timestamp;
    std::uint64_t sequenceNumber;
    std::uint32_t payloadSize;
    std::uint32_t checksum;
  };
  static_assert(std::is_trivially_copyable_v<Header>);
  static_assert(sizeof(Header) == 32);
  static_assert(offsetof(Header, sessionId) == 0);
  static_assert(offsetof(Header, timestamp) == 8);
  static_assert(offsetof(Header, sequenceNumber) == 16);
  static_assert(offsetof(Header, payloadSize) == 24);
  static_assert(offsetof(Header, checksum) == 28);
  ```
  All members in both shapes are naturally aligned with no padding beyond
  what's noted, so `sizeof` is stable today. **Trivial copyability alone
  does not guarantee this stays true** — it says the bytes can be copied
  safely, not that the compiler picked any particular size or member order.
  A field added, reordered, or widened later could introduce padding or
  change `sizeof` without breaking `is_trivially_copyable_v`. The explicit
  `sizeof`/`offsetof` asserts above are what actually pin the layout: a
  future edit that shifts it fails to compile here, instead of silently
  desyncing producer and consumer (who each compile `Header` independently
  and must agree on its byte layout, since it's copied raw across the
  shared-memory boundary, not serialized). This applies at both the v1 and
  v2 shapes — bumping from one to the other is itself exactly the kind of
  layout change `layoutVersion` (see "Lifecycle and ownership" below,
  **[v2]**) exists to catch.
- **`sessionId`** — **[v2]**. Identifies one producer run — see "Session ID:
  distinguishing restart from loss" below for why this exists and how the
  consumer uses it. v1 has no restart disambiguation at all: a v1 consumer
  has no way to distinguish a producer restart from data loss, because v1
  has no crash-recovery story in the first place (see "v1 simplifications"
  below) — the field simply doesn't exist yet.
- **`timestamp`'s clock, unit, and capture point are deliberately left
  undefined even once introduced in v2 — the field is reserved, not yet
  interpreted.** Nothing at v2 reads `timestamp` for any decision (not
  `PacketValidator`, not `StatsReporter`); its only role is as one of the
  fields the checksum protects (see "Checksum: algorithm and scope" below),
  where its *presence and stability* matter but its *meaning* does not. The
  producer populates it with `std::chrono::steady_clock::now()`'s count at
  the point it builds the header — some value must go in the field for the
  checksum to cover, and `steady_clock` is the safe, non-committal default
  given the monotonic-clock preference established elsewhere in this
  document (see `StatsReporter`'s use of `steady_clock` over `system_clock`
  in Part III) — but neither the specific clock, nor the unit (ticks vs. ns
  vs. ms), nor "producer-build-time vs. some other capture point" is a
  committed contract yet. **Latency measurement is what would actually need
  this field defined precisely** (producer-timestamp vs. receive-timestamp,
  a consistent clock/unit/capture-point contract both processes agree on),
  and that feature is deferred beyond v2 (see "Explicitly deferred
  (consumer)" in Part III). Defining `timestamp`'s semantics ahead of that
  feature actually needing them risks committing to a contract before the
  requirement is known — pin it down when latency measurement is actually
  built, not before.
- **`Message` drops `std::vector<std::byte> payload` entirely, at every
  iteration** — a vector owns a heap pointer valid only in its own process's
  address space; copying that struct into shared memory hands the consumer
  a pointer that's garbage in its own address space. Instead, `Message`
  pairs a `Header` with a non-owning `std::span<std::byte>` into a buffer
  the *caller* — producer or consumer — owns and reuses across calls:
  ```cpp
  struct Message {
    Header header;
    std::span<std::byte> payload;  // points into a caller-owned buffer, not into shared memory
  };
  ```
  Nothing containing a pointer or a container is ever placed *in* shared
  memory itself — only the raw `Header` bytes and payload bytes are, and
  only via an explicit copy in/out (see "`send()`/`receive()` contract"
  below).
- Same rule applies to any future struct placed in shared memory: no
  pointers, no containers, no virtual functions — trivially copyable and
  fixed-size, checked with `static_assert`, or it doesn't go in the ring.

##### `send()`/`receive()` contract: one copy in, one copy out

**Every iteration uses caller-owned buffers and a single `memcpy` per
direction — not a zero-copy borrowed view, at v1, v2, or v3.** An earlier
draft of this design had `receive()` return a `MessageView` borrowed
directly from the ring slot, with the read cursor advancing only on an
explicit `release(view)` call. That was rejected before implementation: it
added a manual lifetime contract (forgetting to `release()` deadlocks the
producer, since the slot is never freed), a transport-specific `release()`
concept with no precedent elsewhere in this design, a genuine
use-after-free hazard if `view` is read after `release()`, and —
decisively — no matching story for the producer side, which still needed
*some* reserve/write/publish sequence to build directly into a slot it
"already owned." That gap was hand-waved rather than designed. None of this
is worth taking on before the 1:1 happy path even works, and v3's
performance work (see above) targets the mutex, not this copy — a
zero-copy borrowed-slot API remains a valid *future* optimization beyond
v3, not something any currently-planned iteration needs.

Instead, at every iteration:

- **`send(const Message&)`** first checks `header.payloadSize ==
  Config.payloadSize` — payloads are exact-size, not "up to a maximum" (see
  "Fixed slot capacity" above), so anything other than equality is already
  a defect, not a smaller-but-valid message. The producer builds the header
  itself, so this should never fail in practice, but the check exists so a
  bug upstream can't silently write a malformed frame into the ring for
  some future consumer to trip over. **v1** treats a failure here as a
  fail-fast `bool` `false` (see "v1 error handling" below); **v2** returns
  `ReceiveResult::Malformed` on the receiving side specifically (see
  "Malformed-frame policy" below for the fuller v2 policy). Only after that
  check passes does it copy `header` and `payloadSize` bytes of `payload`
  into the next free slot (waiting on "slot free" per "Shared-memory ring
  layout" above if the ring is full), then advance the write cursor and
  signal "message available." The caller's `Message`/buffer is free to
  reuse or discard the instant `send()` returns — no lifetime tie to the
  slot at all.
- **`receive(Message&)`** waits on "message available" if the ring is
  empty, then reads the slot's `Header` **first, alone** — before touching
  the payload bytes at all — and checks `header.payloadSize ==
  Config.payloadSize` against the same configured exact size. **Only if
  that check passes** does it proceed to copy `payloadSize` bytes of
  payload out of the slot into the caller-supplied `Message`'s buffer,
  advance the read cursor, and signal "slot free." This ordering is
  deliberate and not optional, at every iteration: `payloadSize` is
  attacker-and-corruption-controlled data arriving from shared memory, not
  a trusted local value, and a corrupted or garbage `payloadSize` used
  directly as a copy length before any check is an out-of-bounds read from
  the slot (and a potential out-of-bounds *write* if the caller's buffer
  was sized to the expected `payloadSize` rather than defensively larger).
  A v2 `PacketValidator`'s checksum/sequence checks (see Part III) run
  **after** this point and cannot substitute for it — they operate on a
  `Message` that, by their point in the pipeline, has already been fully
  copied; the payloadSize bound-check has to happen inside `receive()`
  itself, before the copy, or it happens too late to matter. **v1** has no
  `PacketValidator` at all — the bound check inside `receive()` plus a
  sequence-number check in the consumer loop (see "v1 error handling"
  below) is the entirety of v1's validation, and that's sufficient, since
  the bound check is what actually prevents the unsafe read regardless of
  what runs afterward. There is no separate release step for the success
  path, at any iteration; by the time a successful `receive()` returns, the
  slot is already free for the producer to reuse.
- **Buffer reuse is the caller's responsibility, not the transport's** — this
  is what keeps `send()`/`receive()` allocation-free per call. The producer
  keeps one reusable payload buffer across sends; the consumer keeps one
  reusable buffer across receives. Neither transport method allocates; both
  work purely by copying into/out of a buffer the caller already owns. This
  makes `send()` and `receive()` symmetric in a way the borrowed-view design
  was not: both copy into/out of caller memory, no special-casing on either
  side.
- **Cost accepted:** one `memcpy` of exactly `payloadSize` bytes per `send()`
  and per `receive()`, at every iteration. This trades away part of
  priority 1 in "Efficiency definition" below (no unneeded copies) for
  correctness and a much smaller implementation surface — worth it given
  the actual bottleneck at typical payload sizes is still the cross-process
  wait/wake path, not a bounded memcpy.

##### Malformed-frame policy — **[v2]**

**v1 does not have this section's machinery** — v1's `receive()` still does
the same pre-copy `payloadSize` bound-check described above (that part is
not optional at any iteration, since it's what prevents an unsafe read),
but on failure it simply returns `bool` `false` and lets
`ProducerApp`/`ConsumerApp` log-and-exit per "v1 error handling" below.
Everything below this point — the dedicated `Malformed` result, the
detailed rationale for why this differs from an ordinary checksum/sequence
defect — is v2 scope, layered on top of the same underlying check.

A frame whose `header.payloadSize` does not equal `Config.payloadSize`
(checked in `receive()` above, before any payload copy — payloads are
exact-size, so any mismatch, over *or* under, is a defect, not a shorter
valid message) is a different class of problem than a checksum mismatch or
sequence gap (see "Defect handling" below): it isn't safely copyable at
all, so there is no valid `Message` for `PacketValidator` to inspect and no
meaningful way to "keep consuming and log it" the way an in-bounds-but-
corrupted frame allows.

**Policy: this is a transport-level fatal error, not a protocol defect and
not a silent crash.** Concretely:

- `receive()` does not attempt the payload copy, does not advance the read
  cursor, and does not signal "slot free" — the ring is left exactly as it
  was found, since nothing was actually consumed.
- `receive()` returns a distinct `ReceiveResult::Malformed` (added
  specifically for this — see "Fatal-error policy" above for how this
  relates to `SendResult`/`ReceiveResult` more broadly), rather than
  silently returning a truncated/garbage `Message` or throwing.
- The caller — `ConsumerApp`'s consumer loop, per Part III (see
  "Fatal-error policy" above: the transport reports `Malformed` as a
  value, `ConsumerApp` is what logs and exits, not the transport itself) —
  treats `Malformed` as fatal: log the offending `header.payloadSize` and
  the configured exact size it should have matched, then exit non-zero.
  **No retry, no skip-and-continue** — a `payloadSize` mismatch this deep
  into an ordered-and-lossless-by-construction ring (per "Shared-memory
  ring layout" above) means either a wire-format bug, a wraparound/offset
  bug in the ring implementation, or memory corruption outside the
  checksum's reach (the checksum itself lives *in* the slot and can't be
  trusted to validate the very read that would fetch it safely) — none of
  which "skip this one and keep going" can safely paper over, unlike an
  ordinary checksum/sequence defect where the slot's bytes are at least
  fully readable.
- This is a narrower, harder failure than the `errorCount`-and-continue path
  in "Defect handling" below: that path handles frames that were fully and
  safely copied but fail validation *after the fact*; this path exists
  precisely because a mismatched `payloadSize` cannot reach that point
  safely at all.

This mirrors the `layoutVersion` mismatch policy (see "Lifecycle and
ownership" below): both are "the bytes can't be trusted enough to keep
running normally" situations, and both resolve the same way — log clearly,
exit non-zero, don't try to be clever about continuing.

##### Session ID: distinguishing restart from loss — **[v2]**

**v1 has no session disambiguation** — this entire mechanism, and the
problem it solves, only becomes relevant once v2 introduces crash recovery
and stale-segment recreation (see "Crash recovery scope" below); v1 has no
restart-recovery story for a producer restart to be confused with in the
first place.

A sequence-number discontinuity is ambiguous on its own. Consumer observes:

```text
..., 41, 42, 43, 0, 1, 2, ...
```

This could mean either of two very different things, and `sequenceNumber`
alone cannot tell them apart:

1. **The producer restarted.** It's a new process; its `sequenceNumber`
   counter starts over at 0. Nothing was actually lost — this is a new
   session, not a gap in the old one.
2. **Genuine massive loss or reorder within the same session** — a real
   defect, per "Defect handling" below.

**Fix: `sessionId`, a random `uint64_t` generated once at producer startup**
(not derived from PID+start-time — a PID can theoretically repeat across
reboots/wraparound, and a random 64-bit value has negligible collision
probability for this use case without that edge case). Producer generates it
once before entering `Ready` and copies it into **every** `Header` it builds
for that run. It lives only in `Header` (wire format), not as a separate
segment-metadata field — the segment already has its own PID-based liveness
check for crash recovery (see "Lifecycle and ownership" below), which is a
distinct concern from per-message restart detection; `sessionId` doesn't need
to duplicate that.

Placed in `Header` itself (not only in segment metadata checked once at
attach) — the consumer already inspects every `Header` it receives, so
checking one more field there is free, and it composes correctly with the
per-message sequence check either way. **This does not actually let an
already-attached consumer follow a producer restart mid-stream, at any
iteration** — that would require the consumer to detach and reattach to the
*new* segment the restarted producer creates, which "A producer restart is
not visible to an already-attached consumer" (see "Crash recovery scope"
below) explicitly says is out of scope; an already-attached consumer just
keeps reading its stale mapping and never sees the new segment at all. What
per-`Header` placement *does* provide is exactly the case in "Producer
restart" under "Testing strategy" below: a **new** consumer, attaching
*after* the restart, correctly treats the reset `sequenceNumber` as a new
session rather than a defect from its very first message — including if
that consumer happened to attach partway through what looks like "mid-run"
from an external observer's perspective, since it has no prior `sessionId`
to compare against. Per-`Header` placement is kept as **future-compatible
metadata**: it is exactly what a later mid-stream-follow feature (detach
whenever the segment's identity changes, resume against the new one) would
need to detect the change — building that resume logic is not this
document's job at any planned iteration, but choosing not to bury
`sessionId` in attach-only metadata means it's already in the right place
for that feature to consume without a wire-format change.

```cpp
if (header.sessionId != lastSessionId) {
  // New producer session -- reset loss-tracking, NOT a defect.
  lastSessionId = header.sessionId;
  lastSeen = header.sequenceNumber;
} else {
  // Same session -- normal gap/reorder defect check applies (see below).
  if (header.sequenceNumber != lastSeen + 1) {
    errorCount.fetch_add(1);
    // log: unexpected sequence number within session sessionId
  }
  lastSeen = header.sequenceNumber;
}
```

The first message of a new session is never itself flagged as a defect,
regardless of what `sequenceNumber` it carries — a session change is
expected to reset the counter, that's the entire point.

##### Checksum: algorithm and scope — **[v2]**

**v1 has no checksum.** The v1 `Header` (see "Wire format" above) doesn't
even carry a `checksum` field — v1's data-integrity story is the
pre-existing `payloadSize` bound-check inside `receive()` (which is not
optional, see "`send()`/`receive()` contract" above) plus the
sequence-number check in the consumer loop; it has no way to detect a bit
flip that leaves `payloadSize` and `sequenceNumber` intact but corrupts
payload bytes. That gap is accepted for v1 and closed in v2 by this
mechanism:

- **CRC32, not FNV-1a.** FNV-1a is a general-purpose hash tuned for
  hash-table distribution, not for detecting accidental bit-level
  corruption. CRC32 is specifically designed as an error-detecting code —
  it has guaranteed detection properties for common corruption patterns
  (burst errors, single/double bit flips), which is exactly the failure
  mode a checksum on an IPC channel exists to catch. Use CRC32
  unconditionally; no pluggable checksum strategy planned at any iteration.
- **Checksum covers header fields *and* payload, not payload alone.**
  `checksum` itself is excluded from its own input, but `sessionId`,
  `timestamp`, `sequenceNumber`, and `payloadSize` are included:
  ```text
  checksum = crc32(bytes_of(sessionId, timestamp, sequenceNumber, payloadSize) ++ payload)
  ```
  Rationale: a payload-only checksum leaves `sessionId`, `sequenceNumber`,
  and `payloadSize` unprotected. A corrupted `sequenceNumber` would silently
  feed wrong data into the consumer's own loss/reorder detection (the
  mechanism meant to catch defects would itself be blind to corruption of
  its input), a corrupted `sessionId` could cause a real defect to be
  misread as a session change (or vice versa) — exactly the ambiguity this
  field exists to resolve — and a corrupted `payloadSize` risks the consumer
  reading the wrong number of bytes from the slot before validation even
  runs (which is exactly why the `payloadSize` bound-check in `receive()`
  cannot itself depend on the checksum — see "`send()`/`receive()`
  contract" above — it has to run first, independent of CRC).

##### Defect handling (consumer side)

**v1 policy — fail-fast, no error counter:** v1's consumer loop checks
`header.sequenceNumber == lastSeen + 1` on every received message (there is
no `sessionId` to check first, since v1 has none — see "Session ID" above).
Any gap or out-of-order value is treated as an immediate hard error: log
the expected vs. actual sequence number and exit non-zero via
`ConsumerApp`. **No `errorCount`, no "keep consuming"** — v1's whole point
is to prove the ring is ordered and lossless by construction (see
"Shared-memory ring layout" above); a v1 test run either completes with
zero gaps or the test fails immediately at the first one. The fuller,
softer policy below is v2 scope.

**From v2 onward:** since the ring is ordered and lossless by construction
(see above), the consumer treats any checksum failure, sequence gap, or
reorder as a **defect**, not routine data — **provided `sessionId` matches
the previous message's**. A sequence discontinuity that coincides with a
`sessionId` change is a producer restart, not a defect — see "Session ID:
distinguishing restart from loss" above; the check there runs before this
one, so this section only applies within a single session:

- Increment a dedicated atomic error counter — **separate from** the normal
  stats counters (total/interval packet and byte counts; see each plan's
  `StatsReporter` step). Conflating them would corrupt the throughput
  numbers with error events.
- Log the specific defect (expected vs. actual sequence number, checksum
  mismatch, etc.), including the `sessionId` it occurred in.
- Keep consuming — useful for interactive/manual runs and for observing
  whether the defect is transient or persistent.
- Automated/integration tests assert the error counter stays at zero for the
  duration of a normal run; a nonzero value fails the test. This is what
  makes "ordered and lossless by construction" an enforced invariant rather
  than a comment.

#### Control loop and signal safety — **[v2]**

**v1 has none of this.** v1's producer sends N messages and exits; v1's
consumer receives until it observes v1's own end condition (see "v1
Definition of done" in Part II/III below) and exits. No interactive
`p`/`r`/`q`, no `SIGINT`/`SIGTERM` handling, no `Controller`, no stdin
thread. This whole section describes v2 machinery layered in afterward.

`Controller`'s `Running/Paused/Stopped` state (in-process, `std::mutex` +
`std::condition_variable`) is driven by two independent inputs, both of
which have real hazards if implemented naively:

- **stdin thread** (`p`/`r`/`q`): a thread blocked in a blocking read call on
  stdin cannot be woken by a condition variable — on quit, this thread would
  still be sitting in `read()`, unable to observe the shutdown or be
  `join()`-ed cleanly. Fix: on the quit path, close/shutdown the stdin file
  descriptor (or use a cancellable read primitive) so the blocking read
  actually returns, rather than leaving the thread stuck until process exit.
- **Signals** (`SIGINT`/`SIGTERM`): a signal handler must not call
  `controller.pause()/resume()/stop()` directly. Those use `std::mutex`,
  which is not async-signal-safe — acquiring a mutex (or doing most
  non-trivial work) inside a signal handler risks deadlock if the signal
  interrupts the same thread while it already holds that mutex. Two
  accepted-safe patterns, either is fine:
  - **Self-pipe**: signal handler does the one async-signal-safe thing
    (`write()` a byte to a pipe), and a dedicated thread blocks in `read()`
    on that pipe, translating the wakeup into a real `controller.stop()`
    call outside signal-handler context.
  - **Signal-waiting thread**: block `SIGINT`/`SIGTERM` on all threads at
    startup, then have one dedicated thread call `sigwait()`, which safely
    blocks for a signal without registering a handler at all, and call
    `controller.stop()` from ordinary thread context after it returns.

  Either way, no controller method is ever invoked from inside actual signal
  handler context — only from a normal thread that was woken by one.

This control-loop machinery (`Controller`, stdin thread, signal-safety
pattern) is shared code, used unchanged by both producer and consumer — see
directory layout below.

#### Lifecycle and ownership

Producer and consumer are separate processes with no shared parent to
coordinate startup/shutdown — this has to be an explicit protocol, not
implicit, at every iteration. Producer is authoritative: it owns creation,
layout, and teardown. **v1 needs a much smaller slice of this section than
v2/v3** — see "v1 simplifications" at the end of this section for exactly
what v1 skips.

##### Ownership by resource

Every named/shared resource has exactly one owner for creation,
initialization, and cleanup — never split across both processes:

| Resource | Created by | Initialized by | Cleaned up by |
|---|---|---|---|
| Shared-memory segment `/ipc_ring_v1` | Producer (`shm_open(O_CREAT\|O_EXCL)`) | Producer (sizes it via `ftruncate` to fit `Config.ringCapacityBytes`) | Producer (`shm_unlink`, only after reaching `Closed`) |
| `pthread_mutex_t`/`pthread_cond_t` pair (inside the segment) | Producer (lives in the segment producer created) | Producer (`pthread_mutex_init`/`pthread_cond_init` with `PTHREAD_PROCESS_SHARED`, at construction) | **Not explicitly destroyed** — no `pthread_mutex_destroy`/`pthread_cond_destroy` call; reclaimed implicitly when the backing page is unlinked and unmapped by every process (see "Teardown ordering" below). Consumer never touches these beyond lock/wait/signal calls |
| `state`/`layoutVersion`/producer-PID fields — **[v2]**, v1 has none of these | Producer (fixed offsets at segment start) | Producer (writes `Initializing` → `Ready`, `layoutVersion`, own PID, in that order) | Producer (transitions to `Closed` before unlink) |
| Write cursor | Producer | Producer (zeroed at construction) | Producer (implicitly, via segment unlink) |
| Read cursor | Producer (lives in the control block producer created) | Producer (zeroed at construction, alongside the write cursor) | Producer (implicitly, via segment unlink); consumer only ever advances it after attach, never (re)initializes it |
| Segment *mapping* (`mmap`/`munmap`) in each process | Each process maps its own view | N/A (mapping isn't "initialized," just mapped) | Each process unmaps its own view (`munmap`) on exit — independent of who unlinks the underlying segment |
| In-process `Controller` (`Running`/`Paused`/`Stopped`) — **[v2]**, v1 has none | Each process, independently | Each process, independently | Each process, independently (pure in-process object, no cross-process resource, no shared ownership question) |

The pattern: **producer initializes the entire control block**, including
*both* cursors, before the segment is usable — not just its own write
cursor. This matters because the producer may start filling the ring before
any consumer ever attaches; if the read cursor were left for the consumer
to initialize on attach, an early-attaching-later consumer would find an
uninitialized read cursor sitting behind however many slots the producer
already wrote, with no correct value to reset it to without losing those
slots. Zeroing both cursors at construction time — before either process's
data-path code ever touches them — avoids that ordering hazard entirely.

Ownership *after* that initial setup still splits: **producer owns writing
the write cursor for the rest of the run** (advances it on every `send()`);
**consumer owns advancing the read cursor** for the rest of the run
(advances it internally on every `receive()` — see "`send()`/`receive()`
contract" above) — it never reinitializes or resets the value the producer
already zeroed. Neither process re-touches the other's cursor. This is also
why consumer never calls `shm_unlink` — cleanup for producer-owned resources
happens only in the process that created them, so there's never a "who
cleans this up" ambiguity. Note the mutex/condvar pair specifically is
**not** explicitly destroyed by anyone, producer included — see "Teardown
ordering" below for why.

- **Naming** — fixed name at every iteration, not user-configurable:
  shared-memory segment `/ipc_ring_v1` (`shm_open`). No separate named
  semaphore: the `pthread_mutex_t`/`pthread_cond_t` pair lives *inside* this
  same segment (see "Two distinct sync mechanisms" above), so there is
  exactly one name to manage. The *name* is fixed; the segment's *size* is
  not — it's computed from `Config.ringCapacityBytes` at creation time (see
  "Shared-memory ring layout" above): a fixed constant in v1, a
  `--ring-capacity`/`--payload-size`-driven value from v2 onward.
- **Permissions** — `shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600)`: owner
  read/write only. Producer and consumer are expected to run as the same
  user; no cross-user sharing in scope at any iteration.
- **Either process may start first — [v2].** v1 assumes the producer starts
  first (see "v1 simplifications" below); this bullet describes the v2
  upgrade. This is expected, not a corner case, once v2 adds it (a
  monitoring consumer might be left running across multiple producer runs,
  or a user might launch consumer before producer out of habit). It's
  handled asymmetrically by *role*, not symmetrically by *race*: producer is
  the only process that ever creates the segment (`O_CREAT | O_EXCL`);
  consumer only ever opens existing (`O_RDWR`, no `O_CREAT`), regardless of
  which one happened to start first. Consumer starting first does not mean
  consumer creates it — it means consumer waits longer. This is deliberate:
  the segment's content (`payloadSize`/`ringCapacityBytes`, from producer's
  CLI flags) is producer-defined config; a symmetric "whoever gets there
  first creates it" model would require the consumer to also carry defaults
  for config it has no CLI flags for, and would make ownership conditional
  on race outcome instead of on role. One creator, one role, regardless of
  start order.
- **Attach retries — [v2].** v1's consumer assumes the segment already
  exists (see "v1 simplifications" below) and does a single blocking
  attach, not a retry loop. From v2 onward: if the segment doesn't exist
  yet (producer isn't up), `shm_open` fails with `ENOENT` — consumer
  retries on a bounded backoff (e.g. every 100 ms). Unlike a producer
  crash-recovery check (bounded, see "Stale segments" below), a consumer's
  wait for a producer that simply hasn't started yet is not itself an error
  condition — a 5 s give-up would misfire on the legitimate "consumer
  launched well before producer" case. Retries indefinitely on
  `ENOENT`/`state != Ready` (logging periodically so it's visibly waiting,
  not hung); an operator can `Ctrl-C` it if that's not what they wanted. A
  bounded give-up is worth adding later if this proves annoying in
  practice, but isn't required for correctness.
- **Initialization readiness — [v2].** v1 has no `state` field to gate on
  (see "v1 simplifications" below) — v1's consumer just attaches once the
  segment exists and the control block is the expected size. From v2
  onward: segment *existing* isn't sufficient — the producer must finish
  constructing the process-shared mutex/condvar pair and cursors before a
  consumer may touch them. A `state` field (see state machine below),
  placed at a fixed offset at the very start of the segment, gates this:
  producer writes its own PID **first** (see "Stale segments after a
  crash" below for why this ordering matters), then sets `Initializing`
  right after `mmap`, does all pthread-attr/cursor setup, then atomically
  publishes `Ready` last. Consumer's retry loop treats `ENOENT` and
  `state != Ready` identically — keep backing off and retrying.
- **Layout compatibility — [v2].** A `uint32_t layoutVersion` field
  immediately after `state`, bumped whenever `Header`/slot layout changes
  (including the v1→v2 `Header` shape change itself — see "Wire format"
  above). Consumer checks this right after observing `state == Ready`; a
  mismatch means consumer logs and exits non-zero instead of interpreting
  bytes through the wrong struct layout. v1 has no such field — v1's
  producer and consumer are built from the same source at the same time,
  so a layout mismatch between them isn't a scenario v1 needs to guard
  against yet.
- **`shm_unlink` ownership** — producer only, on clean shutdown, at every
  iteration (v1: simply on process exit; v2 onward: after reaching `Closed`,
  see state machine below). Consumer never unlinks; it only detaches its
  own mapping (`munmap`/`close`).
- **Stale segments after a crash — [v2].** v1 has no crash-recovery story
  (see "v1 simplifications" below) — a leftover segment from a killed v1
  producer would need to be removed manually (e.g. `rm
  /dev/shm/ipc_ring_v1` on Linux) before the next run. From v2 onward:
  `shm_unlink` isn't automatic on crash, so a segment can outlive the
  process that created it. Producer startup handles this explicitly: before
  attempting `O_CREAT | O_EXCL`, try a plain `shm_open` (no `O_EXCL`). If
  that succeeds, inspect `state` and the PID the previous producer stored
  (see below):
  - `state == Closed` → previous producer shut down cleanly but didn't
    unlink for some reason; safe to unlink and recreate.
  - `state` looks live (`Initializing`/`Ready`/`Stopping`) → check whether
    the stored PID is still alive (`kill(pid, 0)`); if not, it's a crash
    artifact — unlink and recreate. If the PID *is* alive, another producer
    is genuinely running — refuse to start with a clear error (this
    project is 1:1 at every iteration, two producers on the same segment is
    a misuse, not a case to silently handle).
  - **PID must be valid *before or at* the transition into `Initializing`,
    not deferred to `Ready`.** An earlier version of this document had the
    producer write its PID only "at the point it publishes `Ready`," which
    leaves a real gap: if a producer crashes *during* `Initializing` (after
    setting `state`, before ever reaching `Ready`), the PID field is still
    whatever `ftruncate`'s zero-fill left it — not the crashed producer's
    actual PID. A second producer's stale-segment check would then see
    `state == Initializing` (looks live) but read a bogus/zero PID, making
    `kill(pid, 0)` meaningless: `kill(0, 0)` has its own special
    (signal-to-process-group) semantics, not "is PID 0 alive," and a
    garbage nonzero PID could coincidentally belong to an unrelated live
    process, causing a false "another producer is genuinely running"
    refusal. **Fix:** the producer writes its own PID into the segment
    (fixed field, right after `layoutVersion`) as the very first thing it
    does after `mmap` — *before* setting `state` to `Initializing` — so the
    field is always valid for any `state` value a second producer might
    observe, including `Initializing` itself. This PID is what the
    crash-recovery check above reads, for any live-looking `state`, not
    only for `Ready`.

##### Lifecycle state machine — **[v2]**

**v1 has no lifecycle state machine.** v1's producer creates the segment,
runs, and unlinks it on exit; v1's consumer attaches once and reads until
its own end condition (see "v1 Definition of done" below). There is no
`Initializing`/`Ready`/`Stopping`/`Closed` progression to observe. This
entire subsection is v2 scope.

**`state` is `std::atomic<uint32_t>`** (an explicit fixed-width underlying
type, per "`Header` must be trivially copyable... use only fixed-width
integer types" above — no plain `enum class` with an implementation-defined
underlying type), stored at a fixed offset at the start of the segment,
alongside `layoutVersion` and the producer's PID:

```text
Initializing -> Ready -> Stopping -> Closed
```

The lock-free requirement in "Cross-process atomics: an explicit platform
requirement" above (`is_always_lock_free`, verified via the same
release/acquire smoke test) **applies to `state` exactly as it does to the
v3 write/read cursors — this is not a separate, unstated assumption.**
`state` is cross-process shared atomic data read and written outside any
mutex, same as the v3 cursors: the consumer's attach-retry loop polls it
directly (see "Initialization readiness" above) and the bounded-wait
predicate check reads it on every wake (see "Clean producer shutdown and
the receive predicate" below) — neither takes the mutex to do so. The same
publication/consumption discipline from "Cursor synchronization model"
applies too: the producer completes whatever `state` implies (finishing
pthread/cursor setup before `Ready`; finishing its own drain before
`Closed`) *before* the release-store that publishes the new `state` value,
and any process reading `state` uses an acquire-load before trusting that
what the new state implies has actually happened — e.g. the consumer's
acquire-load observing `Ready` is what makes it safe to then read
`layoutVersion` and the cursors, not the mere fact of observing *some*
value in the `state` field. `std::uint32_t` specifically (not `uint64_t`
like the cursors) is enough range for four states and keeps the field
smaller — lock-free guarantees for `std::atomic` don't depend on matching
width across every atomic in the segment, only on each individual
instantiation satisfying `is_always_lock_free` on its own, which
`uint32_t` does exactly as reliably as `uint64_t` on every mainstream
platform this project targets. Since `state` is atomic and cross-process
from the moment it's introduced in v2 (unlike the cursors, which stay
mutex-protected plain integers until v3), it needs this same lock-free
verification from v2 onward, ahead of the cursors' own v3 upgrade.

- **`Initializing`** — producer has created/opened the segment and is
  constructing sync primitives and cursors. Consumer must not attach beyond
  polling this field.
- **`Ready`** — fully initialized; safe for the consumer to attach, check
  `layoutVersion`, and start `receive()`.
- **`Stopping`** — producer received quit (`SIGINT`/`SIGTERM`/`q`) and is
  winding down. Consumer keeps draining whatever remains in the ring; new
  consumer attach attempts should back off rather than proceed.
- **`Closed`** — producer has finished and is about to `shm_unlink`.
  Consumer sees this mid-loop, drains any remaining messages, and exits.

##### Clean producer shutdown and the receive predicate — **[v2]**

**v1 has no `state` and no `EndOfStream`** — v1's consumer detects the end
of a run via its own simpler condition (see "v1 Definition of done" below,
e.g. "received the expected message count" or a producer-signaled
completion specific to v1's minimal protocol), not via observing a
lifecycle transition. This subsection is entirely v2 scope, describing how
the mechanism above interacts with the bounded-wait predicate once both
exist.

An earlier version of this document specified the blocked-`receive()`
wake checks (see "Waking a blocked `send()`/`receive()`" above) as only
predicate-met / local-`Stopped` / producer-PID-liveness — it never said
what happens when the ring goes empty *because the producer is finishing
normally*, as opposed to being dead. That's a real, common case (every
clean shutdown passes through it: producer drains its own send path,
reaches `Stopping`, then `Closed`, well before the consumer necessarily
finishes draining) and deserves its own outcome, not a fall-through to
"wait out the bound, time out, re-check, eventually notice via some other
path."

**Fix, in two parts:**

1. **Producer signals "message available" on every `state` transition from
   `Ready` onward — while holding the condition-variable mutex** — into
   `Stopping` and again into `Closed` — even though no new message was
   written. This is a deliberate, explicit exception to two things at once:
   (a) "producer signals `message available` only after advancing the
   write cursor" (see "Cursor synchronization model" above) — the state
   transition itself is news the consumer needs, so it gets the same
   wakeup treatment as a new message; and (b) v3's "the fast path never
   takes the mutex" — **lifecycle transitions are not the fast path.** A
   producer makes at most two of them per run (`Ready`→`Stopping`,
   `Stopping`→`Closed`), so taking the mutex here costs nothing measurable
   and does not reintroduce the mutex on the per-message `send()`/
   `receive()` path at all, even once v3's lock-free fast path exists.

   This distinction matters once v3 exists, because of "Missed
   notifications become possible and accepted" above: that acceptance is
   scoped to v3's per-message fast path, where the notifier updates a
   cursor and signals *without* the mutex, leaving the TOCTOU window
   described there. **A lifecycle-transition signal sent while holding the
   mutex does not have that window** — the classic mutex-guarded-predicate
   guarantee (no lost wakeup between a waiter's predicate check and its
   `pthread_cond_wait` call, when both waiter and notifier serialize
   through the same mutex) applies here specifically, because the notifier
   takes the mutex too, unlike the ordinary cursor-advance signal. This is
   what makes the producer's prompt-wake claim in its Definition of Done
   actually true, rather than merely likely: a consumer already blocked in
   `pthread_cond_timedwait` at the moment of the transition is guaranteed
   to be woken by this signal specifically, not just eventually by the next
   timeout.
2. **The bounded-wait predicate check (step 2 in "Waking a blocked
   `send()`/`receive()`" above) explicitly includes `state`:** on every
   wake, `receive()` checks not just "is the ring non-empty" but "is the
   ring non-empty, **or** is `state` `Stopping`/`Closed`." If the ring is
   empty and `state` is `Stopping` or `Closed`, `receive()` returns
   `ReceiveResult::EndOfStream` (see the enum definition above) instead of
   continuing to wait. If the ring is *not* empty, `receive()` still drains
   normally regardless of `state` — `Stopping`/`Closed` means "no more new
   messages are coming," not "discard what's already buffered." This is
   exactly the "consumer drains whatever remains, then exits" behavior
   already specified for `Stopping`/`Closed` above, now given a precise
   signal (`EndOfStream`) instead of leaving the consumer to infer "drained
   and producer is gone" from the absence of further messages.

`EndOfStream` is expected, not an error: the consumer's normal shutdown
path (per Part III) treats it the same way it already treats observing
`Stopping`/`Closed` directly — stop calling `receive()` and exit cleanly,
with no `errorCount` increment (see "Defect handling" above) and no
non-zero exit code attributable to this alone.

##### Teardown ordering

Naively, "producer owns the mutex/condvar pair, so producer destroys them"
(per the ownership table above) suggests calling `pthread_mutex_destroy`/
`pthread_cond_destroy` right before `shm_unlink` on process exit (v1) or on
the `Stopping` → `Closed` transition (v2 onward). **That is unsafe, and not
only in a crash — this applies at every iteration, including v1.** At v2
onward, the `Closed` state's own definition says the consumer "drains any
remaining messages" after observing it — meaning the consumer is *expected*
to still be calling `receive()` (locking the mutex, waiting on the condvar)
for some window after the producer reaches `Closed`. Even in v1, which has
no `Closed` state, the same underlying race exists the moment the producer
finishes sending and starts tearing down while the consumer might still be
mid-`receive()` on the last few messages. If the producer destroys the
primitives at that point, it races an in-spec, non-crashed consumer that
just hasn't finished draining yet — undefined behavior on ordinary,
successful shutdown, not an edge case requiring a prior crash.

**Policy at every iteration: never explicitly destroy the mutex/condvar
pair.** No `pthread_mutex_destroy`/`pthread_cond_destroy` call, by either
process, ever:

1. Producer finishes sending (v1) or reaches `Closed`, publishes it (v2
   onward), and calls `shm_unlink` — this removes the *name* from the
   filesystem namespace but does not invalidate mappings already open in
   either process (standard `shm_open`/`unlink` semantics, same as a
   regular file: existing `mmap`s stay valid until unmapped).
2. Producer `munmap`s its view, then exits.
3. Consumer drains whatever is left in the ring (v1: until its own end
   condition; v2 onward: per the `Closed`-state contract), then `munmap`s
   its own view and exits — independently, on its own schedule, with no
   signal required from the producer beyond having already observed the
   end condition.
4. Once *both* processes have unmapped, the kernel reclaims the underlying
   pages — mutex, condvar, and all — as an ordinary consequence of the last
   mapping going away. Skipping the explicit destroy calls costs nothing:
   `pthread_mutex_destroy`/`pthread_cond_destroy` on process-shared
   primitives backed by shared memory don't release any resource beyond
   invalidating the object in place, which reclaiming the page already does
   for free.

This avoids the choice between (a) a consumer-detached acknowledgement
handshake — the producer would have to wait, post-shutdown, for some signal
that the consumer has actually unmapped, which is new protocol surface not
worth adding at any planned iteration — and (b) the race described above.
No handshake, no race: the pair is simply never destroyed out from under a
peer that might still be using it, because nothing ever destroys it at all.

##### Crash recovery scope — **[v2 partially closes this; some gaps remain beyond v3]**

**v1 has essentially none of this.** v1 assumes both processes stay alive
for the run's duration and shut down cleanly; a killed v1 process leaves a
stale segment that must be cleaned up manually before the next run (see
"Stale segments after a crash" above). Everything below describes what v2's
mechanisms (PID-liveness check, bounded wait, `sessionId`, teardown
ordering) do and do not cover, once they exist.

The v2 mechanisms cover clean shutdown and stale-segment cleanup at next
startup. They do **not** add up to full crash recovery, and pretending
otherwise would overstate what v2 actually does. Three gaps remain, each
real, each out of scope through v3 as currently planned (a fourth —
teardown racing a still-attached peer — was a clean-shutdown bug, not a
crash-only gap, and is fixed above in "Teardown ordering," not merely
scoped out, and applies from v1 onward):

- **Only the producer's PID is stored.** There is no consumer-liveness
  record in the segment, so the producer has no way to detect a gone
  consumer — the "Waking a blocked `send()`/`receive()`" mechanism above
  only lets the *consumer* detect a dead *producer*, not the reverse. This
  is not crash-specific: a consumer that exits **cleanly** (`q`/`SIGINT`,
  reaching its own `Stopped` state and returning) is exactly as invisible to
  the producer as one that crashes — neither writes anything to the segment
  on the way out, since the consumer owns no lifecycle field the producer
  reads. A producer blocked on a full ring, whether the consumer crashed or
  quit normally, wakes only on its own local `Controller` state, never on
  consumer absence — see `SendResult` in "Waking a blocked `send()`/`receive()`"
  above, which has no `PeerClosed` case for exactly this reason.
- **The process-shared mutex is not robust — and this makes
  `ReceiveResult::PeerClosed` best-effort, not guaranteed.** If a process
  dies while holding `pthread_mutex_t` (e.g. between lock and unlock inside
  `send()`/`receive()`), the mutex stays locked forever. `pthread_cond_timedwait`
  re-acquires that same mutex internally before it can return control to the
  caller — including before the consumer's bounded-wait loop can reach its
  PID-liveness check (see "Waking a blocked `send()`/`receive()`" above). So
  a producer crash that orphans the mutex leaves the consumer blocked
  re-acquiring it, never reaching the check that would have returned
  `PeerClosed` — the bounded-wait latency guarantee simply doesn't apply to
  this crash shape. `PTHREAD_MUTEX_ROBUST` (`pthread_mutex_consistent` +
  `EOWNERDEAD` handling) would fix this, but is not implemented at any
  currently-planned iteration.
- **A producer restart is not visible to an already-attached consumer.**
  Stale-segment recovery (above) unlinks and recreates the segment, but a
  consumer that already has the *old* segment `mmap`'d has no trigger to
  detach and reattach — it keeps reading its stale mapping and never
  observes the new `sessionId`. The `sessionId` check ("Session ID:
  distinguishing restart from loss" above) only works for a consumer that
  attaches *after* the restart; it does nothing for one already attached
  across it.

**Explicitly declared unsupported through v3:** a forced crash (`kill -9`,
segfault, OOM-kill) of either process while the *other* is live, and a
live producer restart against an already-attached consumer. What v2/v3
*do* support: clean shutdown (`Stopping` → `Closed` → unlink, both
processes independently drain and exit via their normal quit path per
"Teardown ordering" above — no destroy-ordering race in that path) and
stale-segment cleanup the *next* time a producer starts against a segment
nobody is attached to. Full crash recovery (robust mutex, consumer-liveness
record, detach/reattach signaling) is future work beyond v3, not a gap to
silently paper over.

#### Shared directory layout

Producer and consumer share message format, transport, and control-loop
plumbing — that code lives in `src/common/` so neither app depends on the
other's binary, only on the same headers/objects. This is the eventual,
full-featured layout (v3-complete); v1 builds a strict subset of it — see
"v1 build order" in Part II/III below for exactly which files exist at each
step.

```text
src/
├── common/
│   ├── message/
│   │   ├── message.h
│   │   ├── header.h               # build_header(payload, seq) -- v1: seq+size only; v2 adds sessionId/timestamp/checksum
│   │   └── checksum.h             # crc32() over header fields + payload -- [v2]
│   ├── transport/
│   │   ├── transport.h            # ITransport
│   │   └── shared_memory_transport.{h,cpp}  # process-shared pthread mutex+condvar; mutex-protected cursors in v1/v2, lock-free atomics in v3
│   └── control/                   # [v2] -- does not exist in v1
│       ├── controller.{h,cpp}     # Running/Paused/Stopped, std::condition_variable (in-process)
│       └── signal_handler.{h,cpp} # SIGINT/SIGTERM -> controller, self-pipe or sigwait
├── producer/
│   ├── producer_app.{h,cpp}
│   ├── command_line_parser.{h,cpp}  # v1: --count only; v2 adds --payload-size/--ring-capacity
│   ├── message_builder.{h,cpp}
│   ├── payload_generator.h        # IPayloadGenerator -- [v2]; v1 builds payloads directly, no interface
│   ├── random_payload_generator.{h,cpp}
│   └── producer_main.cpp
└── consumer/
    ├── consumer_app.{h,cpp}
    ├── command_line_parser.{h,cpp}
    ├── packet_validator.{h,cpp}    # checksum + sequence-gap detection, error counter -- [v2]; v1 checks sequence number inline, no separate class
    ├── stats_reporter.{h,cpp}      # 1s ticker: cumulative + interval counters, monotonic clock -- [v2] -- does not exist in v1
    └── consumer_main.cpp
```

`ProducerController`/`ConsumerController` both reduce to the same
`Running/Paused/Stopped` state machine — one `Controller` class in
`common/control/`, used by both apps, from v2 onward. Same for the signal
handler: it only translates a signal into a `controller.pause()/resume()/
stop()` call from safe thread context (see "Control loop and signal safety"
above), identical in both processes.

Skip `HeaderGenerator` as a separate interface at any planned iteration —
checksum + timestamp + sequence number is one small free function
(`build_header(payload, seq)`), not enough variance to justify an interface
yet. Revisit if a second header scheme shows up.

#### Testing strategy

The definitions of done in each plan cover manual verification, but manual
testing alone doesn't scale to the failure modes this document defines.
Coverage is added incrementally, matching each iteration's actual scope —
there's no value in writing a corruption test against v1's minimal
`Header`, which has no checksum to corrupt yet.

**v1 automated coverage:**

- **Cross-platform pthread smoke test** — the `fork()`-based
  `PTHREAD_PROCESS_SHARED` mutex/condvar check described in "Cross-platform
  verification" above, run in CI on the `ubuntu-26.04` runner (see this
  document's Requirements/Setup sections) to confirm the same guarantee
  holds on Linux, not just the macOS machine it was manually verified on.
  v1 depends on process-shared pthread primitives from the start, so this
  test belongs at v1, not later.
- **End-to-end test** — start a real producer process and a real consumer
  process (or drive both in-process against the same transport instance,
  whichever is simpler to wire up first), send N fixed-size sequenced
  messages, assert the consumer receives all N in order with the exact
  final count v1's Definition of Done specifies. This is the one test that
  directly proves v1's Definition of Done.
- **Wraparound** — drive the ring past `N` slots multiple times (write
  cursor and read cursor both wrap), assert no corruption at the wrap
  boundary. This exercises the ring's core mechanism (see "Shared-memory
  ring layout" above), so it belongs at v1, not deferred to a later
  iteration just because the ring itself is v1 scope.
- **Backpressure** — fill the ring with no consumer draining, assert
  producer's `send()` blocks (doesn't drop, doesn't busy-spin — e.g. assert
  on CPU usage or a bounded wait) and unblocks promptly once draining
  resumes. Same reasoning as wraparound: this is testing v1's ring, not a
  later feature.
- **Transport round-trip** — send N messages through the ring in-process
  (no separate producer/consumer binaries needed), assert exact byte-for-byte
  receipt in order. A lower-level companion to the end-to-end test above,
  useful for isolating ring bugs from process-startup/CLI-wiring bugs.

**v2/v3 automated coverage** (added once the corresponding iteration's
features exist):

- **Cross-process atomic visibility smoke test — [v3]** — the release-store
  (parent) / acquire-load (child) `std::atomic<uint64_t>`-in-shared-memory
  check described in "Cross-process atomics: an explicit platform
  requirement" above, run on both macOS and the `ubuntu-26.04` CI runner.
  Paired with the compile-time `is_always_lock_free` `static_assert` in the
  same section — the runtime test catches what the compile-time check
  can't (actual cross-process visibility), the compile-time check catches
  what the runtime test shouldn't have to (a platform that silently
  compiles a lock-based fallback). Not meaningful before v3 introduces the
  atomics it's testing.
- **Shutdown — [v2]** — assert stdin thread and signal path both actually
  cause clean process exit (join, not force-kill) under `SIGINT`/`SIGTERM`
  and the `q` keypress, from each of `Running`/`Paused` state.
  - The two prior signal hazards ("stdin thread never gets a wake" and
    "controller may not be called directly from a handler") are best caught
    by the same test that drives real `SIGINT` during a blocked `receive()`.
- **Corruption — [v2]** — deliberately flip a bit in a slot's payload/header
  after it's written, assert the consumer's error counter increments and
  the checksum failure is logged, without crashing. Requires v2's checksum
  to exist at all.
- **Producer restart — [v2]** — per "Crash recovery scope" above, restart
  against an *already-attached, live* consumer is explicitly unsupported,
  so this test does not attempt it. Instead: stop the consumer cleanly,
  kill and restart the producer, then start a *new* consumer against the
  recreated segment; assert two independent things: (1) the stale-segment
  recovery path in "Lifecycle and ownership" is exercised correctly
  (recreate vs. refuse-to-start based on PID liveness), and (2) the
  session-ID check in "Session ID: distinguishing restart from loss" fires
  correctly — the new consumer's `errorCount` does **not** increment
  despite `sequenceNumber` resetting to 0, because `sessionId` changed.
- **Version-mismatch — [v2]** — start a consumer against a segment whose
  `layoutVersion` doesn't match, assert it exits non-zero with a clear
  message instead of misinterpreting bytes.

### Part II: Producer Implementation Plan

Scope: `producer-cli` only. Part I above is the shared contract this plan
must honor — only what's specific to the producer is covered here. See
Part III for the consumer side.

#### Efficiency definition

Priority order for this project, at every iteration:

1. Lowest latency — no busy-wait; one bounded `memcpy` per `send()`/`receive()`
   is accepted (see "`send()`/`receive()` contract" in Part I), not a
   zero-copy borrowed view — that optimization is deferred indefinitely,
   not premature to want later.
2. Minimal overhead — small, fixed-size header; no allocation per message
   (the per-call copy reuses a caller-owned buffer, never allocates).
3. High throughput — sustained sends, no per-message syscall if avoidable.
4. Low CPU/memory — condition-variable wait (v1: plain `pthread_cond_wait`;
   v2 onward: bounded `pthread_cond_timedwait`), not spin. See "Two distinct
   sync mechanisms" in Part I.

Shared memory transport serves all four; that's why it's the transport
built first, at v1.

#### v1 build order

The goal for v1 is a small, reviewable vertical slice — each step below is
sized to produce roughly 100–200 lines of code, independently buildable and
testable before moving to the next:

1. **Config + CLI parsing** (~50–100 lines) — a `Config` struct with
   `payloadSize`/`ringCapacityBytes` as **compile-time constants** (see
   "Shared-memory ring layout" in Part I — no `--payload-size`/
   `--ring-capacity` flags in v1), plus a minimal command-line parser for a
   single `--count <N>` flag (how many messages to send, required or
   defaulted to something small like 1000 for manual testing).
2. **`Header`/`Message` wire format** (~50–100 lines) — the v1 `Header`
   (`sequenceNumber` + `payloadSize` only, see "Wire format" in Part I) with
   its `static_assert`s, and the `Message` struct pairing a `Header` with a
   `std::span<std::byte>` into a caller-owned buffer. No checksum, no
   `sessionId`, no `timestamp` — see "v1 simplifications" below for why
   each is deferred.
3. **`ITransport` interface + `SharedMemoryTransport` construction/attach**
   (~150–200 lines) — `shm_open`/`mmap`/`ftruncate`, the process-shared
   `pthread_mutex_t` + two `pthread_cond_t` initialized with
   `PTHREAD_PROCESS_SHARED`, and the two plain-integer cursors, all zeroed
   at construction (see "Ownership by resource" in Part I). v1's attach
   logic is deliberately minimal: producer creates with `O_CREAT | O_EXCL`
   (no stale-segment recovery — see "v1 simplifications" below); consumer
   does one attach attempt assuming the producer already created the
   segment (see "v1 Definition of done" below — "start the producer, then
   start the consumer" is a stated v1 precondition, not a race to handle).
4. **Ring `send()`/`receive()` with mutex-protected cursors** (~100–150
   lines) — the v1 baseline from "Cursor synchronization model" in Part I:
   every call takes the single mutex, checks the full/empty predicate
   against the plain-integer cursors, copies into/out of the slot, advances
   its own cursor, and signals the appropriate condvar — blocking via plain
   `pthread_cond_wait` (no timeout, no `Stopped`/`PeerClosed` checks — those
   are v2). Returns `bool`, not a result enum (see "Fatal-error policy" in
   Part I).
5. **Producer main loop + `producer_main.cpp` wiring** (~50–100 lines) —
   build `Config.payloadSize`-sized payloads (e.g. a simple counter or fixed
   pattern — no `IPayloadGenerator` interface yet, see "v1 simplifications"
   below), stamp each with the next `sequenceNumber`, `send()` it, repeat
   `--count` times, then exit. On any `send()` failure, log and exit
   non-zero (see "v1 error handling" below).

#### v1 error handling

`send()`/`receive()` return plain `bool` in v1 — `true` on success, `false`
on any fatal condition (a transport construction/attach failure, or —on the
consumer side — a `payloadSize` mismatch or sequence-number gap detected
inline). `ProducerApp`/`ConsumerApp` check the return value, print a clear
message describing what failed, and exit non-zero. No result enums, no
exceptions crossing the `ITransport` boundary, no retry — see "Fatal-error
policy" in Part I for why this minimal version is deliberate and what v2
replaces it with.

#### v1 simplifications (what v1 explicitly skips, and why)

Each item below is fully specified elsewhere in Part I as v2 or v3 scope —
nothing here is a permanent decision, just a statement of what v1 does not
yet build:

- **No interactive `p`/`r`/`q` controls, no `Controller`, no stdin thread,
  no signal handling** — v1's producer runs to completion and exits; there
  is nothing to pause or resume. See "Control loop and signal safety" in
  Part I — **[v2]**.
- **No `StatsReporter`, no periodic output, no throughput/latency
  measurement** — v1 has no dedicated stats thread; a final message count
  printed once at the end (see "v1 Definition of done" below) is enough to
  prove correctness. See Part III's `StatsReporter` build step — **[v2]**.
- **No CRC32, no `PacketValidator`** — v1's only per-message check is the
  sequence-number continuity check, done inline in the consumer loop. See
  "Checksum: algorithm and scope" and "Defect handling" in Part I —
  **[v2]**.
- **No `sessionId`, no producer-restart interpretation** — v1 has no
  crash-recovery story for a restart to be confused with data loss in the
  first place. See "Session ID: distinguishing restart from loss" in
  Part I — **[v2]**.
- **No `timestamp` field** — v1's `Header` has no field for it; it's
  introduced alongside the v2 `Header` shape. See "Wire format" in Part I.
- **No producer PID liveness checks, no `PeerClosed` detection** — v1's
  consumer has no bounded wait and no PID to check; a dead producer simply
  leaves the consumer blocked (see "Waking a blocked `send()`/`receive()`"
  in Part I — **[v2]**).
- **No crash recovery, no stale-segment recreation** — a killed v1 producer
  leaves a segment that must be removed manually before the next run. See
  "Stale segments after a crash" in Part I — **[v2]**.
- **No consumer-starts-first retry/backoff** — v1's consumer assumes the
  producer already created the segment; "start the producer, then start the
  consumer" is a stated v1 precondition. See "Attach retries" in Part I —
  **[v2]**.
- **No runtime `--payload-size`/`--ring-capacity` flags** — both are
  compile-time constants in v1. See "Fixed slot capacity" in Part I —
  **[v2]**.
- **No detailed malformed-frame recovery, no `Malformed` result** — v1's
  `payloadSize` bound-check (which does run — it's not optional, see
  "`send()`/`receive()` contract" in Part I) fails the whole call as a
  `bool` `false`, not a distinct enum case. See "Malformed-frame policy" in
  Part I — **[v2]**.
- **No rich result enums, no structured transport errors** — `bool`
  everywhere. See "Fatal-error policy" in Part I — **[v2]**.
- **No `IPayloadGenerator` interface** — v1 builds payloads directly in the
  producer loop; introducing an interface for one implementation is
  premature until a second payload strategy actually exists. See Part II's
  "Explicitly deferred" below.
- **No cross-process atomics, no lock-free fast path** — v1's cursors are
  plain integers behind the mutex, locked on every call. See "Cursor
  synchronization model" and "v3: lock-free fast path" in Part I — **[v3]**.
- **No exhaustive signal/crash/restart/corruption/version-mismatch tests**
  — v1's test list (see "Testing strategy" in Part I) covers only what v1
  actually builds: the pthread smoke test, an end-to-end test, wraparound,
  backpressure, and transport round-trip. Everything else in "Testing
  strategy" is tagged **[v2]**/**[v3]** and added when the feature it tests
  exists.

#### v2 build order

Layered on top of v1's mutex-based ring, without changing `ITransport`'s
public shape or the ring's core concept — v2 adds features around/inside
the same interface, not a rewrite of it:

1. **`sessionId` + fuller `Header`** — expand v1's `Header` to the v2 shape
   (`sessionId`, `timestamp`, `sequenceNumber`, `payloadSize`, `checksum`),
   bump `layoutVersion`, add the CRC32 checksum function (see "Checksum:
   algorithm and scope" and "Wire format" in Part I).
2. **`IPayloadGenerator` interface** — extract v1's inline payload
   construction into an `IPayloadGenerator` interface with
   `RandomPayloadGenerator` as its first implementation (see Part II's
   "Explicitly deferred" below for why this waited until v2).
3. **Lifecycle state machine + ownership metadata** — add `state`,
   `layoutVersion`, producer-PID fields to the control block; implement the
   `Initializing`→`Ready`→`Stopping`→`Closed` transitions; add stale-segment
   detection/recreation on producer startup (see "Lifecycle and ownership"
   in Part I).
4. **Bounded wait + rich result enums** — replace v1's blind
   `pthread_cond_wait` with bounded `pthread_cond_timedwait`; add
   `SendResult`/`ReceiveResult`; add the producer-PID liveness check and
   `PeerClosed`; add the `Malformed` result and the mutex-held
   lifecycle-transition signal for `EndOfStream` (see "Waking a blocked
   `send()`/`receive()`", "Malformed-frame policy", and "Clean producer
   shutdown and the receive predicate" in Part I).
5. **`common::Controller` + control input** — `Running/Paused/Stopped`
   state, stdin thread (`p`/`r`/`q`), signal handling via self-pipe or
   `sigwait()` (see "Control loop and signal safety" in Part I).
6. **`PacketValidator` + `StatsReporter`** — checksum recomputation,
   session-ID-aware sequence checking, `errorCount`; cumulative/interval
   counters with a monotonic-clock ticker (see "Defect handling" in Part I
   and Part III's build order below).
7. **Runtime `--payload-size`/`--ring-capacity` flags** — replace v1's
   compile-time `Config` constants with CLI-parsed values, validated at
   startup.
8. **Consumer-starts-first retry/backoff** — replace v1's single blocking
   attach with the indefinite-retry-with-logging loop described in "Attach
   retries" in Part I.

Each v2 step should still be kept reasonably small for review, though v2's
steps don't need to hit v1's 100–200 line target as precisely — v2 is
documented scope for the next phase of work, not an imminent line-by-line
plan the way v1's steps are.

#### v3 build order

Purely internal — no producer- or consumer-visible API change from v2:

1. **Cross-process atomics for cursors** — replace the plain-integer,
   mutex-protected cursors with `std::atomic<uint64_t>`, `alignas(64)`
   cache-line separation, and the `is_always_lock_free` compile-time
   requirement (see "Cross-process atomics: an explicit platform
   requirement" in Part I).
2. **Lock-free fast path** — move `send()`/`receive()`'s cursor
   reads/writes off the mutex entirely; the mutex is acquired only around
   the boundary-condition `pthread_cond_timedwait` call (see "v3: lock-free
   fast path" in Part I).
3. **Release/acquire publication rules** — implement the
   publication/consumption memory-ordering rules for both the cursors and
   the (already-atomic-since-v2) lifecycle `state` field (see "Cursor
   synchronization model" and "Lifecycle state machine" in Part I).
4. **Missed-notification acceptance** — document and verify (via the
   cross-process atomic visibility smoke test) that the fast path's
   mutex-free signal can miss a waiter, and that the existing bounded wait
   is what recovers from it (see "Missed notifications become possible and
   accepted" in Part I).
5. **Cross-process atomic visibility smoke test** — the `fork()`-based
   release-store/acquire-load test described in "Cross-process atomics"
   above, added to CI on both macOS and Linux.

#### Explicitly deferred (producer)

- `TransportType`/`GeneratorType`/`TransportFactory` — premature until a
  second transport or generator actually exists. (`Config` itself is
  **not** deferred — see v1 build step 1 and "Shared-memory ring layout" in
  Part I — only the transport/generator selection fields would be.)
- **`IPayloadGenerator` as a separate interface, in v1** — v1 builds
  payloads directly in the producer loop (see v1 build step 5). One
  implementation doesn't justify an interface yet; v2 introduces it (see
  v2 build order above) once there's an actual reason to swap strategies.
- Throughput/latency measurement — belongs to consumer side (it observes
  arrivals) and is v2 scope; see Part III.
- Any transport beyond shared memory.
- Multi-consumer fan-out (broadcast vs. per-consumer queue) — 1:1 at every
  planned iteration.
- **Zero-copy borrowed slots** — an RAII-scoped borrowed-view API (as
  opposed to the earlier rejected manual `release()` design — see
  "`send()`/`receive()` contract" in Part I) is a valid future optimization
  once the copying version through v3 is proven correct. Not attempted at
  any currently-planned iteration.
- **Richer error propagation beyond `SendResult`/`ReceiveResult`, in v2** —
  the wake-on-shutdown mechanism in "Waking a blocked `send()`/`receive()`"
  (Part I) (bounded `pthread_cond_timedwait` + `Controller` state check,
  plus a producer-PID liveness check on the `receive()` side only) is wired
  through `ITransport`'s `SendResult`/`ReceiveResult` enums — that part is
  in scope for v2, not deferred, **but the two enums are not symmetric**:
  `SendResult` has only `Sent`/`Stopped` (no `PeerClosed` — see "Crash
  recovery scope" for why), `ReceiveResult` has all five
  (`Received`/`Stopped`/`PeerClosed`/`Malformed`/`EndOfStream`). OS-level
  and protocol failures are handled too, but outside these enums entirely —
  see "Fatal-error policy" (Part I): both are fatal (log, exit non-zero),
  not routed through a result type at all. What *is* deferred beyond v3:
  adding consumer-liveness tracking so `send()` could gain its own
  `PeerClosed`, richer diagnostics (e.g. distinguishing *why* a peer is
  gone), and retry/backoff policy exposed to the caller.

#### v1 Definition of done (producer)

Matches the vertical-slice goal exactly: `./producer-cli --count <N>` (or
an equivalent fixed default) runs, writes `N` fixed-size messages —
`sequenceNumber` + `payloadSize`-byte payload, no checksum, no `sessionId`
— into the bounded shared-memory ring at a steady rate, blocking (not
dropping) whenever the ring is full until the consumer drains it. On
sending the `N`-th message, the producer exits cleanly with status 0 — no
interactive controls, no signal handling, nothing left running. If
`SharedMemoryTransport` construction fails (e.g. the segment already
exists from a previous crashed run — see "v1 simplifications" above), the
producer logs a clear error and exits non-zero rather than hanging or
crashing. This is exercised by the v1 end-to-end test (see "Testing
strategy" in Part I), which is the actual proof this Definition of Done
holds, not just a manual check.

#### v2 Definition of done (producer)

`./producer-cli --payload-size 1024` runs, writes framed messages
(header + payload) into shared memory at a steady rate, responds to
`p`/`r`/`q` and `SIGINT`, exits cleanly with 0 (including a clean thread
join, not a stuck stdin read). Blocks (does not drop) when the consumer is
paused and the ring is full. Correctly recreates the segment after a prior
crash (stale, no live PID) and refuses to start if a live producer already
owns it. Reaches `Closed` and unlinks on clean shutdown, signaling "message
available" **while holding the condition-variable mutex** on both the
`Stopping` and `Closed` transitions (per "Clean producer shutdown and the
receive predicate" in Part I — this is what actually guarantees promptness,
not merely makes it likely: the ordinary per-message signal path is
mutex-free from v3 onward and can miss a waiter, per "Missed notifications
become possible and accepted" in Part I, but this specific signal is not on
that path) so a consumer blocked in `receive()` at that moment is
guaranteed to wake on it, not just eventually via the next bounded-wait
timeout (verified by blocking a consumer's `receive()` on an empty ring,
then quitting the producer, and asserting the consumer observes
`EndOfStream` promptly rather than only after the bound elapses). A
`send()` blocked on a full ring wakes and returns within the bounded-wait
window (not stuck forever) on **local** shutdown (`q`/`SIGINT` →
`Controller` reaches `Stopped`) — per "Crash recovery scope" in Part I, a
`send()` blocked because the consumer quit or crashed does **not** wake on
its own; `SendResult` has no `PeerClosed` case, so this is explicitly not
something this definition of done claims. Generates a fresh `sessionId`
each run and stamps it into every message, so a restarted producer against
a newly-attached consumer is distinguishable from data loss (verified in
Part III's v2 definition of done — per "Crash recovery scope" in Part I,
this covers restart-then-reattach, not restart against a consumer that
stayed attached across the crash).

#### v3 Definition of done (producer)

Identical externally-observable behavior to v2's definition of done above
— v3 changes no producer-visible API or CLI surface — but internally:
`send()` no longer takes the mutex except when the ring is actually full
(verified by profiling or instrumentation showing lock acquisition only at
the boundary condition, not on every call); cursors are confirmed
lock-free via `is_always_lock_free` at compile time and the cross-process
atomic visibility smoke test passes in CI on both macOS and Linux; a
deliberately-induced missed-notification scenario (e.g. a test that
artificially delays a waiter between its predicate check and its
`pthread_cond_timedwait` call) is shown to still recover correctly within
one bounded-wait window, not to hang.

### Part III: Consumer Implementation Plan

Scope: `consumer-cli` only. Part I above is the shared contract this plan
must honor — only what's specific to the consumer is covered here. See
Part II for the producer side.

#### Responsibilities

1. Receive packets from the producer via `ITransport`. **[v1]**
2. Validate each packet: v1 checks sequence-number continuity inline (see
   "v1 error handling" below); v2 adds full checksum recomputation and
   `sessionId`-aware loss/reorder detection via `PacketValidator` (see
   "Defect handling" in Part I).
3. Print stats: v1 prints a final message count once, at exit (see "v1
   Definition of done" below); v2 adds a periodic once-per-second
   `StatsReporter` (total packets received, packets/sec, bytes/sec).
4. Pause/resume/quit via signals or keypress — **[v2]**, v1 has no
   interactive control at all.

Pause semantics (producer blocks when consumer pauses), the ring layout, and
the lifecycle/attach protocol are all defined once in Part I — not restated
here.

#### v1 build order

Mirrors the producer's v1 steps — each sized to roughly 100–200 lines:

1. **Attach to the existing segment** (~50–100 lines) — a single blocking
   `shm_open`/`mmap` attempt against the fixed `/ipc_ring_v1` name (see
   "Naming" in Part I). v1 assumes the producer already created the
   segment — "start the producer, then start the consumer" is a stated v1
   precondition (see "v1 Definition of done" below), so no retry/backoff
   loop is needed yet (that's v2 — see "Attach retries" in Part I). If
   `shm_open` fails, log a clear error and exit non-zero (see "v1 error
   handling" in Part II, same policy on the consumer side).
2. **Receive loop + inline sequence check** (~100–150 lines) — `receive()`
   into a reusable `Message` buffer, check `header.sequenceNumber ==
   lastSeen + 1` on every message (see "Defect handling" in Part I — v1's
   fail-fast version, no `errorCount`, no "keep consuming"); on the first
   message, seed `lastSeen` from it rather than expecting `0` +1, or simply
   require the producer's first `sequenceNumber` to be `0` and check
   accordingly. On any gap: log the expected vs. actual value and exit
   non-zero immediately.
3. **Loop termination + final count** (~50 lines) — v1 needs a defined way
   for the consumer to know it has received all `N` messages and should
   stop calling `receive()`. Since v1 has no `state`/`EndOfStream` (that's
   v2 — see "Clean producer shutdown and the receive predicate" in Part
   I), the simplest v1-appropriate mechanism is: the consumer knows `N`
   ahead of time (e.g. via the same `--count` value used by the producer,
   passed to both sides in a test harness, or the consumer simply receives
   until `receive()` fails or the process is interrupted for a manual run).
   For the automated end-to-end test specifically, both processes are given
   the same `N`, and the test asserts the consumer received exactly `N`
   messages in order. On reaching `N`, print the final count and exit 0.
4. **`consumer_main.cpp` wiring** (~50 lines) — parse args (just `--count`
   if the consumer needs to know `N` — see step 3), attach transport, run
   the receive loop, print final count, exit.

#### v1 error handling

Same policy as the producer (see "v1 error handling" in Part II): `bool`
return from `receive()`, `false` means a fatal condition (attach failure or
an inline sequence-number gap), `ConsumerApp` logs a clear message and
exits non-zero. No `PacketValidator`, no `errorCount`, no result enum.

#### v2 build order

Layered on top of v1's receive loop:

1. **`common::ITransport` reuse with bounded wait** — consumer opens the
   same `SharedMemoryTransport` ring (open-existing instead of create).
   `receive()` now blocks on "message available" using the bounded
   `pthread_cond_timedwait` wake pattern from "Waking a blocked
   `send()`/`receive()`" in Part I, returning `ReceiveResult` (`Received`/
   `Stopped`/`PeerClosed`/`Malformed`/`EndOfStream`) instead of v1's `bool`.

   Consumer never creates the segment and never calls `shm_unlink` —
   producer owns both. Attach sequence follows "Lifecycle and ownership"
   (Part I) exactly:
   - `shm_open` fails `ENOENT` → producer not up yet (consumer may
     legitimately have started first — see "Either process may start first"
     in Part I). Retry indefinitely on a bounded backoff (e.g. every
     100 ms), logging periodically so it's visibly waiting rather than
     hung; no fixed give-up timeout, since that would misfire on a
     consumer deliberately started well before its producer. `Ctrl-C` is
     the escape hatch if that's not what the operator wanted.
   - `shm_open` succeeds but `state != Ready` → producer still
     initializing; keep backing off exactly like `ENOENT`.
   - `state == Ready` → check `layoutVersion` before touching cursors or
     `Header`; mismatch is reported to `ConsumerApp` as a hard error (see
     "Fatal-error policy" in Part I), which logs and exits non-zero — not a
     best-effort read, and not a decision the transport layer makes for
     itself.
   - `state == Stopping`/`Closed` mid-run → drain whatever remains in the
     ring (`receive()` keeps returning `Received` for buffered messages
     regardless of `state` — see "Clean producer shutdown and the receive
     predicate" in Part I), then `receive()` returns `EndOfStream` once the
     ring is actually empty; exit on `EndOfStream`, not as an error.
2. **`PacketValidator`** — given the `Message` populated by `receive()` (its
   buffer already holds a copy of the slot's bytes — see "Wire format" in
   Part I):
   - Recompute CRC32 over header fields (including `sessionId`) + payload,
     compare to `header.checksum` (see "Checksum: algorithm and scope" in
     Part I).
   - Check `header.sessionId` against the last-seen session ID **first**,
     before any sequence check (see "Session ID: distinguishing restart from
     loss" in Part I):
     - Changed → new producer session. Reset `lastSeen` to
       `header.sequenceNumber`, update the stored `sessionId`, and do **not**
       flag this message as a defect regardless of what its
       `sequenceNumber` is — a session change is expected to reset the
       counter.
     - Unchanged → proceed to the sequence check below.
   - Compare `header.sequenceNumber` to `lastSeen + 1`; any gap or
     out-of-order value **within the same session** is a **defect**, not
     routine data — the ring is ordered and lossless by construction (see
     "Defect handling" in Part I).
   - Compare `header.payloadSize` to actual received byte count. By this
     point `receive()` has already bound-checked `header.payloadSize`
     against `Config.payloadSize` before copying anything (see
     "Malformed-frame policy" in Part I) — a mismatch surviving to here
     means the transport copied fewer/more bytes than `payloadSize` claimed
     despite that check passing, i.e. a bug in `SharedMemoryTransport`
     itself, not a malformed wire frame. Still worth asserting explicitly:
     it's what catches that class of bug in the automated tests.

   On any defect (checksum mismatch, in-session sequence gap/reorder, size
   mismatch): increment a dedicated atomic `errorCount` — kept separate from
   the `StatsReporter` counters below — log the specifics (including the
   `sessionId` it occurred in), and keep consuming (a real change from v1's
   fail-fast policy — see "Defect handling" in Part I for why v2 relaxes
   this). No exceptions for bad packets; `errorCount` is what automated
   tests assert against (see "Testing strategy" in Part I), not a crash.
3. **`StatsReporter`** — two separate atomic counter pairs, not one:
   - **Cumulative** (`totalPackets`, `totalBytes`): only ever incremented,
     read for the `total=` figure. Never reset.
   - **Interval** (`intervalPackets`, `intervalBytes`): incremented
     alongside the cumulative counters on each received packet, and
     atomically read-and-reset once per second to compute `pkts/s`/`bytes/s`.

   **`bytes` means complete frame bytes — `sizeof(Header) + payloadSize` per
   message — not payload bytes alone.** This was previously left undefined.
   Frame bytes, not payload-only, because `bytes/s` exists to characterize
   what actually moved through the ring and the copy path (see "Efficiency
   definition" in Part II — throughput is priority 3): every message costs a
   full `Header` copy in addition to its payload, on both `send()` and
   `receive()` (see "`send()`/`receive()` contract" in Part I), so a
   payload-only count would systematically undercount actual data movement,
   more so at small `payloadSize` where `Header`'s fixed 32 bytes is a larger
   fraction of the frame. Increment by `sizeof(Header) + header.payloadSize`
   per successfully received message (i.e. per `ReceiveResult::Received`) —
   not per `receive()` call, since `PeerClosed`/`Malformed`/`EndOfStream`
   moved no frame.

   **`pkts/s`/`bytes/s` measures end-to-end application throughput, not a
   transport-only benchmark — it includes everything on the critical path
   from producer payload generation through consumer bookkeeping, with
   nothing excluded.** An earlier version of this document claimed
   producer-side payload generation was excluded from this measurement,
   reasoning that `IPayloadGenerator` runs on the producer, not inside the
   consumer's per-message loop where `StatsReporter` lives, and is
   therefore "not on the path any consumer-side stats measurement
   observes." **That reasoning is wrong, and the claim is retracted.**
   Which *process* executes a piece of work is not the same question as
   whether that work affects the *rate* being measured: `pkts/s`/`bytes/s`
   counts messages the consumer actually receives per second, and the
   consumer can only receive as fast as the producer sends. If
   `IPayloadGenerator::generate()` is slow, the producer spends more
   wall-clock time per message before each `send()`, its send rate drops,
   and the consumer's `receive()` calls — bounded by "message available" —
   take proportionally longer to unblock. Slower generation shows up
   directly as lower `pkts/s`/`bytes/s`, exactly as if the slowdown had
   happened inside the consumer. There is no path that separates "how fast
   can the consumer read" from "how fast can the producer produce" in this
   design — there is no read-ahead buffer or decoupling beyond the ring
   itself, and the ring only smooths short bursts, not a sustained
   generation slowdown.

   So: this statistic reflects the **whole pipeline** — producer payload
   generation, producer CRC computation, the transport (`send()`/
   `receive()`, including the copy and the wait/wake path), consumer CRC
   recomputation (`PacketValidator`), and consumer bookkeeping
   (`StatsReporter` itself) — all of it, unavoidably, because it's a
   measurement of observed message arrival rate, not of any one stage in
   isolation.

   **A transport-only benchmark, if one is wanted later, is explicitly a
   different, not-yet-built measurement** — not a reinterpretation of this
   statistic. It would need: pre-generated or reused payload buffers on the
   producer side (removing `IPayloadGenerator`'s per-message cost from the
   timed region), and timing taken narrowly around `send()`/`receive()`
   calls themselves rather than around the whole consumer loop (removing
   `PacketValidator`/`StatsReporter` from the timed region on the consumer
   side too). This is a legitimate future benchmarking addition — see
   "Add a benchmark suite" in the project's Roadmap (README.md) — not
   something v2's `StatsReporter` output already provides.

   Reusing one counter for both `total` and the per-second rate doesn't
   work — resetting it to compute the rate would destroy the running total.

   Read the two interval counters as close together as possible (e.g. swap
   both via a single `exchange` under one lock, or accept the two atomics
   may describe very slightly different instants) — the reporter thread
   doesn't need to synchronize with the receive loop beyond that, since
   `std::atomic<uint64_t>` makes each individual counter update safe.

   Use a **monotonic clock**, not `sleep_for(1s)` in a loop — `sleep_for`
   drifts (each iteration's wake time is "last wake + 1s + scheduling
   slop", so error accumulates across the run). Instead, compute each
   target wake time from a fixed reference point:
   ```cpp
   auto next = std::chrono::steady_clock::now();
   while (running) {
     next += std::chrono::seconds(1);
     std::this_thread::sleep_until(next);
     // read-and-reset interval counters, print
   }
   ```
   `steady_clock` specifically — not `system_clock`, which can jump on
   clock adjustments (NTP sync, manual changes).

   Output:
   ```text
   total=<n> pkts/s=<n> bytes/s=<n>
   ```
4. **`common::Controller` reuse** — same `Running/Paused/Stopped` class as
   producer. Consumer's "paused" means: stop calling `receive()`/stop
   advancing the read cursor, per "Pause semantics" in Part I.
5. **Control input** — same stdin thread (`p`/`r`/`q`) + signal-safety
   pattern as producer (self-pipe or `sigwait()`, interruptible stdin read
   on quit — see "Control loop and signal safety" in Part I), reused from
   `common/control/`.
6. **Consumer loop rework** — `receive()` into a reusable `Message` buffer →
   validate via `PacketValidator` → update stats counters → loop; check
   controller state each iteration. On `ReceiveResult::EndOfStream` (see
   "Clean producer shutdown and the receive predicate" in Part I), stop
   looping and exit cleanly — this is the expected end of a normal run, not
   an error path, replacing v1's `--count`-based termination.
7. **`consumer_main.cpp` rewiring** — parse the fuller arg set, open
   transport, construct validator + stats reporter + controller, spawn
   control thread and stats thread, run consumer loop, join on quit.

#### v3 build order

No consumer-visible change — the consumer's `ITransport` usage is identical
to v2; the atomics/lock-free rewrite lives entirely inside
`SharedMemoryTransport` (see Part II's v3 build order). Verifying v3's
Definition of Done (see Part II) exercises the consumer as a black box
against the upgraded transport, nothing in `consumer_app.{h,cpp}` changes.

#### Explicitly deferred (consumer)

- Multi-consumer fan-out (broadcast-blocks-slowest vs. independent
  per-consumer queue) — 1:1 at every planned iteration; revisit if this
  becomes a real requirement.
- Latency measurement (needs producer timestamp vs. receive timestamp — a
  natural next step once `timestamp`'s semantics are pinned down, but not
  required for v2's "receives and validates" to be done, and `timestamp`
  itself doesn't exist before v2).
- Persisting/exporting stats (file, JSON, etc.) — stdout print is enough at
  every currently-planned iteration.

#### v1 Definition of done (consumer)

Start the producer, then start the consumer. The producer writes `N`
fixed-size, sequenced messages into the bounded shared-memory ring. The
consumer receives the same `N` messages in order — verified via the inline
sequence-number check (see "v1 error handling" above), which exits
non-zero immediately on any gap rather than tolerating one. The producer
blocks when the ring is full; the consumer blocks when the ring is empty
(plain `pthread_cond_wait`, no timeout — see "Cursor synchronization model"
in Part I). Both processes exit normally after the final message: the
producer after sending message `N`, the consumer after receiving message
`N` and printing the final count. **An automated end-to-end test proves
this behavior** (see "Testing strategy" in Part I) — this is the actual
v1 completion criterion, not a manual demo.

#### v2 Definition of done (consumer)

`./consumer-cli` attaches to the producer's shared-memory segment, prints one
stats line per second (`total`, `pkts/s`, `bytes/s`) with the cumulative and
interval counters kept separate and the ticker driven by a monotonic clock
(no visible drift over a multi-minute run). Any checksum failure, sequence
gap, or reorder increments `errorCount` and logs without crashing; a clean
run has `errorCount == 0`. Responds to `p`/`r`/`q` and `SIGINT`/`SIGTERM` with
a clean exit (stdin thread actually joins, signal never calls `Controller`
from handler context). Pausing it blocks the producer (verified by running
both and confirming producer's send-side stalls, not drops, while paused).
Started before the producer, it retries with backoff instead of failing;
started against a mismatched `layoutVersion`, it exits non-zero with a clear
message; it drains and exits cleanly on `ReceiveResult::EndOfStream` once
the ring is empty and the producer has reached `Stopping`/`Closed` — per
"Clean producer shutdown and the receive predicate" in Part I, a
`receive()` blocked at the moment the producer transitions is woken
promptly by the producer's own signal on that transition, not left to
notice only on the next bounded-wait timeout. A `receive()` blocked on an
empty ring wakes and returns `PeerClosed` within the bounded-wait window
when the producer is killed mid-block **and the kill didn't orphan the
process-shared mutex** (e.g. the producer wasn't mid-`send()` at the moment
of death) — per "Crash recovery scope" in Part I, this is best-effort, not
a guarantee for every crash timing: a kill that orphans the mutex instead
leaves the consumer blocked re-acquiring it, with no bounded escape at any
currently-planned iteration. Where the bounded wake *does* occur, this
consumer instance is expected to exit, not stay attached across the
restart. A **new** consumer started after the producer restarts (new
`sessionId`, `sequenceNumber` reset to 0) does **not** increment
`errorCount` on its first message — verified by asserting on a real
producer-restart run that the new consumer's first message is accepted
without a logged defect.

#### v3 Definition of done (consumer)

Identical externally-observable behavior to v2's definition of done above.
Verified indirectly, as a black box, by v3's producer-side Definition of
Done (see Part II) — no consumer-specific behavior changes at v3.

## Editor

`.vscode/` configures the clangd language server (reading
`build/compile_commands.json`) with the Microsoft C/C++ IntelliSense engine
disabled. Install the recommended extensions when prompted. Keep `build/` fresh
(`cmake -S . -B build`) after adding files so clangd sees them.
