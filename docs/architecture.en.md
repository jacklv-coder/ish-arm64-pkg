# IshEmbed architecture and lifecycle

[简体中文](architecture.md) | English

This document describes the Stage1 native ABI transition. The public C ABI
remains version 1, the internal wire protocol is exact-match v4, and the Stage1
Swift wrapper remains v0.3.3-ABI compatible. The complete Swift lifecycle and
Terminal/VT changes are not delivered in this stage.

## Layers and responsibilities

```text
PocketRoot / SwiftUI / UIKit
          │
          ▼
Sources/IshEmbed
v0.3.3-compatible Swift API
          │
          ▼
include/ishembed.h                    public C ABI = 1
          │
          ▼
host/ishembed.c + ffi/ish_ffi.c       instance/session/threads/queues
          │
          ▼
protocol/proto.h                      internal exact-match wire = 4
          │
          ▼
supervisor/ishsv.c                    guest PID 1 / child / PTY / chroot
          │
          ▼
third_party/ish + fakefs RootFS       emulator/kernel state + persistence
```

- Swift provides `IshInstance`, `IshSession`, `IshVM`, and the current
  Terminal/VT interfaces.
- The C host owns the instance, session table, I/O pumps, queues, reference
  counts, and shutdown coordination.
- The FFI exposes only iSH internals needed for embedding. The pinned Stage1 iSH
  revision contains joinable-kernel-thread and soft-halt support. These are
  narrow runtime-lifecycle patches, not a rewrite of the upstream product. That
  fork also carries experimental arm64 BRK skipping; production and host-test
  iSH builds explicitly define `ISH_DISABLE_SKIP_BRK=1` so a fatal guest
  SIGTRAP cannot forge register state and continue.
- Guest PID 1 manages commands, PTYs, signals, stdin, and child reaping.
- RootFS supplies persistent fakefs content, is installed independently by the
  host, and is outside the Release.

`third_party/ish` points to the project's maintained `ish-arm64` fork; this is
not a local rewrite of somebody else's upstream checkout. Join/soft-halt,
embedded lifecycle, and JIT dirty-page coherence require changes inside iSH
core and cannot be completed by the outer FFI alone. PocketRoot builds, tests,
and releases must also pin one reviewed exact revision. Generic fixes can still
be prepared for upstream, but until upstream accepts and releases them, the fork
keeps these narrow differences reviewable through independent PRs, CI, and the
outer gitlink. It does not claim ownership of or replace the upstream project.

## JIT self-modifying-code coherence

iSH's ARM64 JIT can directly chain translated blocks, so guest writes cannot
track only the last page: a cross-page store or several stores in one block can
overwrite that slot and then continue into stale translation. Each TLB now owns
a 1024-bit dirty-bucket set matching `FIBER_PAGE_HASH_SIZE`. C fast writes,
write misses, cross-page writes, and ARM64/x86 gadget paths keep the current exact
page in `dirty_page`; only a page transition ORs the prior page into the bitmap,
At dispatcher drain, the final page keeps its exact identity and takes exact
invalidation when there was no prior bucket; only after a page transition is the
final page added to the bitmap for conservative multi-page batching. Consecutive
same-page writes therefore avoid repeated bitmap work without losing exact
identity, while arbitrary multi-page writes remain lossless. READ paths do not
mark. These fields belong only to runtime invalidation and are cleared
immediately after the dispatcher drains them.

The x86 `ptraceomatic`/`unicornomatic` memory comparison cannot reuse those
hashed buckets: addresses 1024 pages apart collide, and the runtime drain runs
before the tools compare memory. The tools therefore opt into a separate exact
20-bit page bitmap (128 KiB plus a 2 KiB second-level summary). Write paths record
the full page number only on page transitions and drain records the final page.
The diagnostic set survives runtime clearing until every page compares
successfully; a mismatch leaves it intact for retry. Normal execution keeps the
pointer `NULL` and allocates none of this diagnostic storage.

Kernel/host code that modifies guest memory through
`mem_ptr(..., MEM_WRITE*)` uses two invalidation edges. It invalidates before
returning the writable pointer, then calls `mem_did_write` after the actual
`memcpy`, atomic RMW, or `memset` while the same `mem->lock` read lifetime is
still held. A compiler that publishes old bytes in that interval is therefore
removed by the post-write edge. The normal path uses atomic occupancy so a
data-only page does not contend on the global JIT mutex; oversized, out-of-range,
or wrapping internal reports conservatively invalidate everything.

