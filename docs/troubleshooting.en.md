# IshEmbed troubleshooting

[简体中文](troubleshooting.md) | English

First decide whether the failure belongs to the manifest binary, source,
locally built XCFramework, RootFS, or PocketRoot product layer. The most common
Stage1 category error is treating “ABI-transition source merged” as “transition
binary already published.”

## First: record the version matrix

```sh
git rev-parse HEAD
git submodule sync -- third_party/ish
git submodule update --init --checkout -- third_party/ish
git submodule status third_party/ish
rg 'ISH_EMBED_ABI_VERSION' include/ishembed.h
rg 'ISH_PROTO_VERSION' protocol/proto.h
rg -n 'url:|checksum:' Package.swift
git status --short
```

`git submodule sync` updates an old checkout's cached submodule URL in
`.git/config` to the current SSH-over-443 address in `.gitmodules`; editing
`.gitmodules` alone does not update that cache.

Also record Xcode/Swift/Zig versions, runtime platform, XCFramework origin and
checksum, and RootFS provenance, size, and SHA-256. Without this matrix, logs
may describe different states.

The current correct combination is C ABI 1, wire v4, Swift not calling
retain/release, and a manifest pointing at the public `v0.4.0-abi.13`. After
`v0.4.0-abi.14` publication, only the manifest URL/checksum should switch to the
maintenance asset.

## Missing retain/release or other link symbols

Symptom: the linker reports undefined `ish_embed_session_retain`,
`ish_embed_session_release`, or join/soft-halt symbols.

```sh
nm -gU path/to/libIshKernel.a | awk '{print $NF}' | sort -u \
  | rg 'ish_embed_session_(retain|release)|ish_ffi_task_join|soft_halt'
```

- If Stage1 Swift calls retain/release, Stage2 code leaked in. Restore the
  old-ABI-compatible Swift layer rather than expanding this maintenance release.
- If a local Stage1 XCFramework lacks them, it was built from an old commit,
  gitlink, or cache. Rebuild in an isolated path.
- If `v0.4.0-abi.14` is public but the manifest is still `v0.4.0-abi.13`, inspect
  whether the release commit/default-branch fast-forward completed. Never guess
  a checksum.

## `Package.swift` looks “not updated”

Between the maintenance PR merge and Release publication, a `v0.4.0-abi.13`
manifest pin is expected. The release script rebuilds and validates assets from
the merged commit, creates a manifest-only release commit, publishes and
verifies assets, then fast-forwards the default branch. Thus the branch never
advertises a 404 URL.

Only after confirming that the `v0.4.0-abi.14` Release is public is an old
manifest abnormal:

```sh
gh release view v0.4.0-abi.14 --repo jacklv-coder/ish-arm64-pkg
git ls-remote --tags origin refs/tags/v0.4.0-abi.14
git fetch origin
git log --oneline --decorate -5 origin/main
```

## Boot failure

Check each layer:

1. RootFS must be the writable fakefs directory containing `data/` and
   `meta.db`, not a tar.gz, Alpine directory, or host `/`.
2. Check sandbox permission, free space, file protection, and path lifetime.
3. The default supervisor path uses the release XCFramework's embedded blob.
   Before installation, SHA-256 over the actual bytes must match build metadata
   and the content-addressed guest path. A custom `supervisorGuestPath` skips
   embedded installation and this default digest check; the caller must supply
   a guest-absolute static AArch64 ELF with valid mode, integrity, provenance,
   and wire v4 compatibility.
4. Preserve output sent to `kernelLogFD`. Logs are best-effort and do not replace
   the return code.

## Guest `uname` triggers SIGTRAP

Symptom: the guest exits while booting or while the shell probes system
information, with `__chk_fail_overflow`, `__strcpy_chk`, `do_uname`, and
`sys_uname` in the crash stack. A host name longer than the 65-byte Linux
`new_utsname` field commonly triggers it.

- `v0.4.0-abi.2` does not contain this fix; do not hide the defect by shortening
  a CI runner name.
- `v0.4.0-abi.3` bounds every fixed-width guest `uname` field and
  guarantees NUL termination.
- Source validation can run iSH's `uname` unit test and
  `scripts/run-host-tests.sh --smoke`. After publication, the Xcode 16 / iOS 18
  gate should also prove a real guest `uname -a` succeeds.

