# IshEmbed testing and acceptance

[简体中文](testing.md) | English

Stage1 acceptance is not “one test passed.” It must simultaneously establish a
coherent native ABI transition, old-Swift compatibility, iOS 18 binary, internal
wire v4, and release supply chain.

## Test matrix

| Layer | Primary gate | What it proves | What it does not prove |
| --- | --- | --- | --- |
| Protocol/digest unit | `proto_test`, `supervisor_stdin_test`, `sha256_test` | v4 framing, boundaries, stdin partial writes/backpressure, SHA-256 known answers and malformed-metadata rejection | a real iSH boot |
| Lifecycle | `lifecycle_test` | retain/release, close/read/write/signal interleavings, shutdown/busy, PID identity, bad embedded digest/path rejection before install | Stage2 Swift borrowing is complete |
| JIT dirty pages | `dirty_page_test`, `dirty-page-trace` | READ does not mark, fast/miss/cross-page writes merge, exact single-page collision filtering, explicit cross-page `end_addr` invalidation, conservative multi-page buckets, host/kernel post-write invalidation, exact tracing, cross-TLB `IC IVAU`, and ARM64/x86 chain boundaries | immediate visibility for nonconforming self-modification without cache maintenance; global cross-thread x86 publication; old write-path throughput |
| Native integration | `internal-signal-mask`, `procfs_test`, `ishembed_smoke` | internal SIGUSR1 masks on embedded/guest task threads, fakefs, spawn, procfs, a real guest `uname -a`, and the general command path | RootFS provenance/license is trustworthy; compatibility with a particular user tool |
| Sanitizers | ASan/UBSan and TSan where applicable | bounds, UAF, undefined behavior, and races on covered paths | every schedule is defect-free |
| RootFS-free Swift | instance/session gates, shutdown retry, public API smoke | oneshot/session leases prevent old-ABI UAF, failure keeps the handle, old public signatures compile | every C call is cancellable or close is always bounded |
| Swift manifest real link | `test-swift-ios.sh --manifest-binary` | Stage1 Swift links the current `v0.4.0-abi.6` binary | `v0.4.0-abi.7` fixes are public |
| Swift local real link | `test-swift-ios.sh --local-binary` | the same Swift links the maintenance XCFramework | GitHub assets are published |
| XCFramework | `build-ios.sh` plus symbol/final-link checks | device/simulator arm64, minimum iOS 18, required symbols | product app behavior |
| Docs/scripts | positive/negative docs gates, shell syntax, policy tests | bilingual links, diagnostics, release-notes/version/tag/source policy | documentation equals implementation |

## Confirm the Stage1 state first

Before `v0.4.0-abi.7` publication, all of these should be true:

- `ISH_EMBED_ABI_VERSION` is 1;
- `ISH_PROTO_VERSION` is 4;
- Swift source does not call `ish_embed_session_retain/release`;
- `Package.swift` still pins the public `v0.4.0-abi.6`;
- the locally built XCFramework exports retain/release and required join/soft-halt symbols;
- RootFS content is absent from Git diff, XCFramework, source archive, and Release manifest.

Only after publication should “manifest pins `v0.4.0-abi.7`” become the expected
state. Do not apply that expectation to a correct pre-publication tree that
still references the verified `v0.4.0-abi.6`.

## Fast metadata and script gates

```sh
git diff --check
bash -n scripts/*.sh
scripts/check-docs.sh
scripts/test-check-docs.sh
scripts/test-build-ios-path-safety.sh
scripts/test-run-host-tests.sh
scripts/test-release-version-policy.sh
scripts/test-release-tag-cas.sh
scripts/test-source-policy.sh
swift package dump-package >/dev/null
```

- `check-docs.sh` validates every Chinese/English pair, language switch, and
  local link/anchor. `test-check-docs.sh` proves failure with deliberately
  broken fixtures.
- Release version policy tests validate prerelease classification and bilingual
  `v*-abi.*` transition notes without network access.
