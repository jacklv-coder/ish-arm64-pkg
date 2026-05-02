#!/usr/bin/env bash
# Resolve Xcode toolchain for iOS arm64 builds and emit a usable cross-file.
#
# Why this script exists:
#   Inheriting CC, CFLAGS, SDKROOT, RUSTC_WRAPPER from the caller's shell
#   breaks Meson's host compiler probes — it tries to compile a tiny test
#   program for the host (macOS) using flags meant for the iOS target and
#   fails with cryptic "stdio.h not found" errors.
#
#   We scrub all toolchain env vars, then explicitly hand Meson a known-good
#   CC and SDKROOT via the cross-file.

set -euo pipefail

unset CC CXX CFLAGS CXXFLAGS LDFLAGS CPPFLAGS SDKROOT
unset RUSTC RUSTC_WRAPPER RUSTFLAGS

CC="$(xcrun --sdk iphoneos --find clang)"
SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)"
export ISH_IOS_CC="$CC"
export ISH_IOS_SDKROOT="$SDKROOT"

CROSS_TEMPLATE="$(cd "$(dirname "$0")" && pwd)/ios-arm64.cross.ini"
CROSS_OUT="${1:-/tmp/ios-arm64.cross.ini.resolved}"
sed "s|@ISH_IOS_SDKROOT@|$SDKROOT|g" "$CROSS_TEMPLATE" > "$CROSS_OUT"
echo "$CROSS_OUT"