## A blocked guest command survives cancellation

Symptom: after signalling a guest command blocked in `poll`, `nanosleep`, or
another host syscall, the command does not exit; the cancellation deadline
cannot confirm cleanup, or a later command cannot recover.

- `v0.4.0-abi.4` explicitly unblocks internal SIGUSR1 on the embedded bootstrap
  and every guest task thread, allowing guest signals to interrupt blocking
  host syscalls.
- The published `v0.4.0-abi.5` adds the finite streaming control-path deadline
  on top of that behavior. Published `v0.4.0-abi.6` completes Swift-marshalling
  and stdin-close reuse of the original SPAWN deadline. Published
  `v0.4.0-abi.7` adds guest-atomic no-replace rename. `v0.4.0-abi.8` makes
  finite-session stdin write/close share one absolute deadline. `v0.4.0-abi.9`
  adds per-call stdin write/close timeout APIs. Published `v0.4.0-abi.10` fixes
  forced task-teardown lifetime races. Published `v0.4.0-abi.11` delays a
  destructive wait until the full thread group is quiescent. `v0.4.0-abi.12`
  fixes Apple writer starvation and retries a briefly deferred destructive
  reap. Published `v0.4.0-abi.13` adds AArch64 AdvSIMD vector `REV16` emulation;
  planned `v0.4.0-abi.14` corrects Darwin/Linux `sockaddr` layout translation;
  none changes guest
  signal semantics.
- Source validation should run iSH's `internal-signal-mask` test, package-level
  host/iOS tests, and a real-RootFS “cancel → native termination confirmed →
  later command → shutdown” flow.

For a local `scripts/run-host-tests.sh` failure, first run
`scripts/build-rootfs.sh --print-identity`, then
`scripts/build-rootfs.sh --verify-bundle build`. The runner only reuses a
four-artifact generation whose receipt/recipe, SQLite row types/16-byte
stats/single root, meta/data paths, and critical AArch64 digests validate. Lock
files intentionally remain; kernel `flock`, not file existence, determines
ownership, so do not delete or take over one merely because it exists.
Replacement exchange keeps `build/fs` visible, failure restores the current
generation, and success discards the old generation with staging rather than
creating `fs.previous.*`. If SIGKILL/power loss interrupts multi-path publish,
receipt-last makes the partial generation fail validation and rebuild next time;
do not manually splice receipts, archives, or directories.

Do not interpret `ROOTFS_RECEIPT` or a successful `--verify-bundle` as “current
`fs` equals the tar.” The receipt binds the static marker and initial
`fs.tar.gz`/`SHA256SUMS`. After legitimate runtime writes, verification checks
current-tree structure, database/critical identity, and initial archive
integrity, but does not compare every current-tree byte with the tar. The initial
tar is not a current-tree recovery backup and cannot be committed or shipped in
a Release.

Stage1 Swift still exposes `IshError.raw(code, message)`; do not write diagnosis
against Stage2 typed statuses. [`include/ishembed.h`](../include/ishembed.h) is
authoritative for C status codes.

## `ISH_ERR_SUPERVISOR_INSTALL`

The default path calls the installation FFI only after three values agree: the
SHA-256 of the actual embedded bytes, 64-character lowercase build metadata,
and `/sbin/.ishsv-ishembed-sha256-<digest>`. This error usually means the blob
and generated metadata came from different builds, bytes changed, or an old
build cache was reused. Rebuild in an isolated directory and run
`scripts/verify-ios-artifact.sh`; do not hand-edit the digest or
content-addressed path to bypass the failure.

An explicit `supervisorGuestPath` already bypasses the default blob installation
and digest gate and is not a shortcut for repairing an inconsistent default
asset. With a custom path, the caller must establish provenance, signatures,
the actual file digest, executable mode, and wire v4 compatibility. Default
SHA-256 comparison proves only that bytes match fixed metadata from the same
build; it is not a digital signature and does not authenticate download origin
or publisher. This source PR carries neither RootFS content nor a prebuilt
binary. Build local test binaries from the current checkout; production binaries
must come from a later Release whose gates passed.

## `ISH_ERR_PROTOCOL` / handshake failure

Host and supervisor must use exact-match wire v4. Common causes:

