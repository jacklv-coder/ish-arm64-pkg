#!/usr/bin/env bash
# Build and run the Swift package tests against the XCFramework produced by
# this checkout. The default mode deliberately replaces the release URL in an
# isolated package copy. `--manifest-binary` leaves Package.swift untouched and
# verifies that its published binaryTarget links the same Swift API; this is the
# phase boundary gate for native-first, Swift-second releases.

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
TEST_MODE=local

if [[ "${1:-}" == "--manifest-binary" ]]; then
    TEST_MODE=manifest
    shift
elif [[ "${1:-}" == "--local-binary" ]]; then
    TEST_MODE=local
    shift
elif [[ "${1:-}" == --* ]]; then
    printf 'test-swift-ios: unknown option: %s\n' "$1" >&2
    exit 1
fi

if [[ "$TEST_MODE" == manifest && $# -ne 0 ]]; then
    printf 'test-swift-ios: --manifest-binary does not accept an XCFramework path\n' >&2
    exit 1
fi
if [[ "$TEST_MODE" == local && $# -gt 1 ]]; then
    printf 'test-swift-ios: usage: %s [--local-binary [XCFRAMEWORK] | XCFRAMEWORK | --manifest-binary]\n' "$0" >&2
    exit 1
fi

XCF_INPUT="${1:-$PKG_ROOT/build/xcframework/libIshKernel.xcframework}"

fail() {
    printf 'test-swift-ios: %s\n' "$*" >&2
    exit 1
}

[[ "$(uname -s)" == Darwin ]] || fail "requires macOS and Xcode"
[[ "$(uname -m)" == arm64 ]] \
    || fail "requires an arm64 macOS host for the arm64 simulator slice"
for command_name in swift xcodebuild xcrun python3 mktemp; do
    command -v "$command_name" >/dev/null \
        || fail "required command not found: $command_name"
done

if [[ "$TEST_MODE" == local ]]; then
    [[ -d "$XCF_INPUT" ]] || fail "XCFramework not found: $XCF_INPUT"
    XCF_DIR="$(cd "$XCF_INPUT" && pwd -P)"
    [[ -f "$XCF_DIR/Info.plist" ]] || fail "XCFramework is missing Info.plist"
    [[ -f "$XCF_DIR/ios-arm64/libIshKernel.a" ]] \
        || fail "XCFramework is missing the arm64 device library"
    [[ -f "$XCF_DIR/ios-arm64-simulator/libIshKernel.a" ]] \
        || fail "XCFramework is missing the arm64 simulator library"
fi

STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-swift-ios.XXXXXX")"
STAGING_DIR="$(cd "$STAGING_DIR" && pwd -P)"
PACKAGE_DIR="$STAGING_DIR/package"
DERIVED_DATA="$STAGING_DIR/DerivedData"
DEVICE_LIST="$STAGING_DIR/devices.json"
TEST_LOG="$STAGING_DIR/xcodebuild.log"

cleanup() {
    rm -rf "$STAGING_DIR"
}
trap cleanup EXIT INT TERM HUP

mkdir -p "$PACKAGE_DIR"
cp "$PKG_ROOT/Package.swift" "$PACKAGE_DIR/Package.swift"
cp -R "$PKG_ROOT/Sources" "$PACKAGE_DIR/Sources"
cp -R "$PKG_ROOT/Tests" "$PACKAGE_DIR/Tests"
cp -R "$PKG_ROOT/include" "$PACKAGE_DIR/include"
if [[ "$TEST_MODE" == local ]]; then
    cp -R "$XCF_DIR" "$PACKAGE_DIR/libIshKernel.xcframework"
fi

if [[ "$TEST_MODE" == local ]]; then
    python3 - "$PACKAGE_DIR/Package.swift" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text(encoding="utf-8")
pattern = re.compile(
    r'\.binaryTarget\(\s*name:\s*"IshKernel",\s*url:\s*"[^"]+"'
    r'\s*,\s*checksum:\s*"[^"]+"\s*\)',
    re.DOTALL,
)
replacement = (
    '.binaryTarget(\n'
    '            name: "IshKernel",\n'
    '            path: "libIshKernel.xcframework"\n'
    '        )'
)
updated, count = pattern.subn(replacement, source, count=1)
if count != 1:
    raise SystemExit("could not uniquely replace the IshKernel binary target")
path.write_text(updated, encoding="utf-8")
PY
fi

swift package --package-path "$PACKAGE_DIR" dump-package >/dev/null

xcrun simctl list -j devices available > "$DEVICE_LIST"
SIMULATOR_UDID="$(python3 - "$DEVICE_LIST" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    payload = json.load(handle)

candidates = []
for runtime, devices in payload.get("devices", {}).items():
    match = re.search(r"\.iOS-(\d+)-(\d+)(?:-(\d+))?$", runtime)
    if not match:
        continue
    version = tuple(int(part or 0) for part in match.groups())
    if version < (18, 0, 0):
        continue
    for device in devices:
        if device.get("isAvailable", True) and not device.get("availabilityError"):
            candidates.append((version, device.get("name", ""), device.get("udid", "")))

candidates = [candidate for candidate in candidates if candidate[2]]
if not candidates:
    raise SystemExit("no available iOS 18+ simulator was found")
print(sorted(candidates)[0][2])
PY
)"
[[ -n "$SIMULATOR_UDID" ]] || fail "could not select an iOS simulator"

if [[ "$TEST_MODE" == local ]]; then
    printf '==> Testing Swift API with local XCFramework on simulator %s\n' \
        "$SIMULATOR_UDID"
else
    printf '==> Testing Swift API with Package.swift binaryTarget on simulator %s\n' \
        "$SIMULATOR_UDID"
fi
(
    cd "$PACKAGE_DIR"
    unset ISH_EMBED_ROOTFS
    xcodebuild test \
        -scheme IshEmbed \
        -destination "platform=iOS Simulator,id=$SIMULATOR_UDID,arch=arm64" \
        -destination-timeout 120 \
        -derivedDataPath "$DERIVED_DATA" \
        CODE_SIGNING_ALLOWED=NO \
        ONLY_ACTIVE_ARCH=YES \
        ARCHS=arm64 \
        SWIFT_STRICT_CONCURRENCY=complete \
        SWIFT_TREAT_WARNINGS_AS_ERRORS=YES \
        2>&1 | tee "$TEST_LOG"
)

python3 - "$TEST_LOG" <<'PY'
import pathlib
import re
import sys

log = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
summaries = re.findall(
    r"Executed ([0-9]+) tests?, with (?:(\d+) tests? skipped and )?",
    log,
)
if not summaries:
    raise SystemExit("xcodebuild succeeded without a recognizable test summary")
executed, skipped = (int(value or 0) for value in summaries[-1])
if executed <= skipped:
    raise SystemExit("all reported tests were skipped")
print(f"    Executed {executed} tests ({skipped} skipped, {executed - skipped} ran)")
PY

if [[ "$TEST_MODE" == local ]]; then
    printf 'Swift iOS simulator tests passed with the local XCFramework.\n'
else
    printf 'Swift iOS simulator tests passed with the Package.swift binaryTarget.\n'
fi
