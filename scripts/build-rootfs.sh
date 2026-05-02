#!/usr/bin/env bash
# Build a fakefs-encoded rootfs that contains:
#   - Alpine i386 minirootfs (busybox + apk)
#   - /sbin/ishsv (PID1 supervisor) installed via fakefsify so metadata is valid
#
# Output: build/fs/                 (fakefs directory tree: data/ + meta.db)
#         build/fs.tar.gz           (tarball ready to bundle into the host app)
#         build/SHA256SUMS
#
# Usage:
#   embed/scripts/build-rootfs.sh
#
# Prereqs:
#   - bash, curl, sha256sum, tar
#   - host with `fakefsify` available (built from third_party/ish/tools)
#   - i486-linux-musl-gcc (or override CC) to build the supervisor
#
# Rootfs version is pinned. Update ALPINE_VERSION carefully — newer Alpines
# sometimes ship libc symbols not implemented by the iSH x86 emulator.

set -euo pipefail

ALPINE_VERSION="${ALPINE_VERSION:-3.19.1}"
ALPINE_ARCH="${ALPINE_ARCH:-aarch64}"   # iSH-arm64 fork expects guest_arch=arm64
ALPINE_MAJOR="${ALPINE_VERSION%.*}"
ALPINE_BASE="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/releases/${ALPINE_ARCH}"
ALPINE_TGZ="alpine-minirootfs-${ALPINE_VERSION}-${ALPINE_ARCH}.tar.gz"
ALPINE_SHA256="${ALPINE_SHA256:-}"   # if known; we'll compute and record otherwise

# PKG_ROOT is the root of this Swift Package repo (where Package.swift lives).
PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISH_SRC="${ISH_SRC:-$PKG_ROOT/third_party/ish}"
BUILD_DIR="${BUILD_DIR:-$PKG_ROOT/build}"
ROOTFS_OUT="$BUILD_DIR/fs"
TARBALL_OUT="$BUILD_DIR/fs.tar.gz"
SUMS_OUT="$BUILD_DIR/SHA256SUMS"

mkdir -p "$BUILD_DIR" "$BUILD_DIR/dl" "$BUILD_DIR/fakefsify"

echo "==> [1/5] Downloading Alpine ${ALPINE_VERSION} i386 minirootfs"
ALPINE_PATH="$BUILD_DIR/dl/$ALPINE_TGZ"
if [[ ! -f "$ALPINE_PATH" ]]; then
    curl -fL --retry 3 -o "$ALPINE_PATH.tmp" "$ALPINE_BASE/$ALPINE_TGZ"
    mv "$ALPINE_PATH.tmp" "$ALPINE_PATH"
fi
ACTUAL_SHA="$(shasum -a 256 "$ALPINE_PATH" | awk '{print $1}')"
echo "    sha256: $ACTUAL_SHA"
if [[ -n "$ALPINE_SHA256" && "$ACTUAL_SHA" != "$ALPINE_SHA256" ]]; then
    echo "ERROR: sha256 mismatch (expected $ALPINE_SHA256)" >&2
    exit 1
fi

echo "==> [2/5] Building fakefsify (host tool)"
# fakefsify is part of iSH tools/. Build with meson if not already.
FAKEFSIFY_BIN=""
if command -v fakefsify >/dev/null 2>&1; then
    FAKEFSIFY_BIN="$(command -v fakefsify)"
elif [[ -x "$BUILD_DIR/ish-host/tools/fakefsify" ]]; then
    FAKEFSIFY_BIN="$BUILD_DIR/ish-host/tools/fakefsify"
else
    if ! command -v meson >/dev/null; then
        echo "ERROR: meson required to build fakefsify" >&2
        exit 1
    fi
    meson setup --reconfigure "$BUILD_DIR/ish-host" "$ISH_SRC" >/dev/null
    meson compile -C "$BUILD_DIR/ish-host" fakefsify
    FAKEFSIFY_BIN="$BUILD_DIR/ish-host/tools/fakefsify"
fi
echo "    using $FAKEFSIFY_BIN"

echo "==> [3/5] Building i386 musl supervisor"
make -C "$PKG_ROOT/supervisor" \
    OUT_DIR="$BUILD_DIR/supervisor"
SUP_BIN="$BUILD_DIR/supervisor/ishsv"
if [[ ! -f "$SUP_BIN" ]]; then
    echo "ERROR: supervisor not built" >&2
    exit 1
fi

echo "==> [4/5] Importing Alpine into fakefs"
rm -rf "$ROOTFS_OUT"
"$FAKEFSIFY_BIN" "$ALPINE_PATH" "$ROOTFS_OUT"
# fakefsify creates ROOTFS_OUT itself and writes data/ + meta.db inside.