- new host library configured to execute an old RootFS `/sbin/ishsv`;
- rebuilding only the supervisor or only the library;
- an old XCFramework in app caches;
- unframed bytes, a truncated frame, or payload over 1 MiB.

Prefer the XCFramework's embedded supervisor and obtain both peers from one
clean build. Public C ABI 1 and internal wire v4 appearing together is correct;
do not change the ABI constant to 4 to “fix” the handshake.

## Legacy e2e becomes slow or times out after pinning the new iSH

First distinguish “package installation/compilation keeps making progress but
exceeds the time budget” from a deadlock with no progress. JIT coherence returns
from a direct chain or RET cache when the next target intersects pending dirty
code pages, then drains dirty pages and disconnects affected chaining/caches.
Data-only writes may keep chaining, but every chained transfer still pays a
check, and write-heavy legacy Alpine end-to-end tests can remain materially
slower.

The latest exact-page fix only narrows the invalidation set. A single-page run
with no transition and explicit `asbestos_invalidate_page` compare exact block
pages; a cross-page block's second page uses `end_addr`. Only the multi-page
hashed bitmap after a page transition can conservatively over-invalidate on a
bucket collision. Passing the single-page collision regression therefore proves
that a remote same-bucket block survives; it does not prove that ordinary writes
never return to the dispatcher or that old throughput has been restored; a code
page intersection must still return.
Preserve complete progress logs and run the testing guide's
`dirty-page-trace`/`dirty_page_test` plus the performance baseline. Do not hide a
correctness or performance problem by disabling invalidation, restoring unsafe
direct chains, or extending CI timeouts without bound.

## Spawn/chroot failure

- `argv` must be nonempty and the guest executable/cwd must exist.
- `chrootPath` is a guest absolute path such as `/srv/vms/default`, not a host URL.
- Guest PID 1 rechecks devices, devpts/procfs, and required directories for every
  absolute chroot spawn. Inspect supervisor logs and RootFS writability/integrity.
- Chroot is not an adversarial sandbox. PocketRoot still owns permission,
  network, and resource policy.

## No session `EXITED` and the log reports instance fail-close

This is a conservative failure because the supervisor could not prove that all
guest descendants were removed; it is not a normal session exit. After cleaning
the tracked group and TTY foreground job, PID 1 also handles untracked children
adopted by the subreaper after double fork/`setsid` and requires two consecutive
clean `/proc` scans. A scan, exact-PID kill, reap, or two-second deadline failure
causes guest-instance-wide last-resort cleanup and supervisor exit, without a
successful `EXITED` or `SHUTDOWN_ACK`.

- Do not synthesize an exit status for the missing success frame or reuse that
  instance; finish host-side close/shutdown and boot a new one.
- Preserve the guest-log fail-close reason and inspect procfs, iSH child reaping,
  and whether a custom supervisor came from the same build as the XCFramework.
- Product timeouts still apply to commands that daemonize. This cleanup is a
  final isolation-integrity guard, not a background-service lifecycle API.

## Output stops or `ISH_ERR_OUTPUT_LIMIT`

Native limits are the final memory guard: 1 MiB per payload, 4 MiB/4096 unread
frames per session, 8 MiB one-shot stdout, and 4 MiB one-shot stderr.

- Continuously `read` streaming sessions on a separate task; do not wait for
  child exit before draining.
- Check for blocking reads or CPU-heavy parsing on the main actor.
- Enforce a smaller product budget and close stdin/terminate/close on overflow.
- Stdin is also queued with a bound; do not produce indefinitely when the guest
  is not consuming.

Stage1 does not deliver new Terminal callback-drop events or VT parser limits.
Logs mentioning those APIs indicate Stage2 candidate source, not this stage.

## `ISH_ERR_CONTROL_LIMIT`

The global host→guest control budget (4 MiB/256 complete wire frames) could not
admit the next frame, usually because the supervisor stopped reading or the
control pipe is severely congested. The failing next frame was not admitted, but
a large `ish_embed_session_write` is split into frames and an earlier chunk may
already be delivered; the whole call is not an atomic write.

For long commands, use 64 KiB or smaller chunks with the per-call timeout API,
drain output, and check cancellation between chunks. After timeout, retry only
a single frame whose admission status is known.

