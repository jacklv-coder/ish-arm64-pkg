# IshEmbed technical documentation map

[简体中文](README.md) | English

This documentation explains IshEmbed's purpose, Stage1 ABI-transition state,
implementation boundaries, testing, and release transaction. Chinese is the
primary language; the English mirrors support collaboration and release review.

## Read first: four independent version surfaces

1. **Public C ABI**: [`include/ishembed.h`](../include/ishembed.h) is
   authoritative. Stage1 remains ABI 1 and adds compatible symbols.
2. **Internal wire protocol**: [`protocol/proto.h`](../protocol/proto.h) is
   authoritative. Stage1 uses exact-match v4 between host and embedded supervisor.
3. **Swift source and manifest binary**: Stage1 Swift remains v0.3.3-ABI
   compatible and does not call retain/release. The manifest currently pins the
   public `v0.4.0-abi.5`; the release transaction moves it to the
   `v0.4.0-abi.6` maintenance binary only after those assets are public and
   verified.
4. **RootFS and PocketRoot**: RootFS is an independent asset and PocketRoot is
   the product layer. Neither is completed automatically by a runtime PR or Release.

`v0.4.0-abi.5` and its `v0.4.0-abi.6` maintenance release are native-first
transition prereleases, not stable v0.4. The complete Swift lifecycle, typed
statuses, Terminal callback queue, and VT parser changes belong to Stage2.

## Suggested reading order

1. [Root README](../README.en.md): purpose, installation state, and current Swift use.
2. [Architecture and lifecycle](architecture.en.md): how Swift, C ABI, wire v4,
   threads, and guest PID 1 cooperate.
3. [Testing and acceptance](testing.en.md): what every gate proves and the
   pre-/post-publication real-link boundaries.
4. [Troubleshooting](troubleshooting.en.md): trace manifest, symbol, protocol,
   RootFS, session, and build failures.
5. [Release transaction](releasing.en.md): publish the ABI transition, update
   the manifest, and recover safely.
6. [Changelog](../CHANGELOG.en.md): Stage1 delivery and Stage2 deferrals.

## Document index

| Document | Question answered |
| --- | --- |
| [Architecture and lifecycle](architecture.en.md) | How is the runtime implemented? How do ABI 1 and wire v4 differ? |
| [Testing and acceptance](testing.en.md) | What do native, sanitizer, Swift/iOS 18, documentation, and supply-chain gates prove? |
| [Troubleshooting](troubleshooting.en.md) | Where should boot, link, protocol, output, shutdown, or release diagnosis start? |
| [Release transaction](releasing.en.md) | Why does the merged maintenance source still pin `v0.4.0-abi.5`, and when does it become `v0.4.0-abi.6`? |
| [Changelog](../CHANGELOG.en.md) | What is Stage1's scope and compatibility boundary? |

## Architecture on one page

```text
SwiftUI / UIKit / PocketRoot
              │  Stage1: v0.3.3-compatible Swift wrapper
              ▼
public C ABI: include/ishembed.h (ABI version 1)
              │  host lifecycle + bounded I/O pumps
              ▼
internal wire: protocol/proto.h (exact-match v4)
              │
              ▼
guest PID 1 supervisor ── child processes / PTY / chroot
              │
              ▼
fakefs RootFS (independently sourced and installed)
```

Swift objects are not a second runtime. They wrap C handles. The C layer manages
threads, session tables, queues, and framing. Guest PID 1 manages Linux children,
PTYs, signals, and reaping. fakefs persists the guest filesystem.

`third_party/ish` is the project fork pinned to an exact revision, not a direct
edit of somebody else's upstream worktree. Embedded lifecycle and JIT coherence
must enter iSH core and cannot be implemented by the outer package alone. The
fork carries narrow differences through independent PRs and CI while generic
fixes can still go upstream. See the
[architecture guide](architecture.en.md#layers-and-responsibilities).

## Stage1 invariants

- One valid instance lifecycle is supported per host process.
- A C session handle returned by `ish_embed_spawn` owns one reference. New
  retain/release entry points let native calls borrow it safely. Stage1 Swift
  does not use native borrowing; its v0.3.3-compatible call gate detaches,
  rejects new calls, waits for admitted calls, and then closes.
- The call gate prevents close/call UAF but cannot cancel a permanently blocked
  non-read C call. Callers should still stop and await tasks first.
- Shutdown succeeds only after every session is closed and active instance calls
  such as spawn/runOneshot end. Swift restores running after `BUSY`; other
  failures quarantine ordinary calls but retain shutdown cleanup retry. Success
  enters a consumed terminal state and rejects another boot.
- Host and supervisor must both use wire v4; there is no cross-version negotiation.
- Before default supervisor installation, SHA-256 over the actual embedded bytes
  must match same-build metadata and the content-addressed path. A custom path
  bypasses that default gate. Digest equality is not a digital signature or
  provenance authentication.
- JIT single-page writes and explicit `invalidate_page` use exact-page filtering;
  only a multi-page hashed bitmap can conservatively over-invalidate on collision.
  A direct-chain or RET target returns to the dispatcher only when it intersects
  pending dirty code pages; data-only writes may keep chaining.
- RootFS is outside the package/Release and must not enter Corresponding Source.
- The current `v0.4.0-abi.6` source change carries no RootFS or prebuilt binary;
  a later release transaction must produce the XCFramework.

## Authoritative sources

| Content | Authoritative file |
| --- | --- |
| Public C ABI and status codes | [`include/ishembed.h`](../include/ishembed.h) |
| Host ↔ supervisor framing | [`protocol/proto.h`](../protocol/proto.h) |
| Swift API | [`Sources/IshEmbed/`](../Sources/IshEmbed/) |
| Current platform and binary URL/checksum | [`Package.swift`](../Package.swift) |
| Native build | [`scripts/build-ios.sh`](../scripts/build-ios.sh) |
| Release transaction | [`scripts/release.sh`](../scripts/release.sh) |

If documentation disagrees with these sources, follow the authoritative file
and fix both language mirrors in the same change.