echo "==> [4b/5] Injecting /sbin/ishsv into fakefs (via host tarball reimport)"
# The robust way to add a file with correct fakefs metadata is to repack
# the source tarball with the supervisor included, then re-fakefsify.
# Cheaper alternative: use the iSH kernel's install_executable at boot
# time. We do BOTH paths here:
#
#   - Primary: copy supervisor into data/sbin/ishsv (fakefs data dir).
#   - Repair pass: fakefsify treats data/ as canonical, and meta.db is
#     populated by import; so we add the path to meta.db by repacking.
#
# We take the simpler, well-tested route: build a small tar containing
# only ./sbin/ishsv with mode 0755 and call fakefsify on the merged
# tarball. This guarantees consistent metadata.

WORK="$BUILD_DIR/dl/work-rootfs"
rm -rf "$WORK"
mkdir -p "$WORK"
tar -xzf "$ALPINE_PATH" -C "$WORK"
mkdir -p "$WORK/sbin"
cp "$SUP_BIN" "$WORK/sbin/ishsv"
chmod 755 "$WORK/sbin/ishsv"

# Pre-seed an inline VM template at /srv/vms/.template inside the
# rootfs. New VMs are created by `cp -a /srv/vms/.template /srv/vms/<n>`
# from inside the guest, which is much faster than re-extracting an
# Alpine tarball through the iSH x86 emulator.
mkdir -p "$WORK/srv/vms/.template"
tar -xzf "$ALPINE_PATH" -C "$WORK/srv/vms/.template"
# Patch the template's network config the same way as the base.
mkdir -p "$WORK/srv/vms/.template/etc"
cat > "$WORK/srv/vms/.template/etc/resolv.conf" <<'RESOLV'
nameserver 1.1.1.1
nameserver 8.8.8.8
nameserver 9.9.9.9
options timeout:2 attempts:2
RESOLV
cat > "$WORK/srv/vms/.template/etc/apk/repositories" <<APK
https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/main
https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/community
APK
echo "ish-vm" > "$WORK/srv/vms/.template/etc/hostname"

# --- DNS ---------------------------------------------------------------
# Alpine minirootfs ships without /etc/resolv.conf. iSH does not have
# DHCP, so apk + any networked tool needs explicit nameservers.
# We pin Cloudflare + Google + Quad9 + plain Cloudflare to maximise
# the chance that one is reachable from any cellular / Wi-Fi network.
mkdir -p "$WORK/etc"
cat > "$WORK/etc/resolv.conf" <<'RESOLV'
# Generated by embed/scripts/build-rootfs.sh — iSH has no DHCP,
# so we hardcode public resolvers. Edit at runtime if you prefer
# your own.
nameserver 1.1.1.1
nameserver 8.8.8.8
nameserver 9.9.9.9
options timeout:2 attempts:2
RESOLV

# Make sure /etc/hosts has the basics (Alpine ships this, but be safe).
if [[ ! -s "$WORK/etc/hosts" ]]; then
    cat > "$WORK/etc/hosts" <<'HOSTS'
127.0.0.1	localhost localhost.localdomain
::1		localhost localhost.localdomain ip6-localhost ip6-loopback
HOSTS
fi

# Sane default hostname so the shell prompt isn't blank.
echo "ish" > "$WORK/etc/hostname"

# Pin /etc/apk/repositories to https + main+community for the version
# we tested. Edge / dev versions have shipped libc symbols iSH can't
# emulate; if you need newer packages, try the same Alpine MINOR.
cat > "$WORK/etc/apk/repositories" <<APK
https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/main
https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/community
APK

MERGED_TGZ="$BUILD_DIR/dl/rootfs-with-supervisor.tar.gz"
tar -C "$WORK" -czf "$MERGED_TGZ" .

rm -rf "$ROOTFS_OUT"
"$FAKEFSIFY_BIN" "$MERGED_TGZ" "$ROOTFS_OUT"

echo "==> [5/5] Packaging tarball + checksums"
tar -C "$BUILD_DIR" -czf "$TARBALL_OUT" "$(basename "$ROOTFS_OUT")"
( cd "$BUILD_DIR" && shasum -a 256 "$(basename "$TARBALL_OUT")" "$(basename "$SUMS_OUT" 2>/dev/null || true)" 2>/dev/null || true ) >/dev/null
( cd "$BUILD_DIR" && shasum -a 256 "$(basename "$TARBALL_OUT")" ) > "$SUMS_OUT"

echo
echo "Done."
echo "  Rootfs dir: $ROOTFS_OUT"
echo "  Tarball:    $TARBALL_OUT"
echo "  Checksums:  $SUMS_OUT"
echo
echo "Bundle the tarball into your iOS app's Resources, extract it"
echo "into a writable sandbox dir at first launch, and pass that"
echo "dir to IshInstance.boot(rootfsPath:)."
