# IshEmbed release guide

[简体中文](releasing.md) | English

This guide is for maintainers publishing the XCFramework and matching
Corresponding Source. `v0.4.0-abi.9` is already public; the only next tag
allowed for preparation is the compatible maintenance release
`v0.4.0-abi.10`. It remains an ABI-transition prerelease that is **not stable
v0.4.0**. Publication
creates a public GitHub Release and updates the default branch, so run it only
with explicit release authorization.

## State before and after `v0.4.0-abi.10` publication

### After the maintenance PR merges, before Release publication

- `Package.swift` still pins the published `v0.4.0-abi.9` URL/checksum;
- Swift source remains v0.3.3-ABI compatible and does not call retain/release;
- repository source contains the published abi.2 procfs/task lifecycle fixes,
  abi.3 guest `uname` field bounds, abi.4 internal-SIGUSR1 fix, abi.5 finite
  streaming control-path deadline, abi.6 Swift-marshalling/stdin-close deadline
  reuse, abi.7 guest-atomic no-replace rename, abi.8 finite stdin-write
  deadline reuse, abi.9 per-call stdin write/close timeout APIs, and the pending
  abi.10 forced task-teardown/address-space lifetime fix;
- there is no installable `v0.4.0-abi.10` binary.

This intermediate state is intentional: the default branch never advertises an
unpublished asset URL that returns 404.

### After successful `v0.4.0-abi.10` publication

- the release commit changes only `Package.swift`, pinning the new XCFramework
  URL/checksum;
- the GitHub prerelease contains `libIshKernel.xcframework.zip` and
  `IshEmbed-corresponding-source.tar.gz`;
- the public C ABI is still 1 and internal wire is still v4;
- Swift is still the old-ABI-compatible layer; the complete Swift lifecycle,
  typed statuses, and Terminal/VT changes remain Stage2;
- RootFS remains outside all release assets.

This maintenance release does not implement a native Agent Loop or install/run
Codex CLI in the app. Node.js/npm remain optional packages of the RootFS/guest
package-management flow, not runtime requirements.

## Prerequisites

1. Run on the default branch with a clean worktree exactly matching the SSH
   remote's default branch.
2. Fetch and push URLs must be GitHub SSH. The script rejects HTTPS and a
   mismatched repository.
3. `gh auth status --hostname github.com` must succeed with permission to create
   Releases/tags and update the default branch.
4. Install `git`, `gh`, `swift`, `zip`, `shasum`, `python3`, `curl`, `zig`,
   Meson, Ninja, LLVM/lld, and other build prerequisites.
5. Initialize `third_party/ish`; the parent gitlink must name the reviewed revision.
6. Codex CR must report no P1/P2, and CI, iOS 18 real links, native sanitizers,
   documentation, and supply-chain gates must pass.
7. Explicitly confirm that this run publishes runtime/Corresponding Source only,
   never RootFS.

## Local pre-publication gates

```sh
git diff --check
bash -n scripts/*.sh
scripts/check-docs.sh
scripts/test-check-docs.sh
scripts/test-release-version-policy.sh
scripts/test-release-tag-cas.sh
scripts/test-source-policy.sh
scripts/run-host-tests.sh
scripts/test-swift-ios.sh --manifest-binary
PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH" \
  scripts/build-ios.sh
scripts/verify-ios-artifact.sh
scripts/test-swift-ios.sh --local-binary
```

`--manifest-binary` proves that Stage1 Swift still links the currently pinned
`v0.4.0-abi.9` binary. `--local-binary` proves that the same Swift source links
the maintenance XCFramework. Both boundaries are required.

## Execute

After confirming that the tag is absent and publication is authorized:

```sh
scripts/release.sh v0.4.0-abi.10
```

The script derives GitHub `prerelease=true` from the SemVer suffix. Only
`v*-abi.*` tags receive the dedicated bilingual transition notice. It states
that this is not stable v0.4, describes native lifecycle/retain-release/
join-soft-halt/wire v4, and records the Swift and RootFS boundaries.
In addition to strict SemVer validation, the Stage1 policy rejects every tag
except `v0.4.0-abi.10`. Reusing `v0.4.0-abi.9` or accidentally entering
`v0.4.0` therefore fails before any tag, draft, or asset is written.

Do not substitute `v0.4.0`. A stable tag must wait for a separate decision after
Stage2 integration, migration, and regression testing.

## What the transaction does