- Tag CAS tests prove that deletion/publication requires the transaction's exact
  raw object OID and commit.
- Source policy fixtures reject RootFS, nested archives, escaping paths, and
  noncompliant assets.
- CI checks a pull request from its base SHA through HEAD and a push from the
  event's `before` SHA through HEAD. A zero or locally unavailable `before`
  falls back to the empty tree, so a multi-commit push cannot validate only its
  final commit.

## Native unit and lifecycle tests

```sh
meson setup build-test-ish "$PWD/third_party/ish" \
  -Dguest_arch=arm64 \
  -Dc_args=-DISH_DISABLE_SKIP_BRK=1 \
  -Db_lundef=false
ninja -C build-test-ish libish.a libish_emu.a libfakefs.a
meson setup build-test \
  -Dish_src="$PWD/third_party/ish" \
  -Dish_build="$PWD/build-test-ish" \
  -Dguest_arch=arm64 \
  -Db_lundef=false \
  -Dwerror=true
meson compile -C build-test
meson test -C build-test --print-errorlogs
```

Important coverage includes:

- v4 header/payload ceilings, unknown types, truncation, and exact-version rejection;
- owner reference, retain/release pairing, and rejection of new calls after close;
- deterministic overlap of an already-borrowed read/write/signal with close;
- immutable leader/PGID validation through the spawn readiness pipe,
  `waitid(..., WNOWAIT)` pre-reap cleanup of a same-group background child, and
  PID/PGID clearing after `waitpid`, preventing
  both process escape and reused-identity signalling;
- Linux real-process double-fork + `setsid` regressions proving that an
  untracked subreaper-adopted descendant is removed after both normal leader
  exit and explicit close, another tracked session is not misclassified, and a
  `/proc` scan/cleanup failure fail-closes without successful `EXITED` or
  shutdown acknowledgement;
- TTY foreground-group close cleanup and slot release;
- side-effect-free rejection of `ish_embed_boot(..., NULL)`;
- before default supervisor installation, SHA-256 over the actual embedded bytes
  must bind both 64-character lowercase metadata and the
  `/sbin/.ishsv-ishembed-sha256-<digest>` path; a bad digest or path returns
  `ISH_ERR_SUPERVISOR_INSTALL` before calling the installation FFI. An explicit
  custom path does not take the default blob-install/digest path. These tests
  prove same-build consistency, not a digital signature or provenance
  authentication;
- busy shutdown with live sessions/active calls and normal soft-halt/join;
- ordinary/critical control-queue frame/byte ceilings, bounded EOF fallback for
  close and finite oneshot under saturation, finite streaming instance/staging/
  queue-gate deadlines, deterministic reuse of the original SPAWN deadline by
  stdin close behind a stalled writer, bounded stdin-close/terminate, busy
  stdin-close behind an active write, exact stop/finish release, and the spawn
  staging gate with a blocked reader; smaller test budgets make overflow and
  reuse deterministic;
- stdin partial writes, `EAGAIN`, queue ceilings, and error propagation;
- no dirty marking on TLB READ and retention of every page across C fast,
  write-miss, and cross-page stores, including a final page deferred until
  drain and preservation of the prior page on transition; a single-page
  same-bucket collision regression proves that writing an empty colliding page
  neither removes the remote block nor advances its generation, while writing
  the real code page invalidates once. An explicit `asbestos_invalidate_page`
  regression proves that a cross-page block's second page matches through
  `end_addr` while a remote same-bucket block survives. After a page transition,
  the multi-page path uses the hashed bitmap and may conservatively batch two
  distinct buckets plus same-bucket collisions, with one-shot generation
  advancement after consumption; a deterministic threaded regression makes drain wait for a
  compile/insert lock holder and invalidate its stale publication;
  `mem_did_write` regressions cover publication in the pre/write interval,
  cross-page writes, wrapping-address full-invalidation fallback, and the
  data-only empty-occupancy path; the x86 exact-trace regression also proves the
  diagnostic page set survives runtime drain, distinguishes addresses 1024
  pages apart in the same runtime bucket, remains intact across repeated
  iteration, and clears only explicitly after success; emulated CTR_EL0 has
  DIC/IDC clear, and a real ARM64 gadget test uses separate writer/executor TLBs
  to prove `IC IVAU, Xt`
  publishes Xt's page before ending the block; a real x86 gadget test establishes
  `writer -> source -> target` direct chains and proves a write cannot execute
  the stale target, while the 16-prefix page-tail regression emits `#UD` before
  a 16th-byte read can touch the unmapped following page; an ARM64 high-address
  adjacent-page TLB-alias regression proves the last aligned A64 instruction at
  a page tail runs normally, while a misaligned PC traps without prefetching and
  evicting the starting page;