Within the total budget, 4 KiB/16 frames are reserved only for internal
close/shutdown lifecycle cleanup. Ordinary calls cannot consume them. If even a
critical frame cannot be admitted, the writer fails, or session close is not
confirmed within one second, the runtime closes the control direction so the
guest cleans up every child on EOF. New calls then return not-running; finish
session close and instance shutdown instead of trying to reuse that instance.

- Pause new input and do not retry in a zero-delay loop; keep draining session
  events/output and observe exit state.
- If the supervisor is unresponsive, terminate/close the session under the
  product timeout policy instead of accumulating input indefinitely.
- If the application protocol retries after this error, use idempotent commands
  or an explicit offset/acknowledgement scheme for partial delivery.

## Crash or error during concurrent close/read/write

The native C API supports overlap between an already-retained call and close.
Stage1 Swift does not use the new reference functions because it must link the
v0.3.3 binary. Instead, `IshSessionCallGate` automatically prevents close/call
UAF: close detaches the handle, rejects new calls, waits for every admitted C
call, and only then invokes native close. Callers no longer need their own lock
around every I/O operation.

A `read(timeout: nil)` uses 100 ms C-read polls and normally re-enters the gate
after the current poll. Close has no fixed upper bound, however. If an admitted
non-read C call, such as a blocked write under v0.3.3 or a custom supervisor,
never returns, close also waits indefinitely.

Callers should still stop and await read/write/signal tasks, then call the
idempotent `close()` and start no new operation afterward. Full native
borrowing/cancellation/typed lifecycle remains Stage2. Adding retain/release
calls alone to Stage1 breaks old-binary linkage.

## Shutdown returns busy

`ISH_ERR_BUSY` means a live session or active instance call remains:

1. stop and await spawn/runOneshot;
2. finish/await read, write, and signal calls;
3. close every session;
4. call shutdown again.

The Stage1 Swift instance gate holds a lease for each oneshot and retains a
spawn lease until the session's native close completes. Any lease makes
shutdown immediately busy before entering v0.3.3 native code; overlapping boot
or shutdown transitions are also rejected. Native `BUSY` preserves the running
handle for retry; timeout and other terminal failures quarantine ordinary calls
but retain shutdown cleanup retry. This prevents old-ABI UAF but does not cancel
a call, so await it before retrying. The transition native runtime can likewise
return busy or the corresponding error for this state.

Never boot again in the same process after successful shutdown. Process-global
and TLS iSH state makes one lifecycle an explicit architecture constraint.

## iOS build or test failure

- Confirm arm64 macOS, full Xcode, and an available iOS 18+ simulator.
- Do not reuse a build directory from another commit, gitlink, or Zig version.
- If `build-check` reports missing `-DISH_DISABLE_SKIP_BRK=1`, safely
  reconfigure or rebuild that dedicated directory. Do not bypass the gate and
  reuse a cache that suppresses fatal guest SIGTRAP.
- `Package.swift` minimum iOS, actual final-link deployment target, and
  XCFramework slices must all be iOS 18/arm64.
- `test-swift-ios.sh --manifest-binary` tests the old/published pin; no argument
  or a path tests the new local binary. They answer different questions.
- On sanitizer failure, diagnose the first stack before later cancellation or
  leak noise.

## Documentation or script gate failure

```sh
scripts/check-docs.sh
scripts/test-check-docs.sh
bash -n scripts/*.sh
scripts/test-release-version-policy.sh
git diff --check
```

- Every language pair must exist. Chinese pages contain an English switch link
  to the matching mirror, and English pages contain a Simplified Chinese switch
  link back to the matching primary document.
- Remove links whenever removing documents. Stage1 must not restore a v0.4
  migration guide because the complete Swift API is not delivered.
- ABI-specific release notes belong only to `v*-abi.*`; ordinary rc or stable
  tags must not receive them.

## Release failure

Do not immediately delete a tag/draft or rerun the same tag. Save the printed
staging path, Release id, release commit, tag raw OID, and digests, then follow
the [release recovery guide](releasing.en.md#failure-and-recovery). A network
failure after publication PATCH is an uncertain state; automatic deletion could
remove a valid public object.

## Filing an issue

Include parent commit, iSH gitlink, manifest URL/checksum, local/Release
XCFramework digest, C ABI/wire versions, iOS/Xcode/Zig versions, RootFS manifest
(without restricted assets), minimal reproduction, status code, logs, and exact
test command. Remove tokens, private keys, personal paths, and product data.
