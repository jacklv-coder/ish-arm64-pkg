# IshEmbed

Embeddable Linux runtime for iOS host apps, built on top of
[iSH](https://github.com/ish-app/ish) (x86 + arm64 usermode emulator,
syscall layer, fakefs).

Lets a SwiftUI / UIKit app boot a Linux userland, run commands
(persistent shells or one-shot), pipe stdin/stdout, send Ctrl+C, and —
unique to this fork — host **multiple isolated VMs** in the same
process via chroot containment.

## Install

In Xcode: **File → Add Package Dependencies…** with URL

```
https://github.com/Lolendor/ish-arm64-pkg
```

The package's `IshKernel` target is a binary xcframework attached to
the GitHub Release; SwiftPM downloads it automatically. No Meson, no
Zig, no LLVM toolchain on the consumer's machine.

## Quick start

```swift
import IshEmbed

// 1. Extract the bundled rootfs tarball into a writable sandbox.
//    (We don't ship one inside the package — the tarball is on the
//    Release page; consumers bundle it into their app's resources.)
let rootfs = try myAppExtractedRootfsURL()

// 2. Boot the kernel once.
try IshInstance.shared.boot(.init(rootfsPath: rootfs.path))

// 3. Per-VM commands are chrooted into /srv/vms/<name>/.
//    A "default" VM is created on first launch.
let vm = try IshInstance.shared.ensureDefaultVM()

let r = try IshInstance.shared.runOneshot(
    .init(argv: ["/bin/echo", "hi"], chrootPath: vm.guestPath)
)
print(String(decoding: r.stdoutData, as: UTF8.self))   // "hi\n"

// 4. Streaming session with stdin pipe, Ctrl+C, etc.
let s = try IshInstance.shared.spawn(in: vm,
    .init(argv: ["/bin/sh"]))
try s.write(Data("ls /\n".utf8))
loop: while true {
    switch try s.read(timeout: 5) {
    case .data(let d, _, _): FileHandle.standardOutput.write(d)
    case .exited:            break loop
    }
}
try s.interrupt()                  // SIGINT (Ctrl+C)
```

See [docs/architecture.md](docs/architecture.md) for the wire protocol,
boot flow, and patches we maintain on the iSH fork.

## Layout

```
ish-arm64-pkg/
├── Package.swift
├── Sources/
│   ├── CIshEmbed/                     ← C ABI module (modulemap + header)
│   └── IshEmbed/                      ← Swift API
│       ├── IshEmbed.swift             ← IshInstance, IshSession
│       └── IshVM.swift                ← multi-VM helpers
│
├── include/ishembed.h                 ← canonical C ABI (vendored to CIshEmbed)
├── protocol/proto.h                   ← wire protocol between host and supervisor
├── ffi/                               ← FFI shims into iSH internals
├── host/                              ← reader/writer pumps, sessions, lifecycle
├── supervisor/                        ← PID 1 (i386/arm64 musl static ELF)
├── meson.build / meson_options.txt    ← native build glue
├── scripts/
│   ├── build-rootfs.sh                ← Alpine + supervisor injection
│   ├── build-ios.sh                   ← xcframework
│   └── ios-arm64.cross.ini
│
├── c-tests/                           ← protocol unit + host smoke
├── Tests/IshEmbedTests/
│
└── third_party/ish/                   ← submodule → Lolendor/ish-arm64
```

## Building artifacts (maintainers only)

Consumers don't need this — the GitHub Release ships everything
prebuilt. You only need this section if you want to regenerate the
xcframework.

```sh
brew install meson ninja sqlite libarchive llvm lld zig
git submodule update --init
scripts/build-rootfs.sh
PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH" \
  scripts/build-ios.sh
# Output:
#   build/fs.tar.gz                              ← bundle into your app
#   build/xcframework/libIshKernel.xcframework   ← uploaded to Release
```

To cut a release:

```sh
scripts/release.sh v0.1.0
# zips xcframework, computes sha256, uploads via gh release create,
# and patches Package.swift's checksum field on the tag commit.
```

## License

iSH is GPL-3.0; everything in this repo follows the same license.
