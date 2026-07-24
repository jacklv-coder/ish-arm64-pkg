# IshEmbed

[简体中文](README.md) | English

IshEmbed is an embeddable Linux runtime Swift Package for iOS host apps. It is
built on the [iSH](https://github.com/ish-app/ish) userspace emulator, Linux
syscall layer, and fakefs. A SwiftUI or UIKit app can boot a Linux userspace,
run one-shot commands, manage streaming sessions, and keep multiple persistent
chroot trees in one fakefs.

This repository is the low-level runtime, not the end-user product. PocketRoot
adds verified RootFS installation, product command policy, Swift Concurrency
isolation, and UI. The minimum supported system is iOS 18, and release device
and simulator slices are arm64.

## Current phase: native ABI transition

The default branch has published `v0.4.0-abi.3` and is preparing the compatible
maintenance prerelease `v0.4.0-abi.4`. Both belong to the Stage1 **native ABI
transition**. Neither is stable `v0.4.0` or the complete v0.4 Swift API. Keep
these four version surfaces distinct:

| Surface | Current `v0.4.0-abi.3` | Planned `v0.4.0-abi.4` |
| --- | --- | --- |
| Public C ABI | `ISH_EMBED_ABI_VERSION == 1`; compatible symbols are published | Still ABI 1, with no new public symbols |
| Internal wire protocol | exact-match v4 between host and embedded supervisor | still v4; this is not the public C ABI version |
| `Package.swift` | pins the public `v0.4.0-abi.3` URL/checksum | the release transaction creates a manifest-only release commit pinned to the maintenance binary |
| Swift source | remains v0.3.3-ABI compatible and does not call retain/release | keeps the same Swift API and old-ABI usage |

Stage1 native code adds session retain/release, a joinable kernel thread,
soft-halt, exact wire v4, and complete session close. The existing Swift wrapper
deliberately does not call the new retain/release entry points. A
v0.3.3-compatible Swift call gate protects both instance and session handles. A
oneshot holds an instance lease for the native call; spawn transfers that same
lease into the session until native close completes. Shutdown returns busy for
an active call/session without entering the old binary's free path. `BUSY`
restores the running handle; other failures preserve it only for shutdown
cleanup retry. Complete native
borrowing/cancellation, typed statuses, Terminal callback queues, and VT parser
changes belong to **Stage2** and are not delivered here.

RootFS content is neither committed to this repository nor included in the
XCFramework or GitHub Release. Apps must independently pin its provenance,
size, SHA-256, licenses, and installation transaction.

## Capabilities and boundaries

- One valid `IshInstance` lifecycle is supported per host process, with multiple
  concurrent command sessions.
- One-shot and streaming commands support standard I/O, stdin close, signals,
  PTY resize, and exit events.
- Persistent trees below `/srv/vms/<name>` can be used as chroots. This isolates
  filesystem views; it is not hardware virtualization or an adversarial sandbox.
- The native reader has hard frame and per-session backlog ceilings. The
  host→guest control queue also has a 4 MiB/256-frame total budget, with
  4 KiB/16 frames reserved for close, shutdown, and other critical lifecycle
  messages. Ordinary calls return `ISH_ERR_CONTROL_LIMIT` when the next frame
  cannot be admitted. Product code must still enforce tighter output, timeout,
  and cancellation policies.
- A release XCFramework embeds a static AArch64 guest supervisor. Default boot
  first hashes the **actual embedded bytes** and installs them into fakefs only
  when that SHA-256 matches same-build metadata and the content-addressed guest
  path; a mismatch returns `ISH_ERR_SUPERVISOR_INSTALL`. An explicit
  `supervisorGuestPath` bypasses the default blob installation and this digest
  gate, making the caller responsible for provenance, integrity, and protocol
  compatibility. This digest comparison is not a digital signature and does not
  authenticate download provenance or publisher identity.
- The pinned iSH fork gives correctness priority on JIT writes. Single-page
  writes and explicit `asbestos_invalidate_page` filter by the exact page, with
  a cross-page block's second page checked through `end_addr`; only the hashed
  bitmap for a multi-page dirty set can conservatively over-invalidate on a
  collision. Direct chains and the RET cache return to the dispatcher when the
  next target block intersects pending dirty code pages. Data-only writes may
  continue chaining while retaining pending dirty state, avoiding an
  unconditional chain break while preserving a real coherence-checking cost.
- Guest PID 1 cleans more than the tracked process group. Untracked descendants
  adopted by the subreaper after a double fork/`setsid` must be removed by exact
  PID and followed by two consecutive clean scans before successful exit is
  published. If cleanup cannot be completed or proven, the whole guest instance
  fail-closes without a false-success `EXITED` or shutdown acknowledgement.

## Why this project maintains an iSH fork

`third_party/ish` pins the project's maintained `ish-arm64` fork. Join/soft-halt,
embedded lifecycle, and JIT dirty-page coherence require changes in the emulator
core and cannot be implemented only in the outer package or Swift layer. The fork
gives those narrow differences independent PRs, CI, and an exact gitlink, making
PocketRoot builds and releases reproducible. We do not directly rewrite somebody
else's local upstream repository; generally useful fixes can still be contributed
to [iSH upstream](https://github.com/ish-app/ish), while the fork carries project
gates until upstream accepts and releases them. The current `v0.4.0-abi.4`
source change includes neither RootFS content nor any prebuilt XCFramework/guest
binary; binaries may be produced and published only by a later release
transaction after its gates pass.

## Installation status

`v0.4.0-abi.3` is public and [`Package.swift`](Package.swift) currently pins it.
Until `v0.4.0-abi.4` is published, the manifest keeps pointing at that verified
asset instead of advertising a future 404 URL. Use Xcode's
**File → Add Package Dependencies…** with:

```text
https://github.com/jacklv-coder/ish-arm64-pkg
```

Select a version whose tag, `libIshKernel.xcframework.zip`, Corresponding Source,
and manifest URL/checksum all match. Consumer projects do not need Meson, Zig,
or LLVM.

`v0.4.0-abi.4` only fixes inherited masking of iSH's internal SIGUSR1 on the
embedded bootstrap and guest task threads, allowing guest signals to reliably
interrupt blocking host syscalls. It does not implement a native Agent Loop or
install Codex CLI in the app.
Node.js/npm remain optional choices of the RootFS/guest package-management flow,
not runtime requirements.

## Swift usage

Stage1 keeps the v0.3.3-compatible Swift API. First install a verified fakefs
RootFS in a writable directory whose top level contains `data/` and `meta.db`.

```swift
import Foundation
import IshEmbed

let instance = IshInstance.shared
let rootfs = try installedRootFSURL() // host-validated, atomically installed
try instance.boot(.init(rootfsPath: rootfs.path))

let result = try instance.runOneshot(
    .init(argv: ["/bin/echo", "hello"], timeout: 10)
)
print(String(decoding: result.stdoutData, as: UTF8.self))
```

`IshSpawnOptions.timeout` is honored only by `runOneshot`. A finite timeout
starts at API entry and includes the SPAWN staging gate and control-queue
admission. If the runtime cannot confirm cleanup, it enters shutting-down state
instead of leaving an unowned guest process. NaN and either infinity return
`ISH_ERR_INVALID_ARG (-13)` before native entry. A positive sub-millisecond
value rounds up to 1 ms instead of degrading to “no timeout.”

Use the `ensureDefaultVM()` and `spawn(in:)` helpers only when the verified
RootFS manifest explicitly includes `/srv/vms/.template`. Continuously drain a
streaming session and always close it:

```swift
let vm = try instance.ensureDefaultVM()
let session = try instance.spawn(
    in: vm,
    .init(argv: ["/bin/sh"], allocateTTY: false)
)
defer { session.close() }

try session.write(Data("printf 'ready\\n'\n".utf8))
try session.closeStdin()

readLoop: while true {
    switch try session.read(timeout: nil) {
    case .data(let data, _, _):
        FileHandle.standardOutput.write(data)
    case .exited(let code, let signal):
        print("exit=\(code), signal=\(signal)")
        break readLoop
    }
}
```

When a finite `read(timeout:)` expires it throws `IshError.raw` with
`ISH_ERR_TIMEOUT (-12)`; it does not return an empty event. A non-finite timeout
returns `ISH_ERR_INVALID_ARG (-13)`; only `nil` means an infinite wait.

For cancellation, close stdin first and then call `terminate()` if needed. Use
`interrupt()` for TTY Ctrl+C. Stop and await a session's tasks before `close()`;
the session gate waits for admitted C calls but cannot cancel a permanently
blocked non-read call. The instance gate keeps a lease for each oneshot and live
session, so `shutdown()` immediately throws `ISH_ERR_BUSY` instead of invoking
v0.3.3 native free. Stop and await spawn/runOneshot, close all sessions, and then
call `try instance.shutdown()`; `BUSY` can be retried after task cleanup, while
other failures quarantine ordinary calls but retain shutdown cleanup retry.
Stage1 still exposes
`IshError.raw(Int32, String)`; typed status mapping is deferred to Stage2.

## C ABI and wire protocol

[`include/ishembed.h`](include/ishembed.h) is authoritative for the public C
ABI. Stage1 adds `ish_embed_session_retain` and
`ish_embed_session_release`, but the ABI version constant remains 1 and the
current Swift source does not use those symbols.

[`protocol/proto.h`](protocol/proto.h) is authoritative for the internal framed
transport between the host and guest PID 1. Wire v4 requires an exact peer
match and adds `SESSION_CLOSE`. The wire version is not the public C ABI version.

## Documentation

- [Documentation map](docs/README.en.md)
- [Architecture and lifecycle](docs/architecture.en.md)
- [Testing and acceptance](docs/testing.en.md)
- [Troubleshooting](docs/troubleshooting.en.md)
- [Release transaction](docs/releasing.en.md)
- [Changelog](CHANGELOG.en.md)

Chinese is the primary documentation language and each technical document has
an English mirror.

## Maintainer build and verification

```sh
brew install meson ninja sqlite libarchive llvm lld zig
git submodule sync -- third_party/ish
git submodule update --init --checkout -- third_party/ish
PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH" \
  scripts/build-ios.sh
scripts/build-rootfs.sh --print-inputs
scripts/build-rootfs.sh --print-identity
scripts/build-rootfs.sh --verify-bundle build
scripts/run-host-tests.sh
scripts/verify-ios-artifact.sh
scripts/test-swift-ios.sh --local-binary
scripts/check-docs.sh
```

Nested iSH submodules are not prerequisites for this package build. The
commands above first synchronize an existing checkout's cached URL with
`.gitmodules`, then initialize only this repository's pinned `third_party/ish`
checkout without implicitly fetching unrelated repositories that are not build
inputs.

On the first host-test run in a clean checkout, the reviewed Alpine version,
architecture, and SHA-256 in `scripts/alpine-rootfs-pin.sh`, together with the
builder's fixed official download location, prepare a development RootFS
automatically. `--print-inputs` audits those inputs before download. Testing
another Alpine version requires an explicitly reviewed `ALPINE_SHA256` at the
same time. The builder holds a kernel `flock` on a stable regular lock file and
stages `fs`, `fs.tar.gz`, `SHA256SUMS`, and `ROOTFS_RECEIPT` on the same volume.
First publication uses no-replace rename; replacement uses atomic exchange so
`build/fs` stays continuously visible. The receipt publishes last as the
four-artifact commit point. Catchable signals or intermediate failure roll the
operations back in reverse; success discards the old generation with staging
instead of retaining `fs.previous.*`. `--print-identity` emits
the schema-v2 recipe prefix covering the builder, supervisor, protocol headers,
iSH revision/worktree/submodules, fakefsify origin, and Alpine pin. The marker
also binds the actual fakefsify, AArch64 supervisor, BusyBox, and initial
meta/data seal. The runner holds the RootFS lock from validation through the
final consumer and checks the receipt, recipe, SQLite row types/16-byte
stats/root, complete meta/data paths, and critical digests while permitting
valid runtime mutations. The repository no longer provisions or tests Codex
CLI. Callers may still explicitly install Node.js/npm as ordinary guest
packages.
`ROOTFS_RECEIPT` is a lineage/initial-snapshot record: it binds the static
identity marker and the initial `fs.tar.gz`/`SHA256SUMS` produced at build time.
After runtime writes mutate `fs`, `--verify-bundle` validates the current tree's
structure, database/critical-binary identity, and the integrity of the initial
archive, but does not prove byte equality between current `fs` and the tar. That
tar is neither a backup of the current tree nor a committable or releasable asset.
`scripts/build-rootfs.sh` is not a package or Release asset, and a pinned digest
does not authorize distribution.
See the [testing guide](docs/testing.en.md) for the complete matrix and both
real-link boundaries.

## Source layout

```text
ish-arm64-pkg/
├── Package.swift                    SwiftPM manifest and binary pin
├── Sources/CIshEmbed/               C module map
├── Sources/IshEmbed/                v0.3.3-compatible Swift wrapper
├── include/ishembed.h               public C ABI
├── protocol/proto.h                 internal host ↔ supervisor wire protocol
├── host/                            lifecycle, sessions, and I/O pumps
├── ffi/                             narrow bridge into iSH internals
├── supervisor/                      static AArch64 guest PID 1
├── c-tests/                         native, protocol, and lifecycle tests
├── scripts/                         build, test, compliance, and release tools
└── third_party/ish/                 pinned iSH submodule revision
```

## License

This repository and its iSH-derived code are GPL-3.0-or-later. Every release
XCFramework must have matching Corresponding Source. See [LICENSE](LICENSE),
[NOTICE.md](NOTICE.md), and the [release guide](docs/releasing.en.md). Review
RootFS provenance and licensing independently.