- chroot preparation repairs only general `/dev`, devpts, procfs, and `/root`
  state and creates no Codex CLI configuration or other tool-specific state.

## JIT performance baseline

On an Apple M3 Pro with release/O3/NDEBUG and an Alpine 3.19.1 ARM64 seed, using
`d9629015` as baseline, each oneshot ran 20,000 shell arithmetic/variable-write
iterations with reversed baseline/current interleaving:

- single-task median `2.6798s -> 2.8044s`, a `4.65%` increase;
- four simultaneously released tasks median `2.9490s -> 3.0594s`, a `3.74%`
  increase;
- all 20 `ishembed_smoke` runs across both variants passed; fixed-wait noise
  makes that test unsuitable for claiming a throughput improvement.

A later target-guard sample on the same machine measured the shell loop at about
`2.77s -> 3.44s` (about `24%`) and legacy SSE2 e2e at about `5.67s -> 7.08s`
(about `25%`). These are short local samples, not a stable benchmark, an
end-to-end PocketRoot estimate, or a release SLA. The final implementation does
not send every ordinary write back to the dispatcher: it exits and drains only
when the next direct-chain or RET target intersects pending dirty code pages;
data-only writes may keep chaining. Each chained transfer still pays a coherence
check. Future changes should reuse the same seed, release configuration, and
interleaving, add repeated statistics, and avoid mistaking system load for a
code change.

## Sanitizers

```sh
meson setup build-asan-ish "$PWD/third_party/ish" \
  -Dguest_arch=arm64 \
  -Dc_args=-DISH_DISABLE_SKIP_BRK=1 \
  -Db_sanitize=address,undefined \
  -Db_lundef=false
ninja -C build-asan-ish libish.a libish_emu.a libfakefs.a
meson setup build-asan \
  -Dish_src="$PWD/third_party/ish" \
  -Dish_build="$PWD/build-asan-ish" \
  -Dguest_arch=arm64 \
  -Db_sanitize=address,undefined \
  -Db_lundef=false \
  -Dwerror=true
meson compile -C build-asan
meson test -C build-asan --print-errorlogs --repeat 5
```

The outer build generates `dirty_page_test` when it receives a usable
`-Dish_build`; Meson does not itself prove that directory uses the same
sanitizer as the outer build. The canonical commands above and CI explicitly
apply matching configurations to iSH and the outer build. CI also runs the same
suite with `-Db_sanitize=thread` in separate `build-ci-ish-tsan` and
`build-ci-tsan` directories. Do not switch sanitizer configurations inside one
build directory or mix iSH and outer builds from different sanitizers; cached
Meson state and mixed runtimes make results ambiguous. Preserve the complete
test log and first sanitizer stack on failure.

## Host/RootFS integration

```sh
# Print the version, official URL, pinned SHA-256, and expected recipe identity without building:
scripts/build-rootfs.sh --print-inputs
scripts/build-rootfs.sh --print-identity
scripts/build-rootfs.sh --verify-rootfs build/fs
scripts/build-rootfs.sh --verify-bundle build
scripts/test-deterministic-rootfs-tar.sh
scripts/prepare-rootfs-candidate.sh --verify-only
scripts/run-host-tests.sh --smoke
```

