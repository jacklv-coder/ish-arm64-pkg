# Changelog

[简体中文](CHANGELOG.md)

Chinese is the primary changelog and this file is its maintained English mirror.

## v0.4.0-abi.6 (planned Stage1 maintenance prerelease)

This is a compatibility maintenance release after `v0.4.0-abi.5`. It remains a
prerelease and is **not stable v0.4.0**.

- The Swift wrapper now establishes one absolute deadline at
  `runOneshot`/`spawn` API entry and recomputes the remaining native
  milliseconds after argv/env/cwd/chroot marshalling. Option staging can no
  longer restart the full timeout and admit SPAWN after the product deadline.
  A remainder below 1 ms returns `ISH_ERR_TIMEOUT` instead of passing native
  `0`, which means unbounded.
- Finite streaming sessions now retain SPAWN's native absolute deadline. Stdin
  close reuses that deadline for control-queue admission and returns
  `ISH_ERR_TIMEOUT` when it cannot acquire the writer gate in time, instead of
  starting a fresh admission wait after SPAWN succeeds.
- The public C ABI remains 1, wire protocol remains v4, and no public symbol is
  added. RootFS remains outside the Release. This version does not implement a
  native Agent Loop or install/run Codex CLI in the iOS app. Node.js/npm remain
  independent optional guest packages.

## v0.4.0-abi.5 (published Stage1 maintenance prerelease)

This is a compatibility maintenance release after `v0.4.0-abi.4`. It remains a
prerelease and is **not stable v0.4.0**.

- A finite `timeout_ms` on streaming `ish_embed_spawn` now covers the SPAWN
  instance gate, staging gate, and control-queue admission from API entry.
  Failure to acquire any gate before the deadline returns `ISH_ERR_TIMEOUT`
  without a session.
- Finite-timeout sessions use ordered, bounded asynchronous admission for
  SPAWN, stdin close, and terminate, so a stalled control writer cannot consume
  the product command deadline. Stdin close returns `ISH_ERR_BUSY` instead of
  waiting behind an active stdin write. Callers must still read authoritative
  `EXITED` before confirming termination.
- Existing streaming calls with `timeout_ms == 0` retain synchronous-write
  semantics. The public C ABI remains 1, wire protocol remains v4, and no public
  symbol is added.
- Host lifecycle tests cover finite streaming instance/staging/queue-gate
  expiry, ordered `SPAWN → STDIN_CLOSE → TERMINATE → EXITED` under a stalled
  writer, and bounded stdin close behind an active write.

## v0.4.0-abi.4 (published Stage1 maintenance prerelease)

This is a compatibility maintenance release after `v0.4.0-abi.3`. It remains a
prerelease and is **not stable v0.4.0**.

- `third_party/ish` advances to `c36dfd2`. The embedded bootstrap thread and
  every guest task thread explicitly unblock iSH's internal SIGUSR1, preventing
  a host-app thread's inherited signal mask from leaving internal interrupts
  pending forever.
- Guest signals can now reliably interrupt blocking host syscalls such as
  `poll` and `nanosleep`, so command cancellation, timeout termination, and
  subsequent runtime recovery do not depend on supervisor polling or an
  app-level workaround.
- Linux x86, macOS arm64, iOS device/simulator, and real-RootFS paths cover the
  signal mask, cancellation, post-cancellation recovery, and shutdown.
- The public C ABI remains 1, wire protocol remains v4, and the Swift API is
  unchanged. RootFS remains outside the Release.
- This version does not implement a native Agent Loop and does not install or
  run Codex CLI inside the iOS app. Node.js/npm remain independent optional
  guest packages.

## v0.4.0-abi.3 (published Stage1 maintenance prerelease)

This is a compatibility maintenance release after `v0.4.0-abi.2`. It remains a
prerelease and is **not stable v0.4.0**.

- `third_party/ish` advances to `5f7535e`. Every fixed-width guest `uname`
  field is bounded to the 65-byte Linux ABI width and NUL-terminated, preventing
  longer host names from triggering `__strcpy_chk` SIGTRAP under the fortified
  libc used by Xcode 16 / iOS 18.
- Linux, macOS arm64, and iOS build paths add or reuse regression coverage for
  bounded truncation, buffer integrity, and a real guest `uname -a`.
- The public C ABI, wire protocol, and Swift API are unchanged. RootFS remains
  outside the Release.
- This version does not implement a native Agent Loop and does not install or
  run Codex CLI inside the iOS app. Node.js/npm remain independent optional
  guest packages.