1. Validate strict SemVer, SSH remote, GitHub auth, clean default branch, and
   remote head/tag/Release state.
2. Run iOS 18 real-link/XCTest against the manifest binary.
3. Rebuild the XCFramework from the fixed commit/submodule in an isolated
   worktree. The same `verify-ios-artifact.sh` used by CI binds the paths actually
   selected by Info.plist, checks every Mach-O member of both arm64 slices for
   platform and minos, performs warning-free iOS 18 final links, checks ABI symbols, and
   proves byte-for-byte supervisor identity from ELF through generated array and
   both linked slices, along with hash/path and licenses, before XCTest uses the
   local binary.
4. Compute SwiftPM/SHA-256, update only `Package.swift` URL/checksum in the
   isolated worktree, and create the release commit. The primary worktree is untouched.
5. Package Corresponding Source from the release commit, recording the parent,
   pinned iSH, Zig, and musl inputs, and apply RootFS/nested-archive rejection policy.
6. Push the release commit to a unique staging ref, create a private draft,
   upload exactly two assets, and verify count, size, digest, and state.
7. Create a unique annotated final tag with an absent-ref lease/CAS, then attach
   the verified draft to the final tag and publish the prerelease.
8. Download both public URLs again and verify their checksums.
9. Only then fast-forward the default branch to the manifest-only release commit,
   avoiding a branch-visible 404, and clean up staging objects with proven ownership.

## Assets and licensing

The Release must contain exactly:

| Asset | Content |
| --- | --- |
| `libIshKernel.xcframework.zip` | device/simulator arm64 runtime and embedded supervisor |
| `IshEmbed-corresponding-source.tar.gz` | parent repository, pinned iSH revision, musl, and other source needed to rebuild the binary |

RootFS is neither the binary nor Corresponding Source. The script never reads or
uploads it, and source policy rejects likely RootFS content by path, name, magic,
and nested-archive inspection. PocketRoot independently owns RootFS provenance,
hashing, licensing, and distribution.

`scripts/prepare-rootfs-candidate.sh` is a separate local RootFS double-build
gate, not part of this XCFramework publication transaction. `--verify-only`
retains and uploads no artifact; `--output` creates only an
outside-the-repository local candidate explicitly marked as unapproved for
distribution, plus a `ROOTFS_BUILD_ENVIRONMENT.json` host/tool/linked-library receipt
that does not enter the RootFS content identity. Any RootFS GitHub Release
still requires its own version policy,
complete LICENSE/NOTICE, corresponding source, SBOM, PocketRoot manifest update,
and explicit owner authorization.

## Post-publication acceptance

```sh
gh release view v0.4.0-abi.10 --repo jacklv-coder/ish-arm64-pkg
git fetch origin --tags
git show v0.4.0-abi.10:Package.swift
git pull --ff-only origin main
scripts/test-swift-ios.sh --manifest-binary
```

Also confirm:

- the Release is marked prerelease and includes the bilingual ABI-transition notice;
- the final tag targets the release commit, which changes only `Package.swift`;
- both assets are public and their digests match release output;
- the manifest URL uses the same tag and checksum matches the public zip;
- `ISH_EMBED_ABI_VERSION` remains 1 and the binary exports retain/release and
  required join/soft-halt symbols;
- no RootFS is present.

## Failure and recovery

The script follows “delete only objects whose ownership can be proven”:

- ordinary failures before publication clean the isolated worktree/temp directory;
- failures after draft verification preserve the draft, staging ref, and local assets;
- an uncertain final-tag push causes no remote deletion;
- after the publication PATCH is attempted, a network failure is ambiguous and
  the script does not delete the tag or Release. Query Release id, tag raw
  OID/peeled commit, assets, and default branch first;
- if the Release is public but default-branch fast-forward fails, do not reuse
  the tag. Verify assets, then safely fast-forward to the existing release commit.

Never force-push over unknown objects or delete a tag/draft by name alone. Keep
the printed staging path, Release id, commit, tag-object OID, and digests for
manual recovery.

## PocketRoot upgrade gate

PocketRoot may move its dependency from `v0.4.0-abi.9` to the maintenance
release only after the public `v0.4.0-abi.10` assets, manifest update, and
post-publication real link all pass, followed by its Xcode 16/iOS 18 gates.
Stage2 and a native Agent Loop are outside this release. Either still requires
an independent plan, review, tests, documentation, and release decision.
