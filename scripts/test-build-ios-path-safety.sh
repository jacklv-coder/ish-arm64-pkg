#!/usr/bin/env bash

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_SCRIPT="$PKG_ROOT/scripts/build-ios.sh"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-build-path.XXXXXX")"

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    rm -rf "$TEST_ROOT"
    exit "$rc"
}
trap cleanup EXIT INT TERM HUP

expect_path_rejected() {
    local description="$1"
    local build_dir="$2"
    local out_dir="$3"
    local log="$TEST_ROOT/rejected-path.log"
    local rc

    set +e
    BUILD_DIR="$build_dir" OUT_DIR="$out_dir" \
        "$BUILD_SCRIPT" >"$log" 2>&1
    rc=$?
    set -e

    [[ "$rc" == 64 ]] || {
        printf '%s returned %s instead of path-policy status 64\n' \
            "$description" "$rc" >&2
        sed -n '1,80p' "$log" >&2
        return 1
    }
    grep -q '^unsafe \(BUILD_DIR\|OUT_DIR\):' "$log" || {
        printf '%s did not report the rejected build path\n' "$description" >&2
        sed -n '1,80p' "$log" >&2
        return 1
    }
}

# If an unsafe OUT_DIR were split by `rm -rf $(OUT_DIR)`, the first word would
# be this victim directory. The build must fail before Make runs and leave the
# sentinel intact.
VICTIM_DIR="$TEST_ROOT/victim"
mkdir -p "$VICTIM_DIR"
touch "$VICTIM_DIR/sentinel"
SPACE_BUILD_DIR="$VICTIM_DIR suffix"
expect_path_rejected \
    "space-containing BUILD_DIR" \
    "$SPACE_BUILD_DIR" \
    "$SPACE_BUILD_DIR/xcframework"
[[ -f "$VICTIM_DIR/sentinel" ]] \
    || { printf 'space-containing path deleted the sentinel\n' >&2; exit 1; }

SAFE_BUILD_DIR="$TEST_ROOT/safe-build"
SHELL_DANGER_OUT_DIR="$TEST_ROOT/xcframework"'$unsafe'
expect_path_rejected \
    "shell-metacharacter OUT_DIR" \
    "$SAFE_BUILD_DIR" \
    "$SHELL_DANGER_OUT_DIR"

CONTROL_BUILD_DIR="$TEST_ROOT/control"$'\n'"path"
expect_path_rejected \
    "control-character BUILD_DIR" \
    "$CONTROL_BUILD_DIR" \
    "$TEST_ROOT/safe-output"

LEADING_DASH_SENTINEL="$TEST_ROOT/leading-dash-sentinel"
touch "$LEADING_DASH_SENTINEL"
expect_path_rejected \
    "leading-dash BUILD_DIR" \
    "-unsafe-build" \
    "$TEST_ROOT/safe-output"
[[ -f "$LEADING_DASH_SENTINEL" ]] \
    || { printf 'leading-dash path deleted the sentinel\n' >&2; exit 1; }

# Exercise standalone `make clean OUT_DIR=//` without ever putting the real rm
# in its PATH. The slash-only guard must reject the command before even the fake
# rm is invoked; this makes the regression safe to run if the guard changes.
command -v make >/dev/null 2>&1 \
    || { printf 'required command not found: make\n' >&2; exit 1; }
FAKE_BIN="$TEST_ROOT/fake-bin"
ROOT_GUARD_SENTINEL="$TEST_ROOT/slash-only-sentinel"
FAKE_RM_CALLED="$TEST_ROOT/slash-only-rm-called"
mkdir -p "$FAKE_BIN"
touch "$ROOT_GUARD_SENTINEL"
printf '%s\n' \
    '#!/bin/sh' \
    ': > "$ISHEMBED_PATH_TEST_RM_CALLED"' \
    'exit 99' \
    > "$FAKE_BIN/rm"
chmod +x "$FAKE_BIN/rm"
set +e
PATH="$FAKE_BIN:$PATH" \
ISHEMBED_PATH_TEST_RM_CALLED="$FAKE_RM_CALLED" \
    make -C "$PKG_ROOT/supervisor" OUT_DIR=// clean \
    >"$TEST_ROOT/slash-only-clean.log" 2>&1
SLASH_CLEAN_RC=$?
set -e
[[ "$SLASH_CLEAN_RC" != 0 ]] \
    || { printf 'slash-only OUT_DIR was accepted by make clean\n' >&2; exit 1; }
[[ ! -e "$FAKE_RM_CALLED" && -f "$ROOT_GUARD_SENTINEL" ]] \
    || { printf 'slash-only OUT_DIR reached rm or changed its sentinel\n' >&2; exit 1; }
grep -q 'refusing to clean slash-only OUT_DIR' \
    "$TEST_ROOT/slash-only-clean.log" \
    || { printf 'slash-only OUT_DIR did not report the clean guard\n' >&2; exit 1; }

printf 'build-ios path safety tests passed.\n'