## v0.4.0-abi.2 (published Stage1 maintenance prerelease)

This is a compatibility maintenance release after `v0.4.0-abi.1`. It remains a
prerelease and is **not stable v0.4.0**.

- `third_party/ish` advances to `71f940a`. Address-space teardown now serializes
  `/proc/<pid>/statm`, `maps`, and `mem` access through `general_lock`, avoiding
  Darwin SIGTRAP when a `pthread_rwlock` is destroyed with queued waiters.
- A task's `general_lock` is initialized before its pointer is published in the
  PID table, so procfs cannot observe a task whose lock initialization is
  incomplete.
- The public C ABI, wire protocol, and Swift API are unchanged. RootFS remains
  outside the Release.
- This version does not implement a native Agent Loop and does not install or
  run Codex CLI inside the iOS app. Node.js/npm remain independent optional
  guest packages.

## v0.4.0-abi.1 (published ABI-transition prerelease)

This is Stage1 of the native-first / Swift-second sequence. It is **not stable
v0.4.0**.

### Publication transaction

- `v0.4.0-abi.1` was produced by the release transaction with its XCFramework,
  Corresponding Source, and manifest-only release commit. `Package.swift`
  currently pins that public asset.
- RootFS content is neither committed, packaged, nor uploaded by the release
  script. Prebuilt XCFramework and guest binaries are produced only by the
  release transaction and are not committed directly to source branches.

### Delivered in Stage1: native runtime

- `third_party/ish` uses an absolute SSH-over-443 URL so SwiftPM and clean checkouts
  cannot resolve a relative submodule address into a local package repository
  cache, while retaining the repository's SSH fetch policy. GitHub-hosted CI,
  which has no SSH private key, applies a read-only public HTTPS rewrite only
  to its explicit submodule checkout command.
- The public C ABI remains `ISH_EMBED_ABI_VERSION == 1`. Additive compatible
  symbols/statuses cover session retain/release, `ISH_ERR_BUSY`, output-backlog
  protection, and supervisor installation failures.
- The kernel thread is joinable; shutdown cooperatively soft-halts and joins it.
  One valid boot/shutdown lifecycle remains supported per host process.
- Separate control-writer, event-reader, guest-log-drain, and log-sink-writer
  paths impose fixed frame, output-backlog, stdin-queue, and log-queue bounds.
  Complete host→guest frames count against a 4 MiB/256-frame total budget from
  admission through actual free, with 4 KiB/16 frames reserved for critical
  lifecycle messages and `ISH_ERR_CONTROL_LIMIT` for ordinary-call overflow.
  Critical close/shutdown/oneshot termination uses bounded asynchronous
  admission and falls back to control EOF when cleanup cannot be confirmed.
  Spawn payload construction is serialized to avoid concurrent large staging
  allocations outside that budget.
- The internal host ↔ guest wire protocol is exact-match v4 and adds
  `SESSION_CLOSE`. Wire v4 and public C ABI 1 are separate version surfaces.
- Before default boot installs the embedded supervisor, it hashes the actual
  blob bytes and requires SHA-256 to match both build metadata and the
  `/sbin/.ishsv-ishembed-sha256-<digest>` content-addressed path. A mismatch
  returns `ISH_ERR_SUPERVISOR_INSTALL` before installation. An explicit
  `supervisorGuestPath` bypasses the default blob installation and digest gate
  and is caller-owned. This proves same-build integrity; it is not a signature
  or provenance/publisher authentication.
- iSH JIT dirty-page draining is serialized with compile/insert, host/kernel
  writes use pre/post invalidation, and ARM64 `IC IVAU` publishes its target page
  before leaving a direct chain. Single-page writes and explicit
  `asbestos_invalidate_page` now filter by exact page; a cross-page block's second
  page uses `end_addr`, and only the multi-page hashed bitmap can conservatively
  over-invalidate on collision. New regressions cover a single-page collision and
  cross-page `end_addr`. Direct chains and the RET cache return to the dispatcher
  only when the next target block intersects pending dirty code pages. Data-only
  writes may keep chaining while retaining pending dirty state and its coherence
  checks.
- Closing a live TTY cleans up the tracked shell, foreground job, and transports.
  A readiness pipe first validates an immutable PID/PGID; after leader exit,
  `waitid(..., WNOWAIT)` preserves that identity while same-group background
  processes are cleaned, then `waitpid` reaps and clears PID/PGID. PID 1 also
  removes untracked descendants adopted by the subreaper after double
  fork/`setsid`, using exact PIDs and requiring two consecutive clean scans
  before success. A scan/kill/reap/deadline failure fail-closes the guest
  instance without a false-success `EXITED` or `SHUTDOWN_ACK`.