On return to the dispatcher, the runtime checks dirty pages and moves matching
blocks to jetsam while holding the same lock used for translation compile/insert,
then uses `invalidate_gen` to clear block/return caches. A write run with no page
transition still retains the final page's exact identity, so candidates are
filtered by that exact page; explicit `asbestos_invalidate_page` uses the same
filter. For a translated block spanning two pages, `page[0]` compares
`block->addr` while `page[1]` must compare `block->end_addr`. Invalidating the
second page therefore removes a real cross-page block without evicting a remote
block that merely shares its hash bucket. Only a **multi-page** dirty set that
has already transitioned pages folds earlier identities into the hashed bitmap
and can conservatively over-invalidate because of a collision; it cannot miss a
required invalidation.

Bucket inspection, invalidation, and dirty-set clearing cannot trust an
empty-bucket fast path outside the coherence lock: another thread could compile
old bytes, wait to insert, and publish a stale block after that set clears. The
implementation reads atomic occupancy with acquire semantics while holding the
coherence read side, and may skip the global mutation mutex only when no code is
a candidate. Before entering the next direct-chain or RET-cache target, assembly
guards compare that block's `addr`/`end_addr` with the current exact dirty page
and conservative buckets for earlier pages. An intersection returns to the
dispatcher to drain dirty state and invalidate affected chaining/caches;
data-only writes that do not intersect the target may keep chaining without
losing pending state. This avoids an unconditional dispatcher exit, although
each chained transfer still pays a coherence check and write-heavy legacy
end-to-end workloads can remain materially slower. See the
[testing guide](testing.en.md#jit-performance-baseline) for the measured baseline.

A normal task holds the read side of `mem->lock` while running JIT code, whereas
a lazy mapping may temporarily drop that read lock and acquire the write side.
To avoid reversing this order against dirty draining and compilation, the
compiler resolves every page the next instruction may read before acquiring the
write side of `dirty_coherence_lock`. Decoder reads then switch to a no-fault TLB
mode: a miss emits a guest fault instead of entering an MMU slow path that could
upgrade `mem->lock`. The x86 decoder also emits `#UD` before reading any 16th
instruction byte, while A64 rejects a non-4-byte-aligned PC before reading; a
valid A64 instruction cannot straddle a 4 KiB page. Compilation is therefore
serialized with draining from guest-byte read through block insertion without
requesting an outer memory write lock under the inner coherence lock.

Architecturally valid ARM64 self-modifying code performs a `DC ...`,
`IC IVAU, Xt`, and barrier sequence after writing. Data-cache maintenance remains
a NOP over the shared host bytes. `IC IVAU` publishes Xt's page into the TLB of
the thread executing that IC, then ends the translated block and returns to the
dispatcher. This covers the valid cross-thread sequence where writer and
cache-maintenance executor use different TLBs. Dirty state is consumed before a
modified target executes, even if that target was directly chained. Guest code
that omits the required instruction-cache maintenance has no immediate visibility
guarantee. The emulated `CTR_EL0 == 0x84448004` deliberately keeps
DIC and IDC (bits 29 and 28) clear, so guest code cannot legally omit the DC/IC
steps. Tests bind that constant and both bits to the `IC IVAU` decoder boundary.

The PocketRoot Stage1 production artifact contains an **ARM64 guest only**. The
x86 guest path remains a compatibility and regression build for the fork:
same-thread writes check the next target at central block-chain and
return-cache-chain boundaries and return to the dispatcher only when it
intersects pending dirty code pages. This stage does not promise global
publication for cross-thread x86 self-modification and does not package an x86
guest slice in the PocketRoot XCFramework.

## Boot flow

1. The host validates RootFS layout, provenance, and hash, then passes a writable
   `data/` + `meta.db` directory to `ish_embed_boot`.
2. The C host creates protocol and log pipes and prepares instance state.
   `out_instance == NULL` is rejected before installation or thread side effects.
3. Without `supervisor_guest_path`, a release XCFramework atomically installs
   its embedded static AArch64 supervisor only after hashing the actual bytes
   and matching their SHA-256 build metadata. It uses a private content-addressed
   fakefs path and verifies all bytes and executable mode. It does not replace a
   RootFS-owned `/sbin/ishsv`. An explicit custom path bypasses this default blob
   installation and digest gate and is entirely caller-owned. SHA-256 here proves
   only that the actual bytes match fixed metadata from the same build; it is not
   a digital signature and does not authenticate download provenance or publisher
   identity.
4. The iSH kernel starts on a joinable pthread and enters guest PID 1.
5. Host and supervisor exchange `HELLO`/`HELLO_ACK`, exactly checking public ABI
   version 1 and wire protocol v4. Boot returns the instance only after success.

Failure paths stop started pumps, request kernel soft-halt, and join joinable
threads. iSH still uses process-global/TLS state, so one valid instance lifecycle
is supported per host process. Do not boot again after successful shutdown.

## Public C ABI 1 versus internal wire v4

The numbers solve different compatibility problems:

- `ISH_EMBED_ABI_VERSION == 1` describes exported C symbols, structures, and
  calling conventions between the app/Swift layer and XCFramework. Stage1 is
  additive and compatible, so the ABI constant does not increase.
- `ISH_PROTO_VERSION == 4` describes frames between the host and the guest
  supervisor packaged in the same XCFramework. They build and ship together;
  v4 therefore requires an exact match rather than cross-version negotiation.

Each wire frame has a 12-byte header (magic, version, type, flags, payload
length, and session id) plus at most 1 MiB of payload. Fixed-width payload fields
use protocol-defined endianness, not native C struct layout. v4 adds
`SESSION_CLOSE` for complete live-session cleanup.

## Session lifecycle and references

A C handle returned by `ish_embed_spawn` owns one owner reference:

1. A native call may obtain a borrow with `ish_embed_session_retain`; every
   successful retain must be released.
2. `ish_embed_session_close` atomically prevents new calls and consumes the
   owner reference.
3. A call that already retained may overlap close and finish. Memory is reclaimed
   only after the final reference is released.
4. The direct child first becomes a session leader that cannot leave its
   process group, then confirms `pid == pgid` to the supervisor over a dedicated
   pipe. Only then does the supervisor store and use the separate PGID. When
   `waitid(..., WNOWAIT)` observes leader exit, it cleans same-group background
   processes and the TTY foreground job before the final `waitpid`, then clears
   PID/PGID. A later close never signals through a released numeric identity.

This is a Stage1 **native capability**. To remain compatible with the v0.3.3
binary, Stage1 Swift does not call retain/release. Instead, its internal
`IshSessionCallGate` supplies old-ABI-safe Swift protection:

1. `withRaw` checks for an attached handle under an `NSCondition` and increments
   `activeCalls`;
2. `close()` detaches first so every new call fails immediately;
3. close waits for `activeCalls == 0` before native close, so a v0.3.3 handle is
   never freed while an admitted C call still uses it;
4. `read(timeout: nil)` uses bounded 100 ms C reads and re-enters the gate between
   polls, allowing close to make the read loop leave.

The gate automatically prevents UAF between close and admitted C calls; callers
no longer need to lock around every I/O operation themselves. Close waits rather
than cancels, however. If a non-read C call never returns under v0.3.3 or a
custom supervisor, `activeCalls` never reaches zero and close also waits. Callers
should still stop and await their tasks first. Full native retain/release
borrowing, cancellation, and typed lifecycle remain Stage2.

The old binary also has no instance retain. `IshInstanceCallGate` therefore
serializes idle/booting/running/shutting-down states and holds a call lease for
each oneshot. A successful spawn transfers the same lease into `IshSession`
without a zero-count window; it is released only after native session close
returns. Shutdown first atomically closes admission. Any oneshot/session lease
causes immediate `ISH_ERR_BUSY` without entering v0.3.3 native shutdown; with no
lease, one caller owns the transition. Native `BUSY` restores the same handle
and running state; timeout or another terminal cleanup error quarantines
ordinary calls but permits shutdown retry. Success alone clears the handle.
Boot/shutdown, two shutdown callers, and shutdown/call therefore cannot
interleave a free of the old-ABI handle.

## Threads and backpressure

The host separates responsibilities that might otherwise block each other:

- the control writer exclusively owns the host→guest fd and orders frames;
- the event reader exclusively owns the guest→host fd, validates frames, and
  dispatches them into session inboxes;
- the guest-log drain continuously empties untrusted guest stderr;
- the log-sink writer sends best-effort logs to the caller fd without letting a
  slow sink block protocol progress.

Authoritative ceilings live in [`include/ishembed.h`](../include/ishembed.h) and
source: a 1 MiB protocol payload, 4 MiB/4096-frame unread output per session,
8 MiB one-shot stdout, and 4 MiB one-shot stderr. The supervisor also has a
bounded per-session stdin queue and handles partial writes/`EAGAIN`. A limit
causes an observable error or session termination rather than unbounded host
memory growth.

The host→guest control path has a separate global 4 MiB/256-frame total budget,
with 4 KiB/16 frames available only to internal critical lifecycle messages. A
complete wire frame, including its 12-byte header, counts from admission before
payload allocation through queued, in-flight, or synchronously completed waiter
state until the actual free. An ordinary call returns `ISH_ERR_CONTROL_LIMIT`
when the next frame cannot be admitted; that frame never enters the pipe, but an
earlier chunk of the same multi-frame `session_write` may already be delivered.

`SESSION_CLOSE`, `SHUTDOWN`, and the oneshot `STDIN_CLOSE → TERM → SIGKILL`
sequence use reserve-aware asynchronous admission, so a caller thread does not
wait indefinitely behind a blocked writer. If session close cannot be admitted,
the writer fails, or exit is not observed within one second, the runtime enters
shutdown and closes the control direction; guest PID 1 cleans up every child on
EOF. A separate gate serializes spawn measure/build/send/free so multiple
roughly 1 MiB staging payloads cannot exist concurrently outside the control
budget. Stop, EOF, and error drains share the same free/accounting path.
The critical byte reserve must hold at least the smallest lifecycle frame at
compile time. Finite oneshot and streaming-spawn deadlines start at API entry
and cover the instance gate, spawn gate, and SPAWN admission to the control
queue. Finite streaming sessions use ordered asynchronous admission for SPAWN,
stdin close, and terminate so a stalled control writer cannot consume the
product deadline. The session retains SPAWN's native absolute deadline, and
stdin close reuses it when acquiring the writer gate; expiry returns
`ISH_ERR_TIMEOUT` without starting a fresh wait. Stdin close returns
`ISH_ERR_BUSY` rather than waiting behind an active stdin write; authoritative
`EXITED` still confirms termination.

Stage1 does not deliver new typed Swift statuses or bounded Terminal callback
delivery. Do not describe native backlog protection as if the Stage2 Swift
callback policy were already present.

The public closure accepted by `IshTerminal.setEventHandler(queue:_:)` keeps its
original non-`@Sendable` signature. A private `@unchecked Sendable` box contains
the existing handlerQueue crossing so strict-concurrency builds pass without a
caller source break. This does not deliver Stage2 callback queue/drop policy.

## Signals, PTYs, and reaping

- A pipe-session signal targets the tracked command process group.
- TTY Ctrl+C/Ctrl+Z normally comes from Swift control bytes and the tty layer
  routes it to the foreground process group. A direct signal still targets only
  the tracked group.
- Resize updates PTY dimensions and triggers SIGWINCH; non-TTY sessions may ignore it.
- Terminate sends SIGTERM, waits the supervisor's fixed roughly 1.5-second
  interval, then sends SIGKILL if required. The existing `grace_ms` shape remains
  for compatibility.
- v4 close handles the live TTY shell group, foreground job, and transports.
  Normal leader exit also uses `waitid(..., WNOWAIT)` to retain the zombie as
  an identity anchor, SIGKILLs the residual group through the immutable PGID
  validated by the spawn-time readiness pipe, and only then calls `waitpid`,
  clears identity, and publishes exit.
  Same-group background children therefore cannot escape after the leader, and
  no post-reap PGID-reuse signalling window exists.
- The supervisor is also a child subreaper. A descendant that escapes the
  tracked group through double fork/`setsid` is eventually adopted by PID 1.
  PID 1 excludes processes still owned by another session/TTY, kills and reaps
  an untracked child by exact PID, and requires two consecutive clean `/proc`
  scans before publishing successful `EXITED`.
- The embedded boot path mounts a supervisor-visible root procfs before starting
  PID 1. This root mount is the authority for adopted-child scans; per-VM procfs
  mounts remain separate and serve processes inside each chroot.
- A scan, kill, reap, or two-second cleanup-deadline failure triggers an
  instance-wide fail-close: the supervisor performs guest-wide last-resort
  cleanup, exits, and emits neither successful `EXITED` nor `SHUTDOWN_ACK`.
  Failure to prove restored isolation therefore closes the protocol instead of
  reporting an apparently normal session completion.

## Chroot preparation and isolation boundary

Before spawning with an absolute `chroot_path`, guest PID 1 revalidates and
repairs required device nodes, devpts/procfs mounts, and the conventional
`/root` home as needed. It does not permanently cache a root as prepared, so a
same-path RootFS replacement or lost mount is checked again. The supervisor
does not create Codex CLI configuration or any tool-specific directory.

Node.js/npm remains an optional general-purpose guest capability. A caller may
install and run it with `apk` in its own writable RootFS, but IshEmbed neither
preinstalls it by default nor automatically installs an npm package.

Chroot isolates only the filesystem view. Sessions share one iSH kernel, host
process, memory budget, and attack surface. It is not a strong security sandbox.
PocketRoot still needs a command allowlist, resource budgets, and product-level
cancellation.

## Shutdown

Recommended order:

1. stop creating commands;
2. cancel or await every `spawn`, `runOneshot`, read, and write task;
3. close every session;
4. call `ish_embed_shutdown` / `IshInstance.shutdown()`;
5. never use the instance or boot again in that process.

Native shutdown returns `ISH_ERR_BUSY` while a live session or active instance
call exists. Before native entry, the Stage1 Swift instance gate adds the old-ABI
protection: every oneshot and live session holds a lease, an existing lease
returns busy immediately, and the state machine also rejects overlapping boot or
shutdown. v0.3.3 shutdown therefore cannot free an instance already used by
spawn/runOneshot/session. Swift clears the handle only on success.
`ISH_ERR_BUSY` restores the same handle and running state for retry after
sessions/tasks are cleaned up. Any other native shutdown failure quarantines the
handle from ordinary calls and boot, while allowing shutdown to be retried so
native cleanup can finish. Once admitted, native shutdown asks the supervisor to exit, closes
and drains pumps, enables iSH soft-halt, waits for and joins the kernel pthread,
then releases the instance.

The gate prevents instance UAF in the old C runtime, but it does not cancel an
operation. After busy, stop and await the oneshot, close sessions, and retry
shutdown. The transition native runtime also returns `ISH_ERR_BUSY` or the
corresponding error for its own live session/active call, so callers should use
the same cleanup order.

## RootFS and release assets

RootFS, XCFramework, and Corresponding Source are independent objects:

| Object | Content | Repository Release |
| --- | --- | --- |
| XCFramework | host runtime, pinned iSH code, embedded supervisor | yes |
| Corresponding Source | parent repository, iSH revision, musl source, and other material needed to rebuild the XCFramework | yes, paired with the binary |
| RootFS | Alpine userspace and fakefs data | no; independent provenance/hash/license/install flow |

Release tooling and source policy reject RootFS content in published assets.
PocketRoot should pin its own RootFS manifest and use same-volume staging,
layout validation, and atomic replacement.

The repository's host-test builder uses schema-v3
`build/fs/.ishembed-rootfs-identity`, a data-only file the runner never sources.
Its recipe prefix covers the builder, deterministic archiver, fixed
`SOURCE_DATE_EPOCH`, supervisor/protocol, iSH
revision/worktree/recursive submodules, fakefsify origin, and Alpine pin. Its
artifact fields bind the actual fakefsify, AArch64 supervisor, BusyBox, and
initial meta/data seal.
The candidate gate reuses one digest-bound host `fakefsify` for two independent
builds and requires full-archive byte equality. CI uploads no archive; actual
distribution remains a separate compliance and authorization transaction.

Concurrency uses permanently stable regular lock files plus kernel `flock`.
File existence is not lock state; owner PID, process-start identity, and random
token authenticate nested calls only. The builder creates `fs`, tar, checksums,
and receipt in unique same-volume staging. After SQLite quick-check, positive
inodes, 16-byte stat BLOBs, exactly one empty root, complete meta/data paths,
AArch64 ELF, and seal checks, a first target uses no-replace and an existing one
uses exchange. The receipt publishes last and commits the generation. Catchable
signals in the rename-to-journal gap are deferred and failures roll back in
reverse; success deletes the old generation with staging. SIGKILL/power loss
cannot guarantee multi-path rollback, but receipt-last makes a partial
generation fail validation and prevents reuse.

The runner holds the lock from validation and optional build through the final
test consumer, closing the validate-to-use replacement window. Reuse allows
valid runtime meta/data mutation while rechecking the four-artifact receipt,
recipe, database/storage consistency, and critical digests.

`ROOTFS_RECEIPT` is a lineage/initial-snapshot commit record. It binds the static
identity marker, the initial `fs.tar.gz`, and its `SHA256SUMS`; it does not seal
the mutable current `fs` byte for byte. After tests or provisioning legitimately
mutate meta/data, `--verify-bundle` validates current-`fs` structure,
database/storage consistency, and critical identity while independently
confirming that the initial tar/sums are intact. It does not prove current-tree
bytes equal the tar. The initial tar is neither a current-tree backup nor a
package, Corresponding Source, or Release asset.

## Stage2 boundary

The following are explicitly outside Stage1:

- full native session retain/release borrowing, cancellation, and typed Swift lifecycle;
- typed `IshError` status and new error mapping;
- bounded Terminal callback queues, drop events, and handler-generation semantics;
- incremental UTF-8 and CSI/OSC parser limits plus related VT API changes.

These changes require a separate review, test cycle, and release after the
transition binary is public, the manifest is pinned, and real-link tests pass.
