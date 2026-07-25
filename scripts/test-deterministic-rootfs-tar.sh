#!/usr/bin/env bash

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
ARCHIVER="$PKG_ROOT/scripts/create-deterministic-tar.py"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-rootfs-tar.XXXXXX")"

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    rm -rf -- "$TEST_ROOT"
    exit "$rc"
}
trap cleanup EXIT INT TERM HUP

for tree in first second; do
    mkdir -p "$TEST_ROOT/$tree/bin" "$TEST_ROOT/$tree/etc"
    printf '#!/bin/sh\nprintf ok\n' > "$TEST_ROOT/$tree/bin/tool"
    chmod 0755 "$TEST_ROOT/$tree/bin/tool"
    printf 'fixture\n' > "$TEST_ROOT/$tree/etc/config"
    ln "$TEST_ROOT/$tree/etc/config" "$TEST_ROOT/$tree/etc/config-hardlink"
    ln -s ../etc/config "$TEST_ROOT/$tree/bin/config-link"
done

touch -t 202101010101 "$TEST_ROOT/first" "$TEST_ROOT/first/bin/tool"
touch -t 202512312359 "$TEST_ROOT/second" "$TEST_ROOT/second/bin/tool"

"$ARCHIVER" \
    --source "$TEST_ROOT/first" \
    --arcname fs \
    --output "$TEST_ROOT/first.tar.gz" \
    --mtime 1704067200
"$ARCHIVER" \
    --source "$TEST_ROOT/second" \
    --arcname fs \
    --output "$TEST_ROOT/second.tar.gz" \
    --mtime 1704067200

cmp "$TEST_ROOT/first.tar.gz" "$TEST_ROOT/second.tar.gz"

python3 - "$TEST_ROOT/first.tar.gz" <<'PY'
import gzip
import pathlib
import stat
import sys
import tarfile

archive_path = pathlib.Path(sys.argv[1])
header = archive_path.read_bytes()[:10]
if len(header) != 10 or int.from_bytes(header[4:8], "little") != 1704067200:
    raise SystemExit("gzip timestamp was not normalized")

with tarfile.open(archive_path, "r:gz") as archive:
    members = {member.name: member for member in archive.getmembers()}
    expected = {
        "fs",
        "fs/bin",
        "fs/bin/config-link",
        "fs/bin/tool",
        "fs/etc",
        "fs/etc/config",
        "fs/etc/config-hardlink",
    }
    if set(members) != expected:
        raise SystemExit("deterministic archive has an unexpected member set")
    for member in members.values():
        if (
            member.uid != 0
            or member.gid != 0
            or member.uname != "root"
            or member.gname != "root"
            or member.mtime != 1704067200
        ):
            raise SystemExit("archive ownership or timestamp is not normalized")
    if not members["fs/bin/config-link"].issym():
        raise SystemExit("symbolic link was not preserved")
    if not members["fs/etc/config-hardlink"].islnk():
        raise SystemExit("hard link was not preserved")
    if stat.S_IMODE(members["fs/bin/tool"].mode) != 0o755:
        raise SystemExit("executable mode was not preserved")
PY

set +e
"$ARCHIVER" \
    --source "$TEST_ROOT/first" \
    --arcname fs \
    --output "$TEST_ROOT/first.tar.gz" \
    --mtime 1704067200 > "$TEST_ROOT/existing.log" 2>&1
existing_rc=$?
set -e
[[ "$existing_rc" -ne 0 ]] || {
    printf 'archiver overwrote an existing output\n' >&2
    exit 1
}

ln -s first "$TEST_ROOT/source-link"
set +e
"$ARCHIVER" \
    --source "$TEST_ROOT/source-link" \
    --arcname fs \
    --output "$TEST_ROOT/link.tar.gz" \
    --mtime 1704067200 > "$TEST_ROOT/link.log" 2>&1
link_rc=$?
set -e
[[ "$link_rc" -ne 0 ]] || {
    printf 'archiver accepted a symlink source\n' >&2
    exit 1
}

printf 'deterministic RootFS tar tests passed\n'