- Host integration tests pin the official URL and SHA-256 of the Alpine
  development RootFS input. The schema-v2 recipe covers the builder,
  supervisor/protocol, iSH worktree/submodules, fakefsify origin, and Alpine pin;
  the artifact seal binds actual critical binaries and initial meta/data. Under
  a stable `flock`, the builder uses unique same-volume staging; `fs`, tar,
  checksums, and receipt form a transaction, replacement uses exchange, the
  receipt commits last, failure rolls back in reverse, and success discards the
  old generation. The runner holds locks from validation through use and checks
  SQLite row types/16-byte stats/single root, complete meta/data paths, and
  critical digests. Derived Codex identity binds clean receipt/content, package,
  exact/tag request kind, VM/bin, provision inputs, and actual version. Exact
  versions must match byte for byte, tags bind their resolution, and the package
  bin, guest executable, and global-entry mapping are part of reuse admission.
  This does not authorize
  committing or publishing RootFS content or replace signature, provenance,
  final-content digest, and license review.
  The receipt is a lineage/initial-snapshot record binding the static marker and
  initial `fs.tar.gz`/`SHA256SUMS`. After runtime mutation, `--verify-bundle`
  separately validates current-tree structure/critical identity and initial
  archive integrity; it does not prove current `fs` bytes equal the tar. The
  initial tar is neither a current-tree backup nor a Release asset.
- `ish_embed_boot(..., NULL)` rejects the call before installation or boot side
  effects.
- Chrooted spawn revalidates required device, devpts/procfs, and runtime-directory
  state.
- Production and host-test iSH builds explicitly define
  `ISH_DISABLE_SKIP_BRK=1`, disabling the fork's experimental arm64 BRK recovery
  so fatal guest SIGTRAP is not swallowed; old Meson caches without it are rejected.
- `third_party/ish` pins the project's maintained `ish-arm64` fork: join/soft-halt,
  embedded lifecycle, and JIT coherence require iSH-core changes that the outer
  package cannot provide alone. Independent PRs/CI and an exact gitlink audit
  these narrow differences while generic fixes can still go upstream; the
  project does not directly rewrite somebody else's maintained upstream worktree.
- A missing chroot Codex configuration is created atomically; an existing
  regular file preserves custom contents while tightening its mode to `0600`,
  and symlinks or other non-regular objects are rejected safely.

### Stage1 Swift compatibility boundary

- Swift source remains v0.3.3-ABI compatible and does not call the new C
  retain/release symbols.
- `IshInstanceCallGate` serializes boot/shutdown and holds an instance lease for
  every oneshot and live session. Any lease returns `ISH_ERR_BUSY` before
  entering v0.3.3 native shutdown. Spawn transfers its lease into the session
  until native close completes. `BUSY` restores the running handle; any other
  shutdown failure quarantines ordinary calls while preserving shutdown-only
  cleanup retry. Successful shutdown enters a consumed terminal state, so
  another boot never reaches native code.
- Swift oneshot/read reject NaN or infinite timeouts before native entry, and a
  positive sub-millisecond oneshot timeout rounds up to 1 ms rather than
  becoming an accidental no-timeout request.
- `IshSessionCallGate` uses a Swift-side `NSCondition` and `activeCalls` count to
  keep `close()` from freeing a handle used by an already-entered C call. Close
  detaches first, rejects new calls, and then waits for admitted calls;
  `read(timeout: nil)` polls internally every 100 ms so a blocked read can leave.
- `IshTerminal.setEventHandler` keeps its original non-`@Sendable` public
  signature. A private `@unchecked Sendable` box contains the queue crossing
  under strict concurrency without breaking caller source.
- Public errors remain `IshError.raw(Int32, String)`; the current
  `IshInstance`, `IshSession`, `IshVM`, and Terminal/VT interface shapes remain.
- Close waits for every admitted C call. If a non-read call never returns with
  v0.3.3 or a custom supervisor, close also keeps waiting, so callers should
  still stop and await their tasks first. Complete native borrowing/cancellation,
  typed statuses, bounded Terminal callback delivery, and VT parser hardening
  remain Stage2.

## v0.3.3

Before the first transition Release, the Stage1 manifest pointed to this binary.
Refer to that tag for historical details.
