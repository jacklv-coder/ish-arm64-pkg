#!/usr/bin/env bash
# Cut a release: zip the xcframework, upload it (and fs.tar.gz) as
# release assets, compute the SHA-256, patch Package.swift to point at
# the new URL/checksum, commit, tag.
#
# Usage:  scripts/release.sh v0.1.0
#
# Prereqs:
#   - gh CLI authenticated
#   - build/xcframework/libIshKernel.xcframework already built (run
#     scripts/build-ios.sh first)
#   - build/fs.tar.gz already built (run scripts/build-rootfs.sh first)

set -euo pipefail

VERSION="${1:?usage: release.sh vX.Y.Z}"
[[ "$VERSION" == v* ]] || { echo "version must start with 'v'" >&2; exit 1; }

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PKG_ROOT/build"
XCF="$BUILD_DIR/xcframework/libIshKernel.xcframework"
FS_TGZ="$BUILD_DIR/fs.tar.gz"

[[ -d "$XCF" ]]    || { echo "missing $XCF — run scripts/build-ios.sh first" >&2; exit 1; }
[[ -f "$FS_TGZ" ]] || { echo "missing $FS_TGZ — run scripts/build-rootfs.sh first" >&2; exit 1; }

ZIP="$BUILD_DIR/libIshKernel.xcframework.zip"
echo "==> Zipping xcframework"
rm -f "$ZIP"
( cd "$BUILD_DIR/xcframework" && zip -qr "$ZIP" "libIshKernel.xcframework" )

echo "==> Computing SHA-256 (SwiftPM checksum)"
SUM="$(swift package compute-checksum "$ZIP")"
echo "    $SUM"

echo "==> Patching Package.swift"
URL="https://github.com/Lolendor/ish-arm64-pkg/releases/download/$VERSION/libIshKernel.xcframework.zip"
python3 - "$PKG_ROOT/Package.swift" "$URL" "$SUM" <<'PY'
import re, sys, pathlib
path, url, sum_ = sys.argv[1], sys.argv[2], sys.argv[3]
p = pathlib.Path(path)
src = p.read_text()
src = re.sub(r'url:\s*"https://github\.com/[^"]+"', f'url: "{url}"', src, count=1)
src = re.sub(r'checksum:\s*"[^"]+"', f'checksum: "{sum_}"', src, count=1)
p.write_text(src)
print(f"  url      = {url}")
print(f"  checksum = {sum_}")
PY

echo "==> Committing & tagging"
git -C "$PKG_ROOT" add Package.swift
git -C "$PKG_ROOT" commit -m "Release $VERSION" -m "url: $URL" -m "checksum: $SUM"
git -C "$PKG_ROOT" tag -a "$VERSION" -m "$VERSION"
git -C "$PKG_ROOT" push origin HEAD
git -C "$PKG_ROOT" push origin "$VERSION"

echo "==> Creating GitHub Release with assets"
gh -R Lolendor/ish-arm64-pkg release create "$VERSION" \
    --title "$VERSION" \
    --generate-notes \
    "$ZIP#libIshKernel.xcframework.zip" \
    "$FS_TGZ#fs.tar.gz"

echo
echo "Done."
echo "  Release: https://github.com/Lolendor/ish-arm64-pkg/releases/tag/$VERSION"
echo "  Consumers: Add Package Dependency → https://github.com/Lolendor/ish-arm64-pkg @ $VERSION"