The runner explicitly passes `scripts/alpine-rootfs-pin.sh` to the builder and
requires build-check, build-host, and the RootFS recipe to use the `arm64` guest.
That manifest pins the reviewed Alpine version, architecture, and SHA-256, so a
clean checkout works while the download is still verified before extraction.

Under a kernel `flock` on a stable regular file, the builder creates a schema-v4
marker in unique same-volume staging. The deterministic archiver normalizes
both tar/gzip layers' owner, order, and timestamps to the fixed
`SOURCE_DATE_EPOCH`. The recipe covers the listed reviewed source and pin
inputs. Recursive submodule data retains only canonical status/object/path
records, excluding local ref descriptions that change after fetch; it does not
cover uncommitted nested-worktree content. Artifact fields separately bind the
supervisor, BusyBox, and initial meta/data seal. The actual fakefsify binary,
host/tool evidence, and linked-library load paths, versions, and available
file digests are recorded in the
candidate's external build-environment receipt. Publication requires the full
seal. Reuse checks the four-artifact receipt, SQLite quick-check, positive
inodes, 16-byte stat BLOBs, exactly one empty root, complete meta/data paths,
and AArch64 supervisor/BusyBox digests. `fs`, tar, and checksums publish before
the receipt commits the generation; replacement uses exchange to keep fs
visible, failure rolls back in reverse, and success retains no prior generation.
The runner holds the RootFS lock from validation through the last consumer, so
a concurrent builder cannot replace the tree during use. Valid runtime changes
remain reusable and copying/editing the marker is insufficient.

The receipt represents lineage/an initial snapshot only: it binds the static
marker and initial `fs.tar.gz`/`SHA256SUMS`. After runtime mutation,
`--verify-bundle` passing means that current-`fs` structure,
database/critical identity, and the initial archive are valid; it does not mean
that current `fs` equals the tar byte for byte. Tests and diagnosis must not
treat the initial tar as a current-tree backup, and it never enters commits,
packages, source archives, or Releases.

Testing another version requires setting `ALPINE_VERSION` and an independently
reviewed `ALPINE_SHA256` together; a missing, malformed, or mismatched digest
fails. `scripts/test-run-host-tests.sh` uses isolated fixtures for first build,
four-artifact reuse, old architecture/pin/missing markers, altered supervisor,
malformed SQLite, empty/corrupt/PID-reuse locks, concurrent builders, valid
runtime mutation, every publish-step fault plus a TERM journal gap, use-phase
lock contention, lock-FD isolation from a consumer's background process, and
status aggregation.
`scripts/test-deterministic-rootfs-tar.sh` proves byte equality and no-replace
behavior with executable, symlink, hardlink, and deliberately different host
mtime fixtures. A separate CI job then runs
`scripts/prepare-rootfs-candidate.sh --verify-only`, performs two real RootFS
builds with one recorded-digest `fakefsify` under a host-architecture-selected
trusted tool path, and compares the complete tar, receipt, identity, SQLite
database, and data tree. It also generates
`ROOTFS_BUILD_ENVIRONMENT.json` with host/tool/linked-library evidence. The temporary
RootFS and receipt are deleted before the job ends and no RootFS bytes are
uploaded; two explicit `--output` invocations can additionally compare the same
`fs.tar.gz`.

Host-test and production iOS iSH builds must include
`-DISH_DISABLE_SKIP_BRK=1` in `c_args`. The runner strictly introspects an old
`build-check`; without the macro it exits 65 and requires safe reconfiguration
instead of silently reusing a cache that skips fatal guest SIGTRAP. A real
`procfs_test` also runs `kill -TRAP $$` and requires the shell to terminate with
SIGTRAP without producing post-trap output.

These tests use a local development RootFS to prove boot/spawn/procfs/command
paths. The pin only makes test inputs reproducible: it does not authorize
committing or publishing the RootFS, and does not replace provenance, signature,
final-content digest, or license review.

## iOS 18 XCFramework

```sh
PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH" \
  scripts/build-ios.sh
scripts/verify-ios-artifact.sh
```

