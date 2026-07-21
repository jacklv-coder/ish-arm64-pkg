#!/usr/bin/env bash

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VALIDATOR="$PKG_ROOT/scripts/validate-source-policy.py"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-source-policy.XXXXXX")"

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    rm -rf "$TEST_ROOT"
    exit "$rc"
}
trap cleanup EXIT INT TERM HUP

expect_rejected() {
    local fixture="$1"
    if python3 "$VALIDATOR" --tree "$fixture" >/dev/null 2>&1; then
        printf 'policy unexpectedly accepted %s\n' "$fixture" >&2
        return 1
    fi
}

mkdir -p "$TEST_ROOT/safe/scripts"
printf '%s\n' '# source-only RootFS build instructions' \
    > "$TEST_ROOT/safe/scripts/build-rootfs.sh"
python3 "$VALIDATOR" --tree "$TEST_ROOT/safe"

mkdir -p "$TEST_ROOT/rootfs-dir/rootfs/bin"
expect_rejected "$TEST_ROOT/rootfs-dir"

mkdir -p "$TEST_ROOT/fakefs-marker/data"
: > "$TEST_ROOT/fakefs-marker/meta.db"
expect_rejected "$TEST_ROOT/fakefs-marker"

mkdir -p "$TEST_ROOT/renamed-archive/assets"
mkdir -p "$TEST_ROOT/nested-archive-input"
printf '%s\n' 'not a RootFS, but still a nested container' \
    > "$TEST_ROOT/nested-archive-input/source.txt"
COPYFILE_DISABLE=1 tar -cf \
    "$TEST_ROOT/renamed-archive/assets/innocent-looking-source.dat" \
    -C "$TEST_ROOT/nested-archive-input" source.txt
expect_rejected "$TEST_ROOT/renamed-archive"

# The historical binary cpio header starts with 0x71c7 in either byte order.
# Keep both renamed fixtures small so this tests the content signature rather
# than the independent 16 MiB size limit.
mkdir -p "$TEST_ROOT/renamed-binary-cpio-be/assets"
printf '\161\307small-renamed-cpio-fixture' \
    > "$TEST_ROOT/renamed-binary-cpio-be/assets/innocent-source.dat"
expect_rejected "$TEST_ROOT/renamed-binary-cpio-be"

mkdir -p "$TEST_ROOT/renamed-binary-cpio-le/assets"
printf '\307\161small-renamed-cpio-fixture' \
    > "$TEST_ROOT/renamed-binary-cpio-le/assets/innocent-source.dat"
expect_rejected "$TEST_ROOT/renamed-binary-cpio-le"
[[ "$(wc -c < "$TEST_ROOT/renamed-binary-cpio-le/assets/innocent-source.dat" | tr -d '[:space:]')" -lt 16777216 ]] \
    || { printf 'binary cpio fixture unexpectedly reached the size limit\n' >&2; exit 1; }

RENAMED_BINARY_CPIO_ARCHIVE="$TEST_ROOT/renamed-binary-cpio-source.tar.gz"
COPYFILE_DISABLE=1 tar -czf "$RENAMED_BINARY_CPIO_ARCHIVE" \
    -C "$TEST_ROOT" renamed-binary-cpio-le
if python3 "$VALIDATOR" --archive "$RENAMED_BINARY_CPIO_ARCHIVE" \
    --prefix renamed-binary-cpio-le >/dev/null 2>&1; then
    printf 'policy unexpectedly accepted renamed binary cpio content in an archive\n' >&2
    exit 1
fi

mkdir -p "$TEST_ROOT/disk-image/assets"
: > "$TEST_ROOT/disk-image/assets/runtime.squashfs"
expect_rejected "$TEST_ROOT/disk-image"

mkdir -p "$TEST_ROOT/large-file/src"
truncate -s 16777217 "$TEST_ROOT/large-file/src/opaque.bin"
expect_rejected "$TEST_ROOT/large-file"

mkdir -p "$TEST_ROOT/renamed-database/data"
printf 'SQLite format 3\000not-a-real-database' \
    > "$TEST_ROOT/renamed-database/data/source-cache.bin"
expect_rejected "$TEST_ROOT/renamed-database"

RENAMED_CONTAINER_ARCHIVE="$TEST_ROOT/renamed-container-source.tar.gz"
COPYFILE_DISABLE=1 tar -czf "$RENAMED_CONTAINER_ARCHIVE" \
    -C "$TEST_ROOT" renamed-archive
if python3 "$VALIDATOR" --archive "$RENAMED_CONTAINER_ARCHIVE" \
    --prefix renamed-archive >/dev/null 2>&1; then
    printf 'policy unexpectedly accepted renamed container content in an archive\n' >&2
    exit 1
fi

SAFE_ARCHIVE="$TEST_ROOT/safe-source.tar.gz"
COPYFILE_DISABLE=1 tar -czf "$SAFE_ARCHIVE" -C "$TEST_ROOT" safe
python3 "$VALIDATOR" --archive "$SAFE_ARCHIVE" --prefix safe

printf 'Corresponding Source policy tests passed.\n'
