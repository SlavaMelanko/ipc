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
cmake -S . -B build                       # configure (also enables the pre-commit hook)
cmake --build build                       # build
./build/producer-cli --count 50000 &      # run: producer, then consumer
./build/consumer-cli --count 50000
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
  deliberately the simplest thing that works — two named POSIX semaphores for
  blocking ("slot free" / "message available"), plus a plain
  process-shared mutex guarding only the cursor increment — not the
  fully-optimized version. No interactivity, no crash recovery, no rich
  diagnostics. See "v1 build order" in Part II/III below.
- **v2 — robustness and observability.** Layered on top of v1's
  semaphore-backed ring without changing `ITransport`'s public shape or the
  ring's core concept: `Controller`/signal handling/interactive control,
  `StatsReporter`, checksums and `MessageValidator`, `sessionId`, `PeerClosed`
  detection, crash recovery and stale-segment handling, configurable
  payload/ring-capacity, malformed-frame handling, and the richer
  `SendResult`/`ReceiveResult` enums this document already specifies in full
  below.
- **v3 — performance.** Purely internal: replace v1's mutex-protected
  cursors with lock-free cross-process atomics, move the mutex off the
  per-message fast path (locked only at the full/empty boundary), and add
  the bounded `sem_timedwait` wait. No producer- or consumer-visible API
  change from v2 — v3 optimizes what's already inside
  `SharedMemoryTransport`. The semaphore-based signal in v1/v2 cannot lose
  a notification in the first place (see "Two distinct sync mechanisms" and
  "v3: lock-free fast path" below) — v3 has no missed-notification recovery
  to build, only the cursor/atomics work itself.

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
  can miss a producer crash that orphans the cursor mutex, per that
  section) — and neither recovers or resends anything that was lost. In
  short: reliable-once-delivered, not fault-tolerant. v1 has no `PeerClosed`
  at all (see "Waking a blocked `send()`/`receive()`" below) — a v1 consumer
  blocked on an empty ring, faced with a dead producer, blocks until locally
  told to stop.
- **Blocking, not async/non-blocking — a deliberate design choice.**
  `send()`/`receive()` block the calling thread rather than returning
  immediately with an EAGAIN-style "would block" result. There is no
  polling API, no callbacks, no async completion planned at any iteration.
  This matches the stated efficiency priority of waiting on a named
  semaphore instead of spinning — see "Efficiency definition" in Part II.
  **[v1]** The *mechanism* behind the block changes across iterations (plain
  `sem_wait` in v1; bounded `sem_timedwait` with peer-liveness checks in v2;
  the same bounded wait over a lock-free fast path in v3) but the observable
  behavior — the calling thread blocks, it doesn't spin or return early —
  is a v1 commitment that holds throughout.

#### Two distinct sync mechanisms — do not conflate

This project needs synchronization at two different scopes; they use
different primitives and must not be blurred together. This split is a
**[v1]** decision — it doesn't change across iterations:

| Scope | Used for | Primitive |
|---|---|---|
| **In-process** (threads within one app) | `Controller`'s `Running/Paused/Stopped` state, e.g. control thread signaling the producer/consumer loop — **[v2]**, v1 has no `Controller` | `std::mutex` + `std::condition_variable` — works only within one process, backed by that process's own kernel object |
| **Cross-process** (producer ↔ consumer, separate processes) | Shared-memory ring "slot free" / "message available" signaling — **[v1]** | Two **named POSIX semaphores** (`sem_open`), one per signal — see below |

A plain `std::condition_variable` is **not** valid for the cross-process case
— it's a per-process abstraction over process-local kernel primitives with no
guarantee of working when placed in shared memory (implementation-defined,
and broken in practice on several platforms). The ring buffer's actual choice,
and the alternatives it was weighed against:

- **Named POSIX semaphores (chosen, [v1]).** `NamedSemaphore` (see
  `src/common/transport/shm/named_semaphore.{h,cpp}`) wraps one `sem_open`'d
  semaphore each for `freeSlots` (initialized to `N`, the slot count) and
  `availableMessages` (initialized to `0`) — the standard bounded-buffer
  pattern, modeling exactly the two conditions this ring needs (see
  "Shared-memory ring layout" below). Two semaphores express the two
  independent wait conditions directly, one per condition — no single
  counter or predicate has to serve both. Two properties make semaphores
  the better fit specifically for this cross-process case (over a
  process-shared pthread mutex + condvar, which would otherwise be a
  reasonable choice for API symmetry with the in-process `Controller`):
  - **No mutex-owner-death exposure.** A semaphore has no "owner" to die
    mid-hold — `sem_post`/`sem_wait` don't lock anything a crashing process
    could leave wedged. This sidesteps the entire non-robust-mutex problem
    documented in "Crash recovery scope" below, which a process-shared
    `pthread_mutex_t` used for blocking would have inherited. (v1 still has
    a `pthread_mutex_t` — see below — but its role shrinks to guarding only
    the cursor increment, a window brief enough that the same crash-exposure
    concern is far smaller in practice, and it never gates blocking.)
  - **A post can never be lost.** `sem_post`'s effect on the semaphore's
    count is unconditional and cumulative — a post that arrives before a
    matching wait is still captured, structurally, with no timing window in
    which it could land in an unobservable gap. A condvar signal sent to no
    one currently waiting is simply lost. See "v3: lock-free fast path"
    below for why this matters once the ring's cursors go lock-free.
  - **Named, not unnamed/`pshared`.** The obvious-looking alternative — two
    unnamed `sem_t` placed as fields directly inside `ControlBlock`,
    initialized with `sem_init(&sem, /*pshared=*/1, initialValue)` — was
    tried and **rejected on verified platform grounds**, not by preference:
    macOS's `sem_init()` is a stub that unconditionally returns `-1`/`ENOSYS`
    for both `pshared` values, confirmed empirically on the dev machine (see
    "Cross-platform verification" below). Named semaphores (`sem_open`),
    which live in their own kernel namespace rather than as raw bytes in the
    segment, are the portable choice that actually works on both macOS and
    Linux. This is why the two semaphores are **not** fields of
    `ControlBlock` — see "Shared-memory ring layout" and "Ownership by
    resource" below for what that means for naming and lifecycle.