Acceptance checks:

1. `Info.plist` contains exactly the device/simulator records, both binding
   `LibraryPath`/`BinaryPath=libIshKernel.a` and `HeadersPath=Headers`; normalized
   in-slice library/header paths and all of their parents are symlink-free;
2. both libraries are arm64 only; apart from archive symbol indexes, every
   Mach-O member must be parsed by `otool` and have an iOS/iOS Simulator
   `LC_BUILD_VERSION` with `minos` exactly 18.0; real C final links target
   `arm64-apple-ios18.0` and `arm64-apple-ios18.0-simulator`, treat linker
   warnings as errors, and have `vtool` report the exact platform and iOS 18;
3. exported symbols include session retain/release, shutdown, bundled supervisor,
   and required kernel join/soft-halt symbols, while obsolete FFI is absent;
4. the embedded supervisor is a little-endian ELF64 AArch64 static ET_EXEC with
   no `PT_INTERP`/`DT_NEEDED`; the generated C array and both slice-linked Mach-O
   blobs are byte-identical to that ELF, with length/hash/path bound to it;
5. XCFramework `Licenses/` matches repository LICENSE/NOTICE/release docs/iSH/musl notices.

CI and the isolated Release build both invoke `verify-ios-artifact.sh`, so a
Release cannot use weaker `Info.plist`/Swift-only validation than a pull request.
The script verifies the actual supervisor and XCFramework bytes in that build.

## Two Swift real-link boundaries

RootFS-independent tests in `Tests/IshEmbedTests` cover:

- the instance gate serializing boot/shutdown, an oneshot or live-session lease
  making shutdown busy before old-native entry, and spawn retaining its lease
  through completion of native session close;
- shutdown busy restoring the same running handle, timeout and other terminal
  failures quarantining ordinary calls while retaining shutdown cleanup retry,
  and success consuming the process's only lifecycle and rejecting a later boot
  before native entry;
- oneshot/read rejecting NaN or infinite timeouts before native entry; spawn
  budgets deduct Swift option marshalling and expire before native entry when
  less than 1 ms remains instead of degrading to no timeout;
- the session gate detaching first, rejecting new calls, and waiting for an
  admitted call before native close;
- public source-compatibility smoke such as writable `keyEncoder`; strict
  concurrency compilation also verifies that `setEventHandler` keeps its
  original non-`@Sendable` public shape.

These tests do not pin a changeable total count and do not claim that close
always completes in a fixed time. A nil-timeout read normally re-enters the gate
after at most its current 100 ms poll. Other admitted C calls determine the
upper bound; a permanently blocked non-read call under an old/custom supervisor
keeps close waiting.

First validate the old binary before publication:

```sh
scripts/test-swift-ios.sh --manifest-binary
```

Then validate the local Stage1 binary:

```sh
scripts/test-swift-ios.sh \
  build/xcframework/libIshKernel.xcframework
```

The script creates an isolated package, selects an iOS 18+ arm64 simulator,
enables complete strict concurrency and warnings-as-errors, runs XCTest, and
rejects a false success where every test was skipped.

Stage1 Swift is an old-ABI-compatible layer. Do not add assertions here that
call native retain/release or depend on typed status and new Terminal/VT
behavior. Complete borrowing/cancellation/typed-lifecycle tests arrive with
Stage2. After the maintenance Release, run `--manifest-binary` again to prove the
updated manifest real-links from the public URL.

## Review and commit gate

Before an important push and before PR merge, run Codex CR to review:

- whether the diff stays within Stage1 native ABI scope;
- session/instance concurrency, references, shutdown, and error paths;
- wire header/payload handling, length arithmetic, and bounded queues;
- iSH gitlink, licensing, and Corresponding Source;
- whether bilingual docs accidentally present Stage2 as delivered;
- whether pre-/post-publication manifest state is described consistently.

P1/P2 must be cleared before push or merge. Tests do not replace review, and
review does not replace real builds or sanitizers.