- **Process-shared pthread mutex + condvar (considered, not chosen for
  cross-process blocking).** `PTHREAD_PROCESS_SHARED` does function
  correctly on this project's target platforms (see "Cross-platform
  verification" below), but named semaphores are chosen instead for the
  reasons above. A process-shared `pthread_mutex_t` is still used in v1 —
  narrowly, to guard only the cursor read-and-increment (see "Cursor
  synchronization model" below), never to gate blocking itself.
- Linux `futex` directly: explicitly **rejected** — Linux-only, and this
  project's initial target is POSIX/macOS-first (per this document). If
  ever needed, it would sit behind a platform adapter, not appear in shared
  application code.

##### Cross-platform verification

Two platform questions here have historically been real risks on macOS
specifically, not something to assume from a man page alone — both were
verified directly rather than taken on faith, since this project's initial
target is POSIX/macOS-first:

- **`PTHREAD_PROCESS_SHARED` for `pthread_mutex_t`** (verified on this
  machine — macOS 26.2, Darwin 25.2.0, arm64): a standalone C test program
  created a `shm_open`/`mmap` segment, initialized a `pthread_mutex_t` with
  `PTHREAD_PROCESS_SHARED` inside it, `fork()`'d, and had the child and
  parent both lock/unlock it around a shared counter. Passed — this is what
  backs `ControlBlock::cursorMutex` (see "Cursor synchronization model"
  below). **Linux**: not independently re-verified in this pass (glibc's
  support for process-shared pthread mutexes is long-standing and
  well-documented), but should get the same smoke test in CI rather than
  resting on reputation alone.
- **Unnamed/`pshared` POSIX semaphores via `sem_init()`** (verified on this
  machine): a standalone C test program called `sem_init(&sem, 1, N)` (and,
  separately, `sem_init(&sem, 0, N)`) directly — both calls returned `-1`
  with `errno == ENOSYS` (`Function not implemented`), unconditionally, with
  no shared-memory segment or `fork()` even required to observe the
  failure. This confirmed Apple never implemented `sem_init()` on Darwin
  (it's a deprecated stub), which is why **named** semaphores (`sem_open`)
  are the chosen mechanism instead, not unnamed `sem_t` fields inside
  `ControlBlock`. **Linux**: `sem_init()` does work there (glibc
  implements it), but named semaphores are used uniformly on both
  platforms rather than branching the ring's implementation per platform.

`BlockingRingBuffer` (see `src/common/transport/shm/blocking_ring_buffer.{h,cpp}`)
picks **named POSIX semaphores** as the concrete cross-process mechanism,
realized as the two independent signals — `freeSlots`/`availableMessages` —
described in "Shared-memory ring layout" below. The in-process `Controller`
(v2) keeps its own separate `std::mutex` + `std::condition_variable`; the two
are never the same primitive, per the table above.

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

**Why the `static_assert` alone is sufficient — no runtime cross-process
test.** The `static_assert` catches "does this type use a lock
internally," which is distinct in principle from the more specific
cross-process question — "does a release-store in one process's mapping
become visible, via an acquire-load, in a *different* process's mapping of
the same physical pages." Standard `std::atomic` is specified in terms of
the C++ memory model, defined over a single program's threads; nothing in
the standard explicitly extends its guarantees across process boundaries.
In practice, though, a lock-free atomic is implemented as a plain CPU
load/store/RMW instruction with no process-local state, and that
implementation works across shared physical pages on every mainstream
platform this project targets — unlike `PTHREAD_PROCESS_SHARED`, which has
a documented history of being unsupported or stubbed on a real target
platform (see "Cross-platform verification" above, macOS's `sem_init()`),
there is no comparable platform risk here to justify a `fork()`-based
runtime smoke test (see "Cross-process atomic visibility" in "Testing
strategy" below). `is_always_lock_free` is sufficient on its own.

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
- Producer's `send()` blocks on the `freeSlots` named semaphore (the "slot
  free" signal described above) — no busy-wait, no dropped messages.
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
  the 1:1 v1 scope). Producer appends messages at the **tail** (its write
  cursor) continuously, with no per-message wait for the consumer; consumer
  reads from the **head** (its read cursor) one message at a time. Producer
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
    holds more than just slots: a **control block** (`ControlBlock`) at the
    front precedes the slot array — in v1, a `pthread_mutex_t` guarding
    only the cursor increment (not blocking itself — see "Cursor
    synchronization model" below) plus the two cursors; from v2 onward also
    `state`, `layoutVersion`, producer PID — see "Lifecycle and ownership"
    below. The two named semaphores (`freeSlots`/`availableMessages`) are
    **not** part of `ControlBlock` and are not counted in
    `controlBlockSize` below — they live outside the segment entirely, as
    their own OS resource with their own name and lifecycle (see "Two
    distinct sync mechanisms" above). Slot count is derived at startup as:
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
  `MessageValidator`/`errorCount`/keep-consuming policy — see "Defect
  handling" below.

##### Cursor synchronization model

This is the exact concurrency model — stated explicitly because "two named
semaphores," "a mutex guarding only the cursor," and (from v3) "atomic
cursors" don't compose into one unambiguous design on their own. Getting
this wrong changes visibility, risks a lost wakeup, and decides whether
producer and consumer can ever touch the same slot concurrently.

**v1 baseline — semaphores block, a mutex only guards the cursor claim:**

v1's `send()`/`receive()` never hold a mutex across the whole call — only
at the full/empty boundary, per the semaphore-based design below, which
keeps the mutex's job narrow:

- **Cursors are ordinary `uint64_t`, not atomics.** Each cursor (write
  cursor, read cursor) lives in the control block as a plain integer. There
  is no lock-free fast path in v1 — this is a deliberate simplification,
  not a placeholder: v1 exists to prove the ring's *shape* (bounded SPSC,
  backpressure, no drops), not to prove its final performance
  characteristics, which is v3's job.
- **Two named semaphores** — `freeSlots` and `availableMessages` — are the
  *only* thing blocking is built on; no condition variable exists in this
  design at any iteration:
  - **`freeSlots`** (consumer → producer): initialized to the slot count.
    Producer's `sem_wait(freeSlots)` blocks when the ring is full — this
    *is* the full/empty boundary check, by construction (the semaphore's
    count already encodes "how many slots are free," so there's no
    separate predicate to compute or re-check); consumer's
    `sem_post(freeSlots)` runs after it finishes reading a slot.
  - **`availableMessages`** (producer → consumer): initialized to `0`.
    Consumer's `sem_wait(availableMessages)` blocks when the ring is empty;
    producer's `sem_post(availableMessages)` runs after it finishes writing
    a slot.

  Collapsing these into one semaphore/predicate works but forces every
  waiter to re-check ring state on each wake; two signals let each side
  wait on exactly the condition it cares about, with no predicate loop
  needed at all — the semaphore's count already *is* the predicate.
- **The cursor mutex's job is narrow: claim-and-increment only, not
  gate blocking.** `ControlBlock::cursorMutex` is taken only *after* the
  relevant `sem_wait` has already returned (the slot is known to be free
  or available), held just long enough to read and increment the one
  cursor this call owns, then released — the slot copy (the `memcpy` of
  header + payload) happens entirely outside the lock. Concretely:
  `send()` calls `sem_wait(freeSlots)`, locks `cursorMutex`, does
  `writePos = writeCursor++`, unlocks, copies into slot `writePos`
  unlocked, then `sem_post(availableMessages)`. `receive()` is symmetric
  on `availableMessages`/`readCursor`/`freeSlots`. The mutex here is held
  for a handful of instructions, never for the duration of a copy, and
  never gates whether a call blocks at all — that's the semaphores' job
  entirely.
- **v1 has no lost-wakeup risk — and not because of a mutex-guarded
  predicate pattern.** A semaphore's `sem_post` is unconditional and
  cumulative: a post that arrives before its matching `sem_wait` is still
  captured in the count, with no timing window in which it could land in
  an unobservable gap the way a condition-variable signal sent to no
  current waiter would be lost. This guarantee holds regardless of
  whether the cursor claim is mutex-protected (v1/v2) or lock-free (v3) —
  see "v3: lock-free fast path" below for why this means v3 doesn't need
  to accept or recover from a missed notification at all.
- **Blocking only at full/empty** — producer blocks only when the ring is
  full (all `N` slots occupied); consumer blocks only when the ring is empty
  (write cursor caught up to read cursor). No busy-waiting: waits are
  ordinary `sem_wait` (v1) or, from v2 onward, bounded `sem_timedwait` —
  see "Waking a blocked `send()`/`receive()`" below.

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
  cursor — no lock. Blocking itself is already handled entirely by the
  `freeSlots`/`availableMessages` semaphores at every iteration (see
  "Cursor synchronization model" above) — v3 removes the mutex from the
  cursor claim as well, so nothing on the `send()`/`receive()` fast path
  takes a lock at all; the mutex simply has no remaining job once cursors
  are atomics.
- **Each cursor has exactly one reader and one writer, both the same
  process — the other side never touches it.** `writeCursor` is claimed
  (`fetch_add`) and read only by the producer; `readCursor` only by the
  consumer. Neither process ever loads the *other* process's cursor. This
  is what actually justifies `std::memory_order_relaxed` on both
  `fetch_add`s: ordering annotations on an atomic exist to constrain what a
  *different* thread observes around it, and here no other thread or
  process ever observes this variable at all — there is nothing for
  `release`/`acquire` to order against. A single-owner counter needs no
  more ordering guarantee than a plain local variable would, regardless of
  which process's address space it lives in.
- **The cross-process fence that actually matters is the semaphore pair,
  not the cursors.** What must be ordered is "producer's slot write" →
  "consumer's slot read," and that edge is already established by
  `sem_post`/`sem_wait` themselves: POSIX guarantees a `sem_post` (release)
  paired with the `sem_wait`/`sem_timedwait` that observes it (acquire)
  forms a full memory fence, exactly like `std::memory_order_release`/
  `_acquire` do. Producer writes the slot, then `sem_post(availableMessages)`;
  consumer's `sem_wait(availableMessages)` returns, then it reads the slot
  — the post/wait pair is the publication edge, not the cursor store/load.
  Symmetric for slot reuse: consumer reads the slot, `sem_post(freeSlots)`;
  producer's wait on `freeSlots` returning is what makes the slot safe to
  overwrite. The cursor value itself only ever needs to be correct within
  its one owning process's program order (compute slot index, then use it
  to index into the slot array) — ordinary sequential execution already
  guarantees that, no atomic ordering required beyond what `relaxed`
  provides (atomicity of the increment itself, so two calls into
  `AcquireWriteSlot()` from the same process — not a concern here, since
  each transport instance is used by one thread — wouldn't race).
- **No concurrent access to the same slot.** The producer only ever writes
  a slot after its own `sem_wait(freeSlots)` for that slot's turn has
  returned, which only happens after the consumer's `sem_post(freeSlots)`
  for that same slot — i.e. after the consumer finished reading it. The
  consumer only ever reads a slot after its `sem_wait(availableMessages)`
  has returned, which only happens after the producer's
  `sem_post(availableMessages)` for that slot — i.e. after the producer
  finished writing it. The semaphores' own accounting (a slot is only
  claimable once the matching `sem_wait` returns) is what prevents the two
  processes from touching the same slot at the same time; this holds
  identically whether the cursor claim is mutex-protected (v1/v2) or
  lock-free relaxed atomics (v3) — the cursor mechanism was never what
  provided this guarantee, in any iteration.
- **No missed-notification problem to accept, because semaphores don't have
  one.** A classic condition-variable design risks a lost wakeup: a signal
  sent by a notifier that isn't also holding the mutex across its state
  change can land in a window where no one is currently waiting, and is
  then lost — the mutex-guarded-predicate no-lost-wakeup guarantee only
  holds when the notifier holds the mutex too, which a lock-free fast path
  (deliberately) doesn't do. That risk doesn't apply here, because this
  design is semaphore-based, not condvar-based (see "Two distinct sync
  mechanisms" above): `sem_post`'s effect on a semaphore's count is
  unconditional and cumulative, with no notifier-side mutex involved at any
  iteration, so there is no window in which a post can land where no one is
  waiting and be lost. This holds at v1, v2, and v3 alike, including once
  the cursor claim itself goes lock-free in v3 below — going lock-free
  changes who holds `cursorMutex` and for how long, but never touches the
  `freeSlots`/`availableMessages` semaphores' own guarantee, since that
  guarantee never depended on the mutex in the first place. There is
  consequently no "accept the miss, let a bounded wait recover" tradeoff to
  make in v3 — the bounded `sem_timedwait` (see "Waking a blocked
  `send()`/`receive()`" below) exists for local-shutdown and peer-death
  detection, not to recover from a missed notification, because there is
  no missed-notification case to recover from.

This layout is shared code (`src/common/transport/shm/shared_memory_transport.{h,cpp}`)
at every iteration — producer and consumer link the same ring implementation,
one opens/creates it, the other opens the existing segment.

##### Waking a blocked `send()`/`receive()` on shutdown or peer death

**v1 baseline:** a blocked `send()`/`receive()` calls a plain `sem_wait`
(no timeout) on the relevant semaphore. It wakes only when the semaphore's
count allows it — there's no spurious-wake case to handle the way a
condition variable would need (a `sem_wait` return means the count was
actually decremented, not merely "something happened, re-check"). **v1 has
no notion of "local shutdown while blocked" or "peer death while
blocked"** — there is no `Controller` to signal a local stop (see "Control
loop and signal safety" below — **[v2]**), and no PID-liveness check for a
dead peer. A v1 producer blocked on a full ring with a dead or exited
consumer, or a v1 consumer blocked on an empty ring with a dead or exited
producer, simply stays blocked — this is accepted v1 scope (see "v1 error
handling" below): v1's fail-fast posture is about *malformed data*, not
about *peer liveness*, which is a v2 concern.

**From v2 onward**, blocked calls must wake on a local stop request and
(best-effort) a dead peer, not just the predicate. Detection is
**asymmetric by design**: only the producer's PID/lifecycle state lives in
the segment (see "Lifecycle and ownership" below), so only the *consumer*
can detect a dead/quit *producer* this way — the reverse is out of scope
(see "Crash recovery scope" below).

**Fix: bounded wait, not blind wait — [v2].** `send()`/`receive()` use
`sem_timedwait` on a short absolute-deadline bound (e.g. 100–200 ms,
recomputed each retry against `CLOCK_REALTIME`), not a blind `sem_wait`. On
every wake, check in order:

1. Predicate met (slot free / message available)? → proceed, done.
2. **`receive()` only:** ring empty **and** `state` is `Stopping`/`Closed`?
   → `EndOfStream` (a producer finishing normally, not an error — see
   "Clean producer shutdown and the receive predicate" below). Checked
   first since it's the most specific condition.
3. Local `Controller` state `Stopped`? → `Stopped`.
4. **`receive()` only:** producer PID no longer alive (`kill(pid, 0)` →
   `ESRCH`)? → `PeerClosed`. No equivalent on `send()`'s side — no stored
   consumer PID to test (see "Crash recovery scope" below).
5. Otherwise recompute the deadline, wait again.

This reuses the PID-liveness check from lifecycle/crash recovery,
consumer-side only, bounding wake latency to the wait granularity. A
blocked `send()` has no equivalent bound on peer death — it wakes on its
own `Controller`'s `Stopped`, never on consumer absence.

**Why `PeerClosed` is still only best-effort.** Detection itself is
unconditional — POSIX semaphores have no owner/mutex to wedge on, unlike
`pthread_cond_timedwait`, so step 4 always runs after every wake. What
remains best-effort is the *subsequent* cursor claim: if the producer died
holding `cursorMutex` (a window of only a few instructions around
`writeCursor++`), the consumer's claim attempt blocks independent of the
`PeerClosed` check having already succeeded. Full closure needs either
`PTHREAD_MUTEX_ROBUST` or moving the cursor claim off a mutex entirely
(v3's atomics work removes it — see "v3: lock-free fast path" above); not
implemented through v2.

This is why, from v2 onward, `ITransport::send()`/`receive()` return a
small result enum instead of `bool` (**v1 uses plain `bool`** — see "v1
error handling" below) — **the two enums are not symmetric**:

```cpp
// v2
enum class SendResult { Sent, Stopped };
enum class ReceiveResult { Received, Stopped, PeerClosed, Malformed, EndOfStream };
```

`Sent`/`Received`: predicate met, completed normally. `Stopped`: local
`Controller` reached `Stopped` while waiting. `PeerClosed` (receive-only,
best-effort per above): producer PID check failed. `Malformed`
(receive-only): `header.payloadSize` mismatch — see "Malformed-frame
policy" above. `EndOfStream` (receive-only): ring empty and `state` was
`Stopping`/`Closed` — expected, not a failure, see "Clean producer
shutdown" below. `send()` has no `PeerClosed` case at any iteration — a
real limitation, see "Crash recovery scope" below. Both enums: returned by
value, fixed underlying `int`, no allocation. Neither covers OS-level
failures or the `Malformed` case's escalation to process exit — see
"Fatal-error policy" below.

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

**From v2 onward:** `SendResult`/`ReceiveResult` describe blocking-wait
outcomes only, not the full space of transport failures. Two other
categories need a stated policy:

- **OS-level failures**: `shm_open`/`mmap`/`ftruncate`/`sem_open` errors,
  `pthread_mutex_init` failing, `sem_timedwait` returning anything but
  `ETIMEDOUT`. Programmer/environment errors, not conditions a caller
  should branch on and recover from.
- **Protocol failures**: `layoutVersion` mismatch at attach (see "Lifecycle
  and ownership" below), the mismatched-`payloadSize` `Malformed` case
  above. The two processes disagree about wire format or the ring is
  corrupted — not safe to paper over.

**Policy: both categories are fatal — log clearly, exit non-zero — but the
transport itself never calls `exit()`/`abort()`/`std::terminate()`.**
`SharedMemoryTransport` reports failure through an ordinary return value
(e.g. `std::expected<SharedMemoryTransport, TransportError>` at
construction, or `enum class TransportError { OsFailure, LayoutMismatch }`
plus a logged detail string) — never a thrown exception, never a direct
`exit()` from transport code. This keeps the transport unit-testable in
isolation. `ProducerApp`/`ConsumerApp` own logging and the final non-zero
exit at every iteration: they inspect the result, log the detail, and
return non-zero from `main()`. Same split applies to `Malformed`:
`receive()` returns the value, `ConsumerApp`'s loop logs and exits. The
error type itself can stay a small enum plus a logged string — the
detect/act-on split is the requirement, not the type's sophistication.
This same principle already applies in v1 with only a `bool` to report it.

No exceptions thrown across the `ITransport` boundary, no `errno`-style
out-parameter, no retry or self-heal, at any iteration.

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
- **`sessionId`** — **[v2]**. Identifies one producer run — see "Session
  ID: distinguishing restart from loss" below. v1 has no restart
  disambiguation at all — the field doesn't exist yet.
- **`timestamp`'s clock, unit, and capture point are deliberately left
  undefined even once introduced in v2** — reserved, not interpreted.
  Nothing reads it for any decision; its only role is as one of the fields
  the checksum protects, where presence/stability matter but meaning
  doesn't. Producer populates it with `steady_clock::now()`'s count at
  header-build time — a safe default, not a committed contract. Latency
  measurement is what would need this pinned down precisely, and that's
  deferred beyond v2 (see "Explicitly deferred (consumer)" in Part III).
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
direction — not a zero-copy borrowed view, at v1, v2, or v3.** A
borrowed-view API — `receive()` returning a `MessageView` directly into
the ring slot, with the read cursor advancing only on an explicit
`release(view)` call — is rejected: it requires a manual lifetime contract
(forgetting to `release()` deadlocks the producer, since the slot is never
freed), a transport-specific `release()` concept with no precedent
elsewhere in this design, a genuine use-after-free hazard if `view` is
read after `release()`, and no clean equivalent on the producer side,
which would still need *some* reserve/write/publish sequence to build
directly into a slot it "already owns." None of this is worth taking on
before the 1:1 happy path even works, and v3's performance work (see
above) targets the mutex, not this copy — a zero-copy borrowed-slot API
remains a valid *future* optimization beyond v3, not something any
currently-planned iteration needs.

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
  A v2 `MessageValidator`'s checksum/sequence checks (see Part III) run
  **after** this point and cannot substitute for it — they operate on a
  `Message` that, by their point in the pipeline, has already been fully
  copied; the payloadSize bound-check has to happen inside `receive()`
  itself, before the copy, or it happens too late to matter. **v1** has no
  `MessageValidator` at all — the bound check inside `receive()` plus a
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
all, so there is no valid `Message` for `MessageValidator` to inspect and no
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

**v1 has no session disambiguation** — this mechanism only becomes
relevant once v2 introduces crash recovery and stale-segment recreation
(see "Crash recovery scope" below).

A sequence-number discontinuity is ambiguous: `..., 41, 42, 43, 0, 1, 2,
...` could mean the producer restarted (new process, counter reset,
nothing lost — a new session) or a genuine loss/reorder defect within the
same session (see "Defect handling" below). `sequenceNumber` alone can't
tell them apart.

**Fix: `sessionId`, a random `uint64_t` generated once at producer
startup** (random, not PID+start-time, to sidestep PID reuse across
reboots). Producer copies it into **every** `Header` for that run — lives
only in `Header` (wire format), not segment metadata, since the segment
already has its own PID-based liveness check for a distinct concern (crash
recovery, see "Lifecycle and ownership" below).

Placing it per-`Header` (not attach-only metadata) means a **new**
consumer attaching *after* a restart correctly treats the reset
`sequenceNumber` as a new session, not a defect, from its very first
message. It does **not** let an *already-attached* consumer follow a
restart mid-stream — that needs detach/reattach to the new segment, out of
scope at any iteration (see "Crash recovery scope" below). Per-`Header`
placement is kept as future-compatible metadata for that feature without
a wire-format change later.

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

- **CRC32, not FNV-1a.** FNV-1a targets hash-table distribution, not
  bit-level corruption detection. CRC32 has guaranteed detection
  properties for common corruption patterns (burst errors, single/double
  bit flips) — exactly what a checksum on an IPC channel needs to catch.
  Used unconditionally; no pluggable strategy planned.
- **Checksum covers header fields *and* payload, not payload alone**
  (`checksum` itself excluded from its own input):
  ```text
  checksum = crc32(bytes_of(sessionId, timestamp, sequenceNumber, payloadSize) ++ payload)
  ```
  A payload-only checksum would leave `sequenceNumber` (feeding the
  consumer's own loss/reorder detection), `sessionId` (the restart/loss
  disambiguation itself), and `payloadSize` unprotected. The `payloadSize`
  bound-check in `receive()` still can't depend on the checksum — see
  "`send()`/`receive()` contract" above — it must run first, independent
  of CRC.

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
  stats counters (total/interval message and byte counts; see each plan's
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

- **stdin thread** (`p`/`r`/`q`): a thread blocked in `read()` on stdin
  can't be woken by a condition variable. Fix: on quit, close/shutdown the
  stdin fd (or use a cancellable read) so the blocking read actually
  returns.
- **Signals** (`SIGINT`/`SIGTERM`): a handler must not call
  `controller.pause()/resume()/stop()` directly — `std::mutex` isn't
  async-signal-safe and risks deadlock if the signal interrupts a thread
  already holding it. Two accepted patterns: **self-pipe** (handler
  `write()`s a byte, a dedicated thread `read()`s it and calls
  `controller.stop()`), or **signal-waiting thread** (block the signals at
  startup, one thread calls `sigwait()`, calls `controller.stop()` after
  it returns). Either way, no controller method runs from actual signal
  handler context.

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
| `pthread_mutex_t cursorMutex` (inside the segment, in `ControlBlock`) | Producer (lives in the segment producer created) | Producer (`pthread_mutex_init` with `PTHREAD_PROCESS_SHARED`, at construction) | **Not explicitly destroyed** — no `pthread_mutex_destroy` call; reclaimed implicitly when the backing page is unlinked and unmapped by every process (see "Teardown ordering" below). Consumer only ever takes it briefly to claim its read cursor |
| `freeSlots`/`availableMessages` named semaphores (own kernel namespace, **outside** the segment) | Producer (`sem_open(O_CREAT\|O_EXCL)`, names derived from the segment name) | Producer (`freeSlots` initialized to the slot count, `availableMessages` to `0`, at construction) | Producer (`sem_unlink`, paired with `sem_close`; see "NamedSemaphore" in "Two distinct sync mechanisms" above). Consumer `sem_close`s its own handle on exit but never unlinks |
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
why consumer never calls `shm_unlink` or `sem_unlink` — cleanup for
producer-owned resources happens only in the process that created them, so
there's never a "who cleans this up" ambiguity. Note `cursorMutex`
specifically is **not** explicitly destroyed by anyone, producer included —
see "Teardown ordering" below for why; the two named semaphores, unlike the
mutex, *are* explicitly cleaned up (`sem_unlink`, producer-only), since they
live outside the segment and aren't reclaimed by unmapping it.

- **Naming** — fixed names at every iteration, not user-configurable, and
  there are now **three** names to manage, not one: the shared-memory
  segment `/ipc_ring_v1` (`shm_open`), and two named semaphores derived
  from it (`freeSlots`, `availableMessages` — see `NamedSemaphore` in "Two
  distinct sync mechanisms" above for why they're separate OS resources
  rather than fields inside the segment's `ControlBlock`). The *names* are
  fixed; the segment's *size* is not — it's computed from
  `Config.ringCapacityBytes` at creation time (see "Shared-memory ring
  layout" above): a fixed constant in v1, a
  `--ring-capacity`/`--payload-size`-driven value from v2 onward.
- **Permissions** — `shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600)` and,
  identically, `sem_open(name, O_CREAT | O_EXCL, 0600, initialValue)` for
  both semaphores: owner read/write only, on every named resource this
  project creates (see `kOwnerReadWrite` in `common/util/posix.h`). Producer
  and consumer are expected to run as the same user; no cross-user sharing
  in scope at any iteration.
- **Either process may start first — [v2].** v1 assumes the producer
  starts first (see "v1 simplifications" below). From v2 onward, handled
  asymmetrically by *role*, not by *race*: producer is the only process
  that ever creates the segment (`O_CREAT | O_EXCL`); consumer only ever
  opens existing (`O_RDWR`, no `O_CREAT`), regardless of start order.
  Consumer starting first just means it waits longer — the segment's
  content is producer-defined config, so a symmetric "whoever gets there
  first creates it" model isn't used.
- **Attach retries — [v2].** v1's consumer does a single blocking attach
  (see "v1 simplifications" below). From v2 onward: `shm_open`/`sem_open`
  failing (`ENOENT`, or producer mid-startup) retries indefinitely on a
  bounded backoff (e.g. 100 ms), logging periodically — not an error
  condition, since a consumer legitimately can start well before its
  producer. A bounded give-up isn't required for correctness.
- **Initialization readiness — [v2].** v1 has no `state` field (see "v1
  simplifications" below) — segment existing is enough to attach. From v2
  onward, segment *existing* isn't sufficient: a `state` field (see state
  machine below) gates it. Producer writes its own PID **first** (see
  "Stale segments after a crash" below for why), sets `Initializing` right
  after `mmap`, does `cursorMutex`/semaphore/cursor setup, then publishes
  `Ready` last. Consumer's retry loop treats `ENOENT` and `state != Ready`
  identically.
- **Layout compatibility — [v2].** A `uint32_t layoutVersion` field after
  `state`, bumped on any `Header`/slot layout change. Consumer checks it
  right after `state == Ready`; mismatch → log and exit non-zero. v1 has
  no such field — producer and consumer are built from the same source at
  the same time, so mismatch isn't a v1 scenario.
- **`shm_unlink`/`sem_unlink` ownership** — producer only, on clean
  shutdown, at every iteration (v1: on process exit; v2 onward: after
  reaching `Closed`). Consumer never unlinks either; it only detaches its
  mapping and closes its own semaphore handles.
- **Stale segments after a crash — [v2].** v1 has no crash-recovery story
  (see "v1 simplifications" below) — a leftover segment must be removed
  manually before the next run. From v2 onward: producer startup tries a
  plain `shm_open` (no `O_EXCL`) before `O_CREAT | O_EXCL`; on success,
  inspects `state` and the stored PID:
  - `state == Closed` → previous producer shut down cleanly but didn't
    unlink for some reason; safe to unlink and recreate the segment and
    both semaphores.
  - `state` looks live (`Initializing`/`Ready`/`Stopping`) → check whether
    the stored PID is still alive (`kill(pid, 0)`); if not, it's a crash
    artifact — unlink and recreate the segment and both semaphores. If the
    PID *is* alive, another producer is genuinely running — refuse to
    start with a clear error (this project is 1:1 at every iteration, two
    producers on the same segment is a misuse, not a case to silently
    handle).
  - **PID must be valid *before or at* the transition into
    `Initializing`, not deferred to `Ready`.** Writing it only at `Ready`
    would leave a gap: a producer crashing *during* `Initializing` leaves
    the PID field as `ftruncate`'s zero-fill, not a real PID — `kill(0, 0)`
    has special (signal-to-process-group) semantics, not "is PID 0 alive,"
    and a garbage nonzero PID could false-positive against an unrelated
    live process. Fix: producer writes its own PID (fixed field, right
    after `layoutVersion`) as the very first thing after `mmap`, before
    setting `state` to `Initializing` — valid for any `state` a second
    producer might observe.

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
requirement" above (`is_always_lock_free`) **applies to `state` exactly as
it does to the v3 write/read cursors.** `state` is cross-process shared
atomic data read outside any mutex from v2 onward (ahead of the cursors'
own v3 upgrade): the attach-retry loop polls it (see "Initialization
readiness" above), the bounded-wait predicate reads it on every wake (see
"Clean producer shutdown" below). Same publication/consumption discipline
as the cursors — producer release-stores the new `state` only after
completing what it implies (pthread/cursor setup before `Ready`; drain
before `Closed`); readers acquire-load before trusting it. `uint32_t`
(not `uint64_t` like the cursors) is enough range for four states —
`is_always_lock_free` doesn't require matching width across atomics.

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

The blocked-`receive()` wake checks (see "Waking a blocked
`send()`/`receive()`" above) need to cover the ring going empty *because
the producer is finishing normally*, not just going dead — a distinct
outcome, not a fall-through to "wait out the bound, time out, re-check,
eventually notice via some other path." This is a real, common case:
every clean shutdown passes through it, since the producer drains its own
send path, reaches `Stopping`, then `Closed`, well before the consumer
necessarily finishes draining.

**Fix, in two parts:**

1. **Producer posts `availableMessages` on every `state` transition from
   `Ready` onward** (into `Stopping`, again into `Closed`), even with no
   new message written — a deliberate exception to "post only after
   advancing the write cursor" (see "Cursor synchronization model" above),
   since the transition itself is news the consumer needs. No mutex is
   needed to avoid a lost wakeup here: `sem_post` is unconditional and
   cumulative regardless of any mutex (see "Two distinct sync mechanisms"
   above) — a consumer already blocked in `sem_timedwait` at the moment of
   transition is guaranteed woken by this post, not just eventually by the
   next timeout.
2. **The bounded-wait predicate check explicitly includes `state`:** on
   every wake, `receive()` checks "ring non-empty **or** `state` is
   `Stopping`/`Closed`." If empty and `Stopping`/`Closed`, returns
   `ReceiveResult::EndOfStream` instead of continuing to wait. If not
   empty, `receive()` still drains normally regardless of `state` —
   `Stopping`/`Closed` means no more *new* messages, not "discard what's
   buffered."

`EndOfStream` is expected, not an error: the consumer's normal shutdown
path (per Part III) treats it the same way it already treats observing
`Stopping`/`Closed` directly — stop calling `receive()` and exit cleanly,
with no `errorCount` increment (see "Defect handling" above) and no
non-zero exit code attributable to this alone.

##### Teardown ordering

Naively, "producer owns `cursorMutex`, so producer destroys it" (per the
ownership table above) suggests calling `pthread_mutex_destroy` right
before `shm_unlink` on process exit (v1) or on the `Stopping` → `Closed`
transition (v2 onward). **That is unsafe, and not only in a crash — this
applies at every iteration, including v1.** At v2 onward, the `Closed`
state's own definition says the consumer "drains any remaining messages"
after observing it — meaning the consumer is *expected* to still be
calling `receive()` (briefly locking `cursorMutex` to claim its read
cursor) for some window after the producer reaches `Closed`. Even in v1,
which has no `Closed` state, the same underlying race exists the moment
the producer finishes sending and starts tearing down while the consumer
might still be mid-`receive()` on the last few messages. If the producer
destroys the mutex at that point, it races an in-spec, non-crashed
consumer that just hasn't finished draining yet — undefined behavior on
ordinary, successful shutdown, not an edge case requiring a prior crash.

**Policy at every iteration: never explicitly destroy `cursorMutex`.** No
`pthread_mutex_destroy` call, by either process, ever:

1. Producer finishes sending (v1) or reaches `Closed`, publishes it (v2
   onward), and calls `shm_unlink` — this removes the segment's *name* from
   the filesystem namespace but does not invalidate mappings already open
   in either process (standard `shm_open`/`unlink` semantics, same as a
   regular file: existing `mmap`s stay valid until unmapped).
2. Producer `munmap`s its view, then exits.
3. Consumer drains whatever is left in the ring (v1: until its own end
   condition; v2 onward: per the `Closed`-state contract), then `munmap`s
   its own view and exits — independently, on its own schedule, with no
   signal required from the producer beyond having already observed the
   end condition.
4. Once *both* processes have unmapped, the kernel reclaims the underlying
   pages — `cursorMutex` and all — as an ordinary consequence of the last
   mapping going away. Skipping the explicit destroy call costs nothing:
   `pthread_mutex_destroy` on a process-shared mutex backed by shared
   memory doesn't release any resource beyond invalidating the object in
   place, which reclaiming the page already does for free.

This avoids the choice between (a) a consumer-detached acknowledgement
handshake — the producer would have to wait, post-shutdown, for some signal
that the consumer has actually unmapped, which is new protocol surface not
worth adding at any planned iteration — and (b) the race described above.
No handshake, no race: `cursorMutex` is simply never destroyed out from
under a peer that might still be using it, because nothing ever destroys
it at all.

**The two named semaphores don't have this problem.** Unlike `cursorMutex`
(reclaimed passively on unmap), `freeSlots`/`availableMessages` are
separate, explicitly-named OS resources the producer *does* tear down
(`sem_unlink`) after `Closed` — safe for the same reason `shm_unlink` is
safe at step 1: removing the *name* doesn't invalidate handles a process
already holds open.

##### Crash recovery scope — **[v2 partially closes this; some gaps remain beyond v3]**

**v1 has essentially none of this.** v1 assumes both processes stay alive
for the run's duration and shut down cleanly; a killed v1 process leaves a
stale segment that must be cleaned up manually before the next run (see
"Stale segments after a crash" above). Everything below describes what v2's
mechanisms (PID-liveness check, bounded wait, `sessionId`, teardown
ordering) do and do not cover, once they exist.

The v2 mechanisms cover clean shutdown and stale-segment cleanup at next
startup — not full crash recovery. Three gaps remain, out of scope through
v3 (a fourth — teardown racing a still-attached peer — is a clean-shutdown
bug, not crash-only, and is fixed in "Teardown ordering" above, applying
from v1 onward):

- **Only the producer's PID is stored** — no consumer-liveness record, so
  the producer can't detect a gone consumer (crashed or cleanly quit
  alike; neither writes anything to the segment on the way out). A
  producer blocked on a full ring wakes only on its own `Controller` state,
  never on consumer absence — `SendResult` has no `PeerClosed` for exactly
  this reason.
- **`cursorMutex` is not robust.** A process dying while holding it (a
  window of a few instructions around the cursor increment) leaves it
  locked forever. This does **not** block `sem_timedwait` or the
  PID-liveness check — POSIX semaphores have no mutex to re-acquire, so
  `PeerClosed` detection is reachable unconditionally after every wake.
  What remains exposed: the consumer's *next* `receive()` still needs to
  lock `cursorMutex`, and a producer that died mid-increment leaves it
  orphaned. `PTHREAD_MUTEX_ROBUST` would fix this; not implemented through
  v2 — v3 closes it by removing the mutex from this path entirely.
- **A producer restart is invisible to an already-attached consumer.**
  Stale-segment recovery unlinks and recreates the segment, but an
  already-`mmap`'d consumer has no trigger to detach/reattach — it keeps
  reading its stale mapping. The `sessionId` check only helps a consumer
  attaching *after* the restart.

**Explicitly unsupported through v3:** a forced crash of either process
while the other is live, and a live producer restart against an
already-attached consumer. What v2/v3 *do* support: clean shutdown and
stale-segment cleanup at the *next* startup against an unattended segment.
Full crash recovery (robust mutex, consumer-liveness record,
detach/reattach signaling) is future work beyond v3.

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
│   │   ├── checksum.h             # crc32() over header fields + payload -- [v2]
│   │   └── message_validator.{h,cpp}  # checksum + sequence-gap detection, error counter -- [v2]; v1 checks sequence number inline, no separate class
│   ├── transport/
│   │   ├── transport.h            # ITransport -- stays here, not under shm/, so a future non-shm transport is a sibling directory, not a rename
│   │   └── shm/
│   │       ├── mapped_segment.{h,cpp}         # raw shm_open/mmap segment ownership
│   │       ├── named_semaphore.{h,cpp}        # RAII named semaphore (sem_open); not sem_init/pshared -- see "Two distinct sync mechanisms"
│   │       ├── control_block.{h,cpp}          # cursorMutex (claim-only) + write/read cursors; mutex-protected in v1/v2, lock-free atomics in v3
│   │       ├── blocking_ring_buffer.{h,cpp}   # owns MappedSegment + two NamedSemaphores; slot acquire/commit
│   │       └── shared_memory_transport.{h,cpp}  # ITransport impl: message framing over BlockingRingBuffer
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

- **Cross-platform pthread mutex smoke test** — the `fork()`-based
  `PTHREAD_PROCESS_SHARED` mutex check described in "Cross-platform
  verification" above, run in CI on the `ubuntu-26.04` runner (see this
  document's Requirements/Setup sections) to confirm the same guarantee
  holds on Linux, not just the macOS machine it was manually verified on.
  v1 depends on `cursorMutex` being genuinely process-shared from the
  start, so this test belongs at v1, not later.
- **Semaphore platform check** — not a smoke test in the usual sense, since
  the answer on macOS is already known and negative: the `sem_init()`
  (unnamed/`pshared`) check described in "Cross-platform verification"
  above should run in CI on both `ubuntu-26.04` and (if a macOS runner is
  ever added) macOS, so a future toolchain change that silently altered
  this doesn't go unnoticed. This is what justifies named semaphores being
  the uniform choice on every platform this project targets, rather than a
  platform-conditional implementation.
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

- **Cross-process atomic visibility — [v3], covered by static assertion
  only, no runtime smoke test.** See "Cross-process atomics: an explicit
  platform requirement" above for why the compile-time
  `is_always_lock_free` `static_assert` is sufficient on its own, with no
  `fork()`-based runtime check needed alongside it.
- **Shutdown — [v2]** — assert stdin thread and signal path both cause
  clean process exit (join, not force-kill) under `SIGINT`/`SIGTERM` and
  `q`, from `Running`/`Paused`, driving a real `SIGINT` during a blocked
  `receive()`.
- **Corruption — [v2]** — flip a bit in a written slot, assert `errorCount`
  increments and the checksum failure logs, without crashing.
- **Producer restart — [v2]** — restart against an already-attached
  consumer is unsupported (see "Crash recovery scope" above), so: stop the
  consumer cleanly, kill/restart the producer, start a *new* consumer;
  assert stale-segment recovery fires correctly and the new consumer's
  `errorCount` does **not** increment despite `sequenceNumber` resetting,
  because `sessionId` changed.
- **Version-mismatch — [v2]** — consumer against a mismatched
  `layoutVersion` exits non-zero with a clear message.

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
4. Low CPU/memory — named-semaphore wait (v1: plain `sem_wait`; v2 onward:
   bounded `sem_timedwait`), not spin. See "Two distinct sync mechanisms"
   in Part I.

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
3. **`ITransport` interface + `BlockingRingBuffer`/`SharedMemoryTransport`
   construction/attach** (~150–200 lines) — `shm_open`/`mmap`/`ftruncate`
   for the segment (`MappedSegment`), `sem_open` for the two named
   semaphores `freeSlots`/`availableMessages` (`NamedSemaphore`), the
   process-shared `pthread_mutex_t cursorMutex` initialized with
   `PTHREAD_PROCESS_SHARED`, and the two plain-integer cursors, all zeroed
   at construction (see "Ownership by resource" in Part I). v1's attach
   logic is deliberately minimal: producer creates with `O_CREAT | O_EXCL`
   (no stale-segment recovery — see "v1 simplifications" below); consumer
   does one attach attempt assuming the producer already created the
   segment and both semaphores (see "v1 Definition of done" below — "start
   the producer, then start the consumer" is a stated v1 precondition, not
   a race to handle).
4. **Ring `send()`/`receive()` with semaphore-backed blocking and
   mutex-protected cursors** (~100–150 lines) — the v1 baseline from
   "Cursor synchronization model" in Part I: `sem_wait` on the relevant
   semaphore blocks until a slot is free/available (no predicate to
   compute — the semaphore's count already is the predicate), then a brief
   `cursorMutex` lock claims and increments the one cursor this call owns,
   released before the slot copy, then `sem_post` on the other semaphore.
   No timeout, no `Stopped`/`PeerClosed` checks — those are v2. Returns
   `bool`, not a result enum (see "Fatal-error policy" in Part I).
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
- **No CRC32, no `MessageValidator`** — v1's only per-message check is the
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
  producer already created the segment and both named semaphores; "start
  the producer, then start the consumer" is a stated v1 precondition. See
  "Attach retries" in Part I — **[v2]**.
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
  plain integers, briefly locked behind `cursorMutex` only for the
  claim-and-increment, not for the whole call. See "Cursor synchronization
  model" and "v3: lock-free fast path" in Part I — **[v3]**.
- **No exhaustive signal/crash/restart/corruption/version-mismatch tests**
  — v1's test list (see "Testing strategy" in Part I) covers only what v1
  actually builds: the pthread mutex smoke test, an end-to-end test,
  wraparound, backpressure, and transport round-trip. Everything else in
  "Testing strategy" is tagged **[v2]**/**[v3]** and added when the feature
  it tests exists.

#### v2 build order

Layered on top of v1's semaphore-backed ring, without changing
`ITransport`'s public shape or the ring's core concept — v2 adds features
around/inside the same interface, not a rewrite of it:

1. **`sessionId` + fuller `Header`** — v2 shape (`sessionId`, `timestamp`,
   `sequenceNumber`, `payloadSize`, `checksum`), bump `layoutVersion`, add
   CRC32 (see "Checksum: algorithm and scope" and "Wire format" in Part I).
2. **`IPayloadGenerator` interface** — extract v1's inline payload
   construction, `RandomPayloadGenerator` as first implementation.
3. **Lifecycle state machine + ownership metadata** — `state`,
   `layoutVersion`, producer-PID fields; `Initializing→Ready→Stopping→Closed`
   transitions; stale-segment detection/recreation (see "Lifecycle and
   ownership" in Part I).
4. **Bounded wait + rich result enums** — `sem_timedwait`,
   `SendResult`/`ReceiveResult`, PID liveness/`PeerClosed`, `Malformed`,
   the `availableMessages`-post for `EndOfStream` (see "Waking a blocked
   `send()`/`receive()`" in Part I).
5. **`common::Controller` + control input** — `Running/Paused/Stopped`,
   stdin thread, signal handling via self-pipe or `sigwait()` (see "Control
   loop and signal safety" in Part I).
6. **`MessageValidator` + `StatsReporter`** — checksum recomputation,
   session-ID-aware sequence checking, `errorCount`, cumulative/interval
   counters on a monotonic-clock ticker.
7. **Runtime `--payload-size`/`--ring-capacity` flags** — replace v1's
   compile-time `Config` constants with validated CLI values.
8. **Consumer-starts-first retry/backoff** — indefinite-retry-with-logging
   loop (see "Attach retries" in Part I).

#### v3 build order

Purely internal — no producer- or consumer-visible API change from v2:

1. **Cross-process atomics for cursors** — `std::atomic<uint64_t>`,
   `alignas(64)` cache-line separation, `is_always_lock_free` requirement
   (see "Cross-process atomics" in Part I).
2. **Lock-free fast path** — remove `cursorMutex` from the cursor claim
   entirely; blocking is unaffected, already handled by the semaphores at
   every iteration.
3. **Release/acquire publication rules** — for both cursors and the
   already-atomic-since-v2 `state` field (see "Cursor synchronization
   model" and "Lifecycle state machine" in Part I).

No separate cross-process atomic visibility smoke test — see
"Cross-process atomic visibility" in "Testing strategy" above.

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
- **Zero-copy borrowed slots** — an RAII-scoped borrowed-view API is a
  valid future optimization once the copying version through v3 is proven
  correct (see "`send()`/`receive()` contract" in Part I). Not attempted
  at any currently-planned iteration.
- **Beyond v3:** consumer-liveness tracking so `send()` could gain its own
  `PeerClosed`, richer diagnostics distinguishing *why* a peer is gone, and
  caller-exposed retry/backoff policy. Note `SendResult`/`ReceiveResult`
  are already asymmetric within v2 itself — `SendResult` has no
  `PeerClosed` (see "Crash recovery scope" in Part I) — this item is about
  going further, not fixing that asymmetry.

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

`./producer-cli --payload-size 1024` runs, writes framed messages into
shared memory at a steady rate, responds to `p`/`r`/`q` and `SIGINT`,
exits cleanly with 0 (clean thread join, not a stuck stdin read). Blocks
(does not drop) when the consumer is paused and the ring is full.
Correctly recreates the segment/semaphores after a prior crash (stale, no
live PID) and refuses to start if a live producer already owns them.
Reaches `Closed` and unlinks on clean shutdown, posting `availableMessages`
on both `Stopping` and `Closed` transitions, so a consumer blocked in
`receive()` at that moment is guaranteed to wake promptly, not just
eventually via timeout (verified by blocking a consumer's `receive()` on
an empty ring, quitting the producer, asserting prompt `EndOfStream`). A
`send()` blocked on a full ring wakes within the bounded-wait window on
**local** shutdown only — per "Crash recovery scope" in Part I,
`SendResult` has no `PeerClosed`, so a `send()` blocked because the
consumer quit or crashed does **not** wake on its own. Generates a fresh
`sessionId` each run so a restarted producer against a newly-attached
consumer is distinguishable from data loss (verified in Part III's v2
definition of done; covers restart-then-reattach only, not a consumer that
stayed attached across the crash).

#### v3 Definition of done (producer)

Identical externally-observable behavior to v2's — no producer-visible API
or CLI change — but internally: `send()` no longer takes `cursorMutex` at
all (verified by profiling showing zero lock acquisitions on the fast
path); cursors confirmed lock-free via `is_always_lock_free` at compile
time on both macOS and Linux. No missed-notification scenario to induce or
recover from — the semaphores' no-lost-post guarantee holds independent of
whether the cursor claim is mutex-protected or lock-free.

### Part III: Consumer Implementation Plan

Scope: `consumer-cli` only. Part I above is the shared contract this plan
must honor — only what's specific to the consumer is covered here. See
Part II for the producer side.

#### Responsibilities

1. Receive messages from the producer via `ITransport`. **[v1]**
2. Validate each message: v1 checks sequence-number continuity inline (see
   "v1 error handling" below); v2 adds full checksum recomputation and
   `sessionId`-aware loss/reorder detection via `MessageValidator` (see
   "Defect handling" in Part I).
3. Print stats: v1 prints a final message count once, at exit (see "v1
   Definition of done" below); v2 adds a periodic once-per-second
   `StatsReporter` (total messages received, msgs/sec, bytes/sec).
4. Pause/resume/quit via signals or keypress — **[v2]**, v1 has no
   interactive control at all.

Pause semantics (producer blocks when consumer pauses), the ring layout, and
the lifecycle/attach protocol are all defined once in Part I — not restated
here.

#### v1 build order

Mirrors the producer's v1 steps — each sized to roughly 100–200 lines:

1. **Attach to the existing segment and semaphores** (~50–100 lines) — a
   single blocking `shm_open`/`mmap` attempt against the fixed
   `/ipc_ring_v1` name, followed by `sem_open` against the two semaphore
   names derived from it (see "Naming" in Part I). v1 assumes the producer
   already created all three — "start the producer, then start the
   consumer" is a stated v1 precondition (see "v1 Definition of done"
   below), so no retry/backoff loop is needed yet (that's v2 — see "Attach
   retries" in Part I). If `shm_open` or either `sem_open` fails, log a
   clear error and exit non-zero (see "v1 error handling" in Part II, same
   policy on the consumer side).
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
exits non-zero. No `MessageValidator`, no `errorCount`, no result enum.

#### v2 build order

Layered on top of v1's receive loop:

1. **`common::ITransport` reuse with bounded wait** — consumer opens the
   same ring (open-existing, not create). `receive()` blocks via bounded
   `sem_timedwait` (see "Waking a blocked `send()`/`receive()`" in Part I),
   returning `ReceiveResult` instead of v1's `bool`. Consumer never
   creates or unlinks the segment/semaphores — producer owns all three.
   Attach sequence (see "Lifecycle and ownership" in Part I): `ENOENT` or
   `state != Ready` → retry indefinitely on bounded backoff, logging
   periodically; `state == Ready` → check `layoutVersion`, mismatch is a
   hard error; `state == Stopping`/`Closed` mid-run → keep draining
   buffered messages, then `EndOfStream` once actually empty.
2. **`MessageValidator`** — given the `Message` `receive()` populated:
   recompute CRC32, compare to `header.checksum`; check `sessionId`
   against last-seen **first** (changed → new session, reset `lastSeen`,
   not a defect; unchanged → sequence check); compare
   `header.sequenceNumber` to `lastSeen + 1` (gap/reorder within a session
   is a defect); assert `header.payloadSize` matches actual received bytes
   (a mismatch here means a `SharedMemoryTransport` bug, not a malformed
   frame — `receive()` already bound-checked before copying). On any
   defect: increment a dedicated atomic `errorCount` (separate from
   `StatsReporter`), log specifics including `sessionId`, keep consuming
   (relaxing v1's fail-fast policy — see "Defect handling" in Part I).
3. **`StatsReporter`** — two separate atomic counter pairs, not one:
   - **Cumulative** (`totalMessages`, `totalBytes`): only ever incremented,
     read for the `total=` figure. Never reset.
   - **Interval** (`intervalMessages`, `intervalBytes`): incremented
     alongside the cumulative counters on each received message, and
     atomically read-and-reset once per second to compute `msgs/s`/`bytes/s`.

   **`bytes` means complete frame bytes — `sizeof(Header) + payloadSize`,
   not payload alone** — every message costs a full `Header` copy too (see
   "`send()`/`receive()` contract" in Part I), and payload-only would
   undercount, more so at small `payloadSize`. Increment by
   `sizeof(Header) + header.payloadSize` per `ReceiveResult::Received` only
   — `PeerClosed`/`Malformed`/`EndOfStream` moved no frame.

   **`msgs/s`/`bytes/s` measures end-to-end application throughput, not a
   transport-only benchmark.** It includes producer payload generation:
   the consumer can only receive as fast as the producer sends, so a slow
   `IPayloadGenerator::generate()` shows up directly as lower `msgs/s`,
   exactly as if the slowdown were inside the consumer — there's no
   read-ahead buffer decoupling the two beyond the ring's short-burst
   smoothing. This reflects the whole pipeline (producer generation + CRC,
   transport copy/wait-wake, consumer CRC + bookkeeping) because it's a
   measurement of observed arrival rate, not any one stage. A
   transport-only benchmark is a distinct, not-yet-built measurement (see
   "Add a benchmark suite" in the project's Roadmap, README.md) — it would
   need pre-generated payload buffers and timing narrowed to just
   `send()`/`receive()`.

   Reusing one counter for both `total` and the per-second rate doesn't
   work — resetting it would destroy the running total. Read the two
   interval counters as close together as possible (e.g. a single
   `exchange` under one lock) — `std::atomic<uint64_t>` makes each
   individual update safe regardless.

   Use a **monotonic clock**, not `sleep_for(1s)` in a loop (which drifts).
   Compute each target wake time from a fixed reference point:
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
   total=<n> msgs/s=<n> bytes/s=<n>
   ```
4. **`common::Controller` reuse** — same `Running/Paused/Stopped` class as
   producer. Consumer's "paused" means: stop calling `receive()`/stop
   advancing the read cursor, per "Pause semantics" in Part I.
5. **Control input** — same stdin thread (`p`/`r`/`q`) + signal-safety
   pattern as producer (self-pipe or `sigwait()`, interruptible stdin read
   on quit — see "Control loop and signal safety" in Part I), reused from
   `common/control/`.
6. **Consumer loop rework** — `receive()` into a reusable `Message` buffer →
   validate via `MessageValidator` → update stats counters → loop; check
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
(plain `sem_wait`, no timeout — see "Cursor synchronization model"
in Part I). Both processes exit normally after the final message: the
producer after sending message `N`, the consumer after receiving message
`N` and printing the final count. **An automated end-to-end test proves
this behavior** (see "Testing strategy" in Part I) — this is the actual
v1 completion criterion, not a manual demo.

#### v2 Definition of done (consumer)

`./consumer-cli` attaches to the producer's segment, prints one stats line
per second (`total`, `msgs/s`, `bytes/s`, cumulative/interval kept
separate, monotonic-clock ticker, no visible drift over a multi-minute
run). Any checksum failure, sequence gap, or reorder increments
`errorCount` and logs without crashing; a clean run has `errorCount == 0`.
Responds to `p`/`r`/`q` and `SIGINT`/`SIGTERM` with a clean exit (stdin
thread joins, signal never calls `Controller` from handler context).
Pausing it blocks the producer. Started before the producer, retries with
backoff; started against a mismatched `layoutVersion`, exits non-zero;
drains and exits cleanly on `EndOfStream` once the ring is empty and the
producer reached `Stopping`/`Closed` — a `receive()` blocked at that
moment wakes promptly on the producer's own post, unconditionally. A
`receive()` blocked on an empty ring wakes within the bounded-wait window
when the producer is killed, reaching its PID-liveness check
unconditionally — but whether it then returns `PeerClosed` remains
best-effort (a kill that orphans `cursorMutex` mid-increment leaves the
subsequent cursor claim blocked, no bounded escape through v2; see "Crash
recovery scope" in Part I). A **new** consumer started after the producer
restarts does **not** increment `errorCount` on its first message despite
`sequenceNumber` resetting, because `sessionId` changed.

#### v3 Definition of done (consumer)

Identical externally-observable behavior to v2's definition of done above.
Verified indirectly, as a black box, by v3's producer-side Definition of
Done (see Part II) — no consumer-specific behavior changes at v3.

## Editor

`.vscode/` configures the clangd language server (reading
`build/compile_commands.json`) with the Microsoft C/C++ IntelliSense engine
disabled. Install the recommended extensions when prompted. Keep `build/` fresh
(`cmake -S . -B build`) after adding files so clangd sees them.
