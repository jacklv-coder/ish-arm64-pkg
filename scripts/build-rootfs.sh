#!/usr/bin/env bash
# Build a fakefs-encoded rootfs that contains:
#   - Alpine aarch64 minirootfs (busybox + apk)
#   - /sbin/ishsv as a compatibility fallback for source builds without the
#     XCFramework-bundled, content-addressed PID 1 supervisor
#
# Output: build/fs/                 (fakefs directory tree: data/ + meta.db)
#         build/fs/.ishembed-rootfs-identity
#         build/fs.tar.gz           (unapproved intermediate bundle archive)
#         build/SHA256SUMS
#         build/ROOTFS_RECEIPT       (four-artifact transaction commit point)
#
# Usage:
#   scripts/build-rootfs.sh
#   scripts/build-rootfs.sh --print-inputs
#   scripts/build-rootfs.sh --print-identity
#   scripts/build-rootfs.sh --verify-rootfs <path>
#   ALPINE_VERSION=<version> \
#     ALPINE_SHA256=<reviewed-64-hex-digest> scripts/build-rootfs.sh
#
# Prereqs:
#   - bash, curl, git, python3, shasum, tar
#   - Meson toolchain for `fakefsify` (built from third_party/ish/tools)
#   - Zig, used to build the aarch64-linux-musl fallback supervisor
#
# Both the Alpine input digest and resulting fakefs archive digest are supply-
# chain inputs. The default version and mandatory SHA-256 come from the reviewed
# scripts/alpine-rootfs-pin.sh. An override must supply its own reviewed digest.
# Update ALPINE_VERSION carefully; newer Alpine builds can require guest features
# that the arm64 emulator does not implement.

set -euo pipefail

# PKG_ROOT is the root of this Swift Package repo (where Package.swift lives).
PKG_ROOT="$(exec 8>&- 9>&-; cd "$(dirname "$0" 8>&- 9>&-)/.." && pwd)"
ROOTFS_INPUTS_FILE="${ROOTFS_INPUTS_FILE:-$PKG_ROOT/scripts/alpine-rootfs-pin.sh}"
ROOTFS_ARCHIVER="${ROOTFS_ARCHIVER:-$PKG_ROOT/scripts/create-deterministic-tar.py}"
ISH_SRC="${ISH_SRC:-$PKG_ROOT/third_party/ish}"
alpine_version_override_set="${ALPINE_VERSION+x}"
alpine_sha256_override_set="${ALPINE_SHA256+x}"

if [[ "$alpine_version_override_set" != "$alpine_sha256_override_set" ]] || \
   { [[ -n "$alpine_version_override_set" ]] &&
     { [[ -z "${ALPINE_VERSION:-}" ]] || [[ -z "${ALPINE_SHA256:-}" ]]; }; }; then
    echo "ERROR: override ALPINE_VERSION and reviewed ALPINE_SHA256 together" >&2
    exit 64
fi

if [[ ! -f "$ROOTFS_INPUTS_FILE" || ! -r "$ROOTFS_INPUTS_FILE" ]]; then
    echo "ERROR: reviewed RootFS input pin is not readable: $ROOTFS_INPUTS_FILE" >&2
    exit 64
fi
# This is a version-controlled, data-only file. A custom path is an explicit
# caller choice and is subject to the same review requirement.
# shellcheck source=scripts/alpine-rootfs-pin.sh
source "$ROOTFS_INPUTS_FILE"

ALPINE_VERSION="${ALPINE_VERSION:-${PINNED_ALPINE_VERSION:-}}"
ALPINE_ARCH="${ALPINE_ARCH:-${PINNED_ALPINE_ARCH:-}}"   # iSH-arm64 expects arm64
ALPINE_SHA256="${ALPINE_SHA256:-${PINNED_ALPINE_SHA256:-}}"
ROOTFS_SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-${PINNED_ROOTFS_SOURCE_DATE_EPOCH:-}}"

if [[ ! "$ALPINE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "ERROR: ALPINE_VERSION must use the numeric major.minor.patch form" >&2
    exit 64
fi
ALPINE_MAJOR="${ALPINE_VERSION%.*}"
ALPINE_BASE="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/releases/${ALPINE_ARCH}"
ALPINE_TGZ="alpine-minirootfs-${ALPINE_VERSION}-${ALPINE_ARCH}.tar.gz"

if [[ "$ALPINE_ARCH" != "aarch64" ]]; then
    echo "ERROR: this package requires ALPINE_ARCH=aarch64" >&2
    exit 64
fi
if [[ -z "$ALPINE_SHA256" ]]; then
    echo "ERROR: ALPINE_SHA256 must be set to the reviewed Alpine archive digest" >&2
    exit 64
fi
if [[ ! "$ALPINE_SHA256" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "ERROR: ALPINE_SHA256 must contain exactly 64 hexadecimal characters" >&2
    exit 64
fi
ALPINE_SHA256="$(exec 8>&- 9>&-; printf '%s' "$ALPINE_SHA256" 8>&- 9>&- | \
    tr '[:upper:]' '[:lower:]' 8>&- 9>&-)"
if [[ ! "$ROOTFS_SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || \
   (( ROOTFS_SOURCE_DATE_EPOCH > 4294967295 )); then
    echo "ERROR: SOURCE_DATE_EPOCH must fit the unsigned 32-bit gzip timestamp" >&2
    exit 64
fi
ROOTFS_IDENTITY_NAME=".ishembed-rootfs-identity"
ISH_GUEST_ARCH="arm64"
ROOTFS_INPUTS_SHA256="$(exec 8>&- 9>&-; shasum -a 256 "$ROOTFS_INPUTS_FILE" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"
ROOTFS_BUILDER_SHA256="$(exec 8>&- 9>&-; shasum -a 256 "$0" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"

required_recipe_inputs=(
    "$ROOTFS_ARCHIVER"
    "$PKG_ROOT/supervisor/ishsv.c"
    "$PKG_ROOT/supervisor/Makefile"
    "$PKG_ROOT/protocol/proto.h"
    "$PKG_ROOT/include/ishembed.h"
)
for recipe_input in "${required_recipe_inputs[@]}"; do
    if [[ ! -f "$recipe_input" || ! -r "$recipe_input" || -L "$recipe_input" ]]; then
        echo "ERROR: RootFS recipe input is missing or unsafe: $recipe_input" >&2
        exit 64
    fi
done
if [[ ! -d "$ISH_SRC" || -L "$ISH_SRC" ]]; then
    echo "ERROR: iSH source directory is missing or unsafe: $ISH_SRC" >&2
    exit 64
fi

SUPERVISOR_SOURCE_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$PKG_ROOT/supervisor/ishsv.c" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
SUPERVISOR_MAKEFILE_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$PKG_ROOT/supervisor/Makefile" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
PROTOCOL_HEADER_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$PKG_ROOT/protocol/proto.h" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
ISHEMBED_HEADER_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$PKG_ROOT/include/ishembed.h" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
ROOTFS_ARCHIVER_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$ROOTFS_ARCHIVER" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"

# Hash the checked-out files that can feed the bundled fakefsify build. The
# commit identifies reviewed upstream history; the worktree hash also binds
# local tracked edits, deletions and non-ignored untracked source files, while a
# separate digest records every recursive gitlink revision/state.
ISH_REVISION="$(exec 8>&- 9>&-; git -C "$ISH_SRC" rev-parse HEAD 8>&- 9>&- 2>/dev/null || true)"
if [[ ! "$ISH_REVISION" =~ ^[0-9a-fA-F]{40,64}$ ]]; then
    ISH_REVISION="unversioned"
fi
ISH_WORKTREE_SHA256="$(exec 8>&- 9>&-; python3 - "$ISH_SRC" 8>&- 9>&- <<'PY'
import hashlib
import os
import stat
import subprocess
import sys

root = os.path.realpath(sys.argv[1])
root_b = os.fsencode(root)
try:
    raw = subprocess.check_output(
        ["git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        stderr=subprocess.DEVNULL,
    )
    paths = sorted(set(part for part in raw.split(b"\0") if part))
except (OSError, subprocess.CalledProcessError):
    paths = []
    for directory, names, files in os.walk(root_b, topdown=True):
        names[:] = sorted(name for name in names if name != b".git")
        for name in sorted(names + files):
            paths.append(os.path.relpath(os.path.join(directory, name), root_b))

digest = hashlib.sha256()
for relative in paths:
    full = os.path.join(root_b, relative)
    digest.update(len(relative).to_bytes(8, "big"))
    digest.update(relative)
    try:
        info = os.lstat(full)
    except FileNotFoundError:
        digest.update(b"missing\0")
        continue
    digest.update((info.st_mode & 0o177777).to_bytes(4, "big"))
    if stat.S_ISREG(info.st_mode):
        digest.update(b"file\0")
        with open(full, "rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    elif stat.S_ISLNK(info.st_mode):
        digest.update(b"link\0")
        digest.update(os.fsencode(os.readlink(full)))
    elif stat.S_ISDIR(info.st_mode):
        digest.update(b"dir\0")
    else:
        digest.update(b"special\0")
print(digest.hexdigest())
PY
)"
if [[ ! "$ISH_WORKTREE_SHA256" =~ ^[0-9a-f]{64}$ ]]; then
    echo "ERROR: could not fingerprint iSH source tree: $ISH_SRC" >&2
    exit 64
fi
ISH_SUBMODULES_SHA256="$(exec 8>&- 9>&-; python3 - "$ISH_SRC" 8>&- 9>&- <<'PY'
import hashlib
import re
import subprocess
import sys

root = sys.argv[1]
try:
    raw = subprocess.check_output(
        ["git", "-C", root, "submodule", "status", "--recursive"],
        stderr=subprocess.DEVNULL,
        text=True,
    )
except (OSError, subprocess.CalledProcessError):
    raw = ""

# `git submodule status` appends a human-readable ref description such as
# `(heads/main)` when one is available locally. That annotation depends on
# fetched refs and made otherwise identical clean checkouts produce different
# RootFS identities. Bind only the semantic status, checked-out object, and
# path, sorted by path.
records = []
for line in raw.splitlines():
    match = re.fullmatch(
        r"(?P<state>[ +\-U])(?P<object>[0-9a-fA-F]{40,64}) "
        r"(?P<path>\S+)(?: .*)?",
        line,
    )
    if not match:
        raise SystemExit("malformed recursive submodule status")
    records.append(
        (
            match.group("path").encode("utf-8"),
            match.group("state").encode("ascii"),
            match.group("object").lower().encode("ascii"),
        )
    )

digest = hashlib.sha256()
for path, state, object_name in sorted(records):
    digest.update(state)
    digest.update(b"\0")
    digest.update(object_name)
    digest.update(b"\0")
    digest.update(path)
    digest.update(b"\n")
print(digest.hexdigest())
PY
)"

FAKEFSIFY_ORIGIN="bundled-ish-source"
FAKEFSIFY_INPUT_SHA256="$ISH_WORKTREE_SHA256"
FAKEFSIFY_OVERRIDE="${FAKEFSIFY_BIN:-}"
FAKEFSIFY_PROVENANCE_OVERRIDE="${FAKEFSIFY_PROVENANCE_SHA256:-}"
FAKEFSIFY_EXPECTED_BINARY_OVERRIDE="${FAKEFSIFY_EXPECTED_BINARY_SHA256:-}"
if [[ -n "$FAKEFSIFY_OVERRIDE" ]]; then
    FAKEFSIFY_OVERRIDE="$(exec 8>&- 9>&-; \
        python3 - "$FAKEFSIFY_OVERRIDE" 8>&- 9>&- <<'PY'
import os
import sys
print(os.path.realpath(sys.argv[1]))
PY
)"
    if [[ ! -f "$FAKEFSIFY_OVERRIDE" || ! -x "$FAKEFSIFY_OVERRIDE" ]]; then
        echo "ERROR: FAKEFSIFY_BIN is not an executable regular file: $FAKEFSIFY_OVERRIDE" >&2
        exit 64
    fi
    if [[ -n "$FAKEFSIFY_PROVENANCE_OVERRIDE" ]]; then
        if [[ ! "$FAKEFSIFY_PROVENANCE_OVERRIDE" =~ ^[0-9a-fA-F]{64}$ ]]; then
            echo "ERROR: FAKEFSIFY_PROVENANCE_SHA256 must contain exactly 64 hexadecimal characters" >&2
            exit 64
        fi
        FAKEFSIFY_INPUT_SHA256="$(exec 8>&- 9>&-; \
            printf '%s' "$FAKEFSIFY_PROVENANCE_OVERRIDE" 8>&- 9>&- | \
            tr '[:upper:]' '[:lower:]' 8>&- 9>&-)"
        if [[ "$FAKEFSIFY_INPUT_SHA256" == "$ISH_WORKTREE_SHA256" ]]; then
            # The candidate gate builds one host tool from this exact source
            # tree and shares it across both generations. Keep the content
            # recipe identical to a direct source build; the external
            # environment receipt binds the actual host-tool bytes.
            FAKEFSIFY_ORIGIN="bundled-ish-source"
        else
            FAKEFSIFY_ORIGIN="reviewed-source-built-binary-override"
        fi
        if [[ ! "$FAKEFSIFY_EXPECTED_BINARY_OVERRIDE" =~ ^[0-9a-fA-F]{64}$ ]]; then
            echo "ERROR: source-provenance fakefsify requires FAKEFSIFY_EXPECTED_BINARY_SHA256" >&2
            exit 64
        fi
        FAKEFSIFY_EXPECTED_BINARY_OVERRIDE="$(exec 8>&- 9>&-; \
            printf '%s' "$FAKEFSIFY_EXPECTED_BINARY_OVERRIDE" 8>&- 9>&- | \
            tr '[:upper:]' '[:lower:]' 8>&- 9>&-)"
    else
        if [[ -n "$FAKEFSIFY_EXPECTED_BINARY_OVERRIDE" ]]; then
            echo "ERROR: FAKEFSIFY_EXPECTED_BINARY_SHA256 requires source provenance" >&2
            exit 64
        fi
        FAKEFSIFY_ORIGIN="reviewed-binary-override"
        FAKEFSIFY_INPUT_SHA256="$(exec 8>&- 9>&-; \
            shasum -a 256 "$FAKEFSIFY_OVERRIDE" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
    fi
elif [[ -n "$FAKEFSIFY_PROVENANCE_OVERRIDE" ||
        -n "$FAKEFSIFY_EXPECTED_BINARY_OVERRIDE" ]]; then
    echo "ERROR: fakefsify provenance and expected binary digest require FAKEFSIFY_BIN" >&2
    exit 64
fi

ROOTFS_RECIPE_SHA256="$(exec 8>&- 9>&-; {
    printf 'ROOTFS_BUILDER_SHA256=%s\n' "$ROOTFS_BUILDER_SHA256"
    printf 'SUPERVISOR_SOURCE_SHA256=%s\n' "$SUPERVISOR_SOURCE_SHA256"
    printf 'SUPERVISOR_MAKEFILE_SHA256=%s\n' "$SUPERVISOR_MAKEFILE_SHA256"
    printf 'PROTOCOL_HEADER_SHA256=%s\n' "$PROTOCOL_HEADER_SHA256"
    printf 'ISHEMBED_HEADER_SHA256=%s\n' "$ISHEMBED_HEADER_SHA256"
    printf 'ROOTFS_ARCHIVER_SHA256=%s\n' "$ROOTFS_ARCHIVER_SHA256"
    printf 'ROOTFS_SOURCE_DATE_EPOCH=%s\n' "$ROOTFS_SOURCE_DATE_EPOCH"
    printf 'ISH_REVISION=%s\n' "$ISH_REVISION"
    printf 'ISH_WORKTREE_SHA256=%s\n' "$ISH_WORKTREE_SHA256"
    printf 'ISH_SUBMODULES_SHA256=%s\n' "$ISH_SUBMODULES_SHA256"
    printf 'FAKEFSIFY_ORIGIN=%s\n' "$FAKEFSIFY_ORIGIN"
    printf 'FAKEFSIFY_INPUT_SHA256=%s\n' "$FAKEFSIFY_INPUT_SHA256"
} 8>&- 9>&- | shasum -a 256 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"

print_rootfs_recipe_identity() {
    # Fixed, data-only format. Consumers compare these bytes; they must never
    # source this file as shell code. Bump recipe when generated contents change.
    printf 'ROOTFS_IDENTITY_SCHEMA=4\n'
    printf 'ROOTFS_RECIPE=alpine-fakefs-ishsv-v4\n'
    printf 'ROOTFS_RECIPE_SHA256=%s\n' "$ROOTFS_RECIPE_SHA256"
    printf 'ROOTFS_BUILDER_SHA256=%s\n' "$ROOTFS_BUILDER_SHA256"
    printf 'SUPERVISOR_SOURCE_SHA256=%s\n' "$SUPERVISOR_SOURCE_SHA256"
    printf 'SUPERVISOR_MAKEFILE_SHA256=%s\n' "$SUPERVISOR_MAKEFILE_SHA256"
    printf 'PROTOCOL_HEADER_SHA256=%s\n' "$PROTOCOL_HEADER_SHA256"
    printf 'ISHEMBED_HEADER_SHA256=%s\n' "$ISHEMBED_HEADER_SHA256"
    printf 'ROOTFS_ARCHIVER_SHA256=%s\n' "$ROOTFS_ARCHIVER_SHA256"
    printf 'ROOTFS_SOURCE_DATE_EPOCH=%s\n' "$ROOTFS_SOURCE_DATE_EPOCH"
    printf 'ISH_REVISION=%s\n' "$ISH_REVISION"
    printf 'ISH_WORKTREE_SHA256=%s\n' "$ISH_WORKTREE_SHA256"
    printf 'ISH_SUBMODULES_SHA256=%s\n' "$ISH_SUBMODULES_SHA256"
    printf 'FAKEFSIFY_ORIGIN=%s\n' "$FAKEFSIFY_ORIGIN"
    printf 'FAKEFSIFY_INPUT_SHA256=%s\n' "$FAKEFSIFY_INPUT_SHA256"
    printf 'ALPINE_VERSION=%s\n' "$ALPINE_VERSION"
    printf 'ALPINE_ARCH=%s\n' "$ALPINE_ARCH"
    printf 'ISH_GUEST_ARCH=%s\n' "$ISH_GUEST_ARCH"
    printf 'ALPINE_URL=%s/%s\n' "$ALPINE_BASE" "$ALPINE_TGZ"
    printf 'ALPINE_SHA256=%s\n' "$ALPINE_SHA256"
    printf 'ROOTFS_INPUTS_SHA256=%s\n' "$ROOTFS_INPUTS_SHA256"
}
ROOTFS_EXPECTED_RECIPE_IDENTITY="$(exec 8>&- 9>&-; print_rootfs_recipe_identity)"

rootfs_data_sha256() {
    local rootfs_path="$1"
    python3 - "$rootfs_path/data" 8>&- 9>&- <<'PY'
import hashlib
import os
import stat
import sys

data_root = os.fsencode(os.path.realpath(sys.argv[1]))
digest = hashlib.sha256()
entries = []
for directory, names, files in os.walk(data_root, topdown=True, followlinks=False):
    names.sort()
    files.sort()
    for name in names + files:
        full = os.path.join(directory, name)
        entries.append((os.path.relpath(full, data_root), full))

for relative, full in sorted(entries):
    info = os.lstat(full)
    digest.update(len(relative).to_bytes(8, "big"))
    digest.update(relative)
    digest.update((info.st_mode & 0o177777).to_bytes(4, "big"))
    if stat.S_ISDIR(info.st_mode):
        digest.update(b"dir\0")
    elif stat.S_ISREG(info.st_mode):
        digest.update(b"file\0")
        with open(full, "rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    else:
        raise SystemExit("unsafe non-file entry in fakefs data: " + os.fsdecode(relative))
print(digest.hexdigest())
PY
}

rootfs_content_sha256() {
    local meta_sha="$1"
    local data_sha="$2"
    {
        printf 'meta.db=%s\n' "$meta_sha"
        printf 'data=%s\n' "$data_sha"
    } 8>&- 9>&- | shasum -a 256 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-
}

identity_value() {
    local identity_path="$1"
    local key="$2"
    awk -F= -v wanted="$key" '$1 == wanted { count += 1; value = substr($0, length($1) + 2) }
        END { if (count == 1) print value; else exit 1 }' "$identity_path" 8>&- 9>&-
}

validate_fakefs_layout() {
    local rootfs_path="$1"
    python3 - "$rootfs_path" "$ALPINE_VERSION" 8>&- 9>&- <<'PY'
import os
import sqlite3
import stat
import sys
import urllib.parse

root = os.path.realpath(sys.argv[1])
expected_version = sys.argv[2]
db = os.path.join(root, "meta.db")
data = os.path.join(root, "data")

def fail(message):
    print("ERROR: " + message, file=sys.stderr)
    raise SystemExit(1)

for path, kind in ((db, "file"), (data, "directory")):
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        fail("RootFS is missing " + os.path.relpath(path, root))
    if stat.S_ISLNK(info.st_mode):
        fail("RootFS contains an unsafe symlink: " + os.path.relpath(path, root))
    if kind == "file" and not stat.S_ISREG(info.st_mode):
        fail("RootFS meta.db is not a regular file")
    if kind == "directory" and not stat.S_ISDIR(info.st_mode):
        fail("RootFS data is not a directory")

for suffix in ("-journal", "-wal", "-shm"):
    sidecar = db + suffix
    if os.path.lexists(sidecar):
        info = os.lstat(sidecar)
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
            fail("unsafe SQLite sidecar: " + os.path.basename(sidecar))
        # A rollback journal means the database may be in the middle of a
        # transaction or require SQLite recovery. Never publish or reuse that
        # ambiguous state, even when the journal happens to be empty.
        if suffix == "-journal":
            fail("fakefs meta.db has a rollback journal")
        # A non-empty WAL can contain committed metadata not present in
        # meta.db. Any snapshot/copy that omits transient sidecars would
        # silently lose it, so it is never accepted as a reusable input.
        if suffix == "-wal" and info.st_size != 0:
            fail("fakefs meta.db has an uncheckpointed non-empty WAL")

uri = "file:" + urllib.parse.quote(db) + "?mode=ro"
try:
    connection = sqlite3.connect(uri, uri=True)
    connection.execute("pragma query_only = on")
    check = connection.execute("pragma quick_check").fetchall()
    if check != [("ok",)]:
        fail("fakefs meta.db quick_check failed")
    tables = {row[0] for row in connection.execute(
        "select name from sqlite_master where type = 'table'"
    )}
    if tables != {"meta", "paths", "stats"}:
        fail("fakefs meta.db has an unexpected table set")
    if connection.execute("pragma user_version").fetchone() != (3,):
        fail("fakefs meta.db user_version is not 3")

    def table_signature(name):
        return [
            (row[1], row[2].upper(), row[3], row[4], row[5])
            for row in connection.execute(f"pragma table_info('{name}')")
        ]

    expected_tables = {
        "meta": [
            ("id", "INTEGER", 0, "0", 0),
            ("db_inode", "INTEGER", 0, None, 0),
        ],
        "stats": [
            ("inode", "INTEGER", 0, None, 1),
            ("stat", "BLOB", 0, None, 0),
        ],
        "paths": [
            ("path", "BLOB", 0, None, 1),
            ("inode", "INTEGER", 0, None, 0),
        ],
    }
    for table, expected in expected_tables.items():
        if table_signature(table) != expected:
            fail("fakefs meta.db has an incompatible " + table + " schema")

    def index_signatures(table):
        signatures = set()
        for _, name, unique, origin, partial in connection.execute(
            f"pragma index_list('{table}')"
        ):
            columns = tuple(
                row[2] for row in connection.execute(
                    "pragma index_info('" + name.replace("'", "''") + "')"
                )
            )
            signatures.add((unique, origin, partial, columns))
        return signatures

    if index_signatures("meta") != {(1, "u", 0, ("id",))}:
        fail("fakefs meta.db is missing the unique meta.id index")
    if index_signatures("stats"):
        fail("fakefs meta.db has unexpected stats indexes")
    if index_signatures("paths") != {
        (1, "pk", 0, ("path",)),
        (0, "c", 0, ("inode", "path")),
    }:
        fail("fakefs meta.db has incompatible paths indexes")
    foreign_keys = list(connection.execute("pragma foreign_key_list('paths')"))
    if foreign_keys != [
        (0, 0, "stats", "inode", "inode", "NO ACTION", "NO ACTION", "NONE")
    ]:
        fail("fakefs meta.db has an incompatible paths foreign key")
    if connection.execute(
        "select count(*) from sqlite_master where type = 'trigger'"
    ).fetchone()[0] != 0:
        fail("fakefs meta.db contains an unexpected trigger")
    if connection.execute("select count(*) from meta").fetchone()[0] != 1:
        fail("fakefs meta.db has an invalid meta row count")
    if connection.execute(
        "select count(*) from meta where typeof(id) != 'integer' or id != 0 "
        "or typeof(db_inode) != 'integer' or db_inode < 0"
    ).fetchone()[0] != 0:
        fail("fakefs meta row has invalid id or db_inode fields")
    if connection.execute(
        "select count(*) from stats where typeof(inode) != 'integer' "
        "or inode <= 0 or typeof(stat) != 'blob' or length(stat) != 16"
    ).fetchone()[0] != 0:
        fail("fakefs stats rows must use positive integer inodes and 16-byte stat blobs")
    if connection.execute(
        "select count(*) from paths where typeof(path) != 'blob' "
        "or typeof(inode) != 'integer' or inode <= 0"
    ).fetchone()[0] != 0:
        fail("fakefs paths rows must use blob paths and positive integer inodes")
    if connection.execute(
        "select count(*) from paths where typeof(path) = 'blob' and length(path) = 0"
    ).fetchone()[0] != 1:
        fail("fakefs meta.db must contain exactly one empty root path")
    if connection.execute(
        "select count(*) from paths p left join stats s on p.inode = s.inode "
        "where s.inode is null"
    ).fetchone()[0] != 0:
        fail("fakefs meta.db contains paths without stat records")
    rows = connection.execute("select path from paths").fetchall()
except (sqlite3.DatabaseError, OSError) as error:
    fail("cannot validate fakefs meta.db: " + str(error))
finally:
    try:
        connection.close()
    except NameError:
        pass

db_paths = set()
for (raw_path,) in rows:
    path = raw_path if isinstance(raw_path, bytes) else os.fsencode(raw_path)
    if path == b"":
        continue
    if not path.startswith(b"/") or b"\0" in path:
        fail("fakefs meta.db contains a non-absolute path")
    components = path[1:].split(b"/")
    if not components or any(component in (b"", b".", b"..") for component in components):
        fail("fakefs meta.db contains a non-canonical path")
    db_paths.add(path)

storage_paths = set()
data_b = os.fsencode(data)
for directory, names, files in os.walk(data_b, topdown=True, followlinks=False):
    names.sort()
    files.sort()
    for name in names + files:
        full = os.path.join(directory, name)
        info = os.lstat(full)
        relative = os.path.relpath(full, data_b)
        if stat.S_ISLNK(info.st_mode) or not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
            fail("fakefs data contains an unsafe storage entry: " + os.fsdecode(relative))
        storage_paths.add(b"/" + relative)

if db_paths != storage_paths:
    missing = len(db_paths - storage_paths)
    extra = len(storage_paths - db_paths)
    fail(f"fakefs meta/data path mismatch (missing={missing}, extra={extra})")
if len(db_paths) < 100:
    fail("fakefs RootFS is implausibly small")

required = (b"/bin/busybox", b"/etc/os-release", b"/sbin/ishsv")
for path in required:
    if path not in db_paths:
        fail("fakefs RootFS is missing " + os.fsdecode(path))

busybox = os.path.join(data_b, b"bin", b"busybox")
supervisor = os.path.join(data_b, b"sbin", b"ishsv")
os_release = os.path.join(data_b, b"etc", b"os-release")
with open(busybox, "rb") as stream:
    if stream.read(4) != b"\x7fELF":
        fail("/bin/busybox is not an ELF executable")
with open(supervisor, "rb") as stream:
    header = stream.read(20)
    if len(header) != 20 or header[:4] != b"\x7fELF" or int.from_bytes(header[18:20], "little") != 183:
        fail("/sbin/ishsv is not an AArch64 ELF executable")
with open(os_release, "rb") as stream:
    release_lines = stream.read(64 * 1024).decode("utf-8", "strict").splitlines()
release = {}
for line in release_lines:
    if "=" in line:
        key, value = line.split("=", 1)
        release[key] = value.strip('"')
if release.get("ID") != "alpine" or release.get("VERSION_ID") != expected_version:
    fail("/etc/os-release does not match the reviewed Alpine version")
PY
}

validate_rootfs() {
    local rootfs_path="$1"
    local require_initial_content="${2:-0}"
    local identity_path="$rootfs_path/$ROOTFS_IDENTITY_NAME"
    local expected_recipe expected_lines total_lines
    local supervisor_sha busybox_sha initial_meta_sha initial_data_sha initial_content_sha
    local actual_sha

    if [[ ! -d "$rootfs_path" || -L "$rootfs_path" ]]; then
        echo "ERROR: missing or unsafe RootFS directory: $rootfs_path" >&2
        return 1
    fi
    if [[ ! -f "$identity_path" || -L "$identity_path" ]]; then
        echo "ERROR: missing or unsafe RootFS identity marker: $identity_path" >&2
        return 1
    fi
    expected_recipe="$(exec 8>&- 9>&-; print_rootfs_recipe_identity)"
    expected_lines="$(exec 8>&- 9>&-; printf '%s\n' "$expected_recipe" 8>&- 9>&- | \
        wc -l 8>&- 9>&- | tr -d ' ' 8>&- 9>&-)"
    if ! cmp -s <(sed -n "1,${expected_lines}p" "$identity_path" 8>&- 9>&-) \
        <(printf '%s\n' "$expected_recipe" 8>&- 9>&-) 8>&- 9>&-; then
        echo "ERROR: RootFS recipe identity does not match reviewed inputs: $identity_path" >&2
        return 1
    fi
    total_lines="$(exec 8>&- 9>&-; wc -l 8>&- 9>&- < "$identity_path" | \
        tr -d ' ' 8>&- 9>&-)"
    if (( total_lines != expected_lines + 5 )); then
        echo "ERROR: RootFS identity marker has an invalid field set: $identity_path" >&2
        return 1
    fi

    supervisor_sha="$(exec 8>&- 9>&-; identity_value "$identity_path" SUPERVISOR_BINARY_SHA256 || true)"
    busybox_sha="$(exec 8>&- 9>&-; identity_value "$identity_path" BUSYBOX_BINARY_SHA256 || true)"
    initial_meta_sha="$(exec 8>&- 9>&-; identity_value "$identity_path" ROOTFS_INITIAL_META_SHA256 || true)"
    initial_data_sha="$(exec 8>&- 9>&-; identity_value "$identity_path" ROOTFS_INITIAL_DATA_SHA256 || true)"
    initial_content_sha="$(exec 8>&- 9>&-; identity_value "$identity_path" ROOTFS_INITIAL_CONTENT_SHA256 || true)"
    for actual_sha in "$supervisor_sha" "$busybox_sha" \
        "$initial_meta_sha" "$initial_data_sha" "$initial_content_sha"; do
        if [[ ! "$actual_sha" =~ ^[0-9a-f]{64}$ ]]; then
            echo "ERROR: RootFS identity marker contains a malformed digest: $identity_path" >&2
            return 1
        fi
    done

    if ! validate_fakefs_layout "$rootfs_path"; then
        return 1
    fi
    actual_sha="$(exec 8>&- 9>&-; shasum -a 256 "$rootfs_path/data/sbin/ishsv" 8>&- 9>&- | \
        awk '{print $1}' 8>&- 9>&-)"
    if [[ "$actual_sha" != "$supervisor_sha" ]]; then
        echo "ERROR: RootFS supervisor does not match its content identity" >&2
        return 1
    fi
    actual_sha="$(exec 8>&- 9>&-; shasum -a 256 "$rootfs_path/data/bin/busybox" 8>&- 9>&- | \
        awk '{print $1}' 8>&- 9>&-)"
    if [[ "$actual_sha" != "$busybox_sha" ]]; then
        echo "ERROR: RootFS busybox does not match its content identity" >&2
        return 1
    fi

    # The full byte seal is verified before publication. A running fakefs is
    # intentionally mutable, so reuse checks the recipe, critical payloads and
    # complete SQLite/data path consistency instead of rejecting valid writes.
    if [[ "$require_initial_content" == "1" ]]; then
        actual_sha="$(exec 8>&- 9>&-; shasum -a 256 "$rootfs_path/meta.db" 8>&- 9>&- | \
            awk '{print $1}' 8>&- 9>&-)"
        if [[ "$actual_sha" != "$initial_meta_sha" ]]; then
            echo "ERROR: staged RootFS meta.db changed after it was sealed" >&2
            return 1
        fi
        actual_sha="$(exec 8>&- 9>&-; rootfs_data_sha256 "$rootfs_path")"
        if [[ "$actual_sha" != "$initial_data_sha" ]]; then
            echo "ERROR: staged RootFS data changed after it was sealed" >&2
            return 1
        fi
        actual_sha="$(exec 8>&- 9>&-; \
            rootfs_content_sha256 "$initial_meta_sha" "$initial_data_sha")"
        if [[ "$actual_sha" != "$initial_content_sha" ]]; then
            echo "ERROR: staged RootFS content seal is invalid" >&2
            return 1
        fi
    fi
    return 0
}

validate_rootfs_bundle() {
    local bundle_dir="$1"
    local rootfs_path="$bundle_dir/fs"
    local tarball_path="$bundle_dir/fs.tar.gz"
    local sums_path="$bundle_dir/SHA256SUMS"
    local receipt_path="$bundle_dir/ROOTFS_RECEIPT"
    local receipt_schema receipt_recipe identity_sha tarball_sha sums_sha actual_sha

    if ! validate_rootfs "$rootfs_path"; then
        return 1
    fi
    for artifact_path in "$tarball_path" "$sums_path" "$receipt_path"; do
        if [[ ! -f "$artifact_path" || -L "$artifact_path" ]]; then
            echo "ERROR: missing or unsafe RootFS bundle artifact: $artifact_path" >&2
            return 1
        fi
    done
    if [[ "$(exec 8>&- 9>&-; wc -l 8>&- 9>&- < "$receipt_path" | \
        tr -d ' ' 8>&- 9>&-)" != "5" ]]; then
        echo "ERROR: RootFS receipt has an invalid field set: $receipt_path" >&2
        return 1
    fi
    receipt_schema="$(exec 8>&- 9>&-; identity_value "$receipt_path" ROOTFS_RECEIPT_SCHEMA || true)"
    receipt_recipe="$(exec 8>&- 9>&-; identity_value "$receipt_path" ROOTFS_RECIPE_SHA256 || true)"
    identity_sha="$(exec 8>&- 9>&-; identity_value "$receipt_path" ROOTFS_IDENTITY_SHA256 || true)"
    tarball_sha="$(exec 8>&- 9>&-; identity_value "$receipt_path" ROOTFS_TARBALL_SHA256 || true)"
    sums_sha="$(exec 8>&- 9>&-; identity_value "$receipt_path" ROOTFS_SUMS_SHA256 || true)"
    if [[ "$receipt_schema" != "1" || "$receipt_recipe" != "$ROOTFS_RECIPE_SHA256" ]]; then
        echo "ERROR: RootFS receipt does not match the reviewed recipe" >&2
        return 1
    fi
    for actual_sha in "$identity_sha" "$tarball_sha" "$sums_sha"; do
        if [[ ! "$actual_sha" =~ ^[0-9a-f]{64}$ ]]; then
            echo "ERROR: RootFS receipt contains a malformed digest" >&2
            return 1
        fi
    done
    actual_sha="$(exec 8>&- 9>&-; \
        shasum -a 256 "$rootfs_path/$ROOTFS_IDENTITY_NAME" 8>&- 9>&- | \
        awk '{print $1}' 8>&- 9>&-)"
    if [[ "$actual_sha" != "$identity_sha" ]]; then
        echo "ERROR: RootFS identity does not match the committed receipt" >&2
        return 1
    fi
    actual_sha="$(exec 8>&- 9>&-; shasum -a 256 "$tarball_path" 8>&- 9>&- | \
        awk '{print $1}' 8>&- 9>&-)"
    if [[ "$actual_sha" != "$tarball_sha" ]]; then
        echo "ERROR: RootFS tarball does not match the committed receipt" >&2
        return 1
    fi
    actual_sha="$(exec 8>&- 9>&-; shasum -a 256 "$sums_path" 8>&- 9>&- | \
        awk '{print $1}' 8>&- 9>&-)"
    if [[ "$actual_sha" != "$sums_sha" ]]; then
        echo "ERROR: SHA256SUMS does not match the committed receipt" >&2
        return 1
    fi
    if [[ "$(< "$sums_path")" != "$tarball_sha  fs.tar.gz" ]]; then
        echo "ERROR: SHA256SUMS does not name the committed RootFS tarball" >&2
        return 1
    fi
    return 0
}

case "${1:-}" in
    "") ;;
    --print-inputs)
        if (( $# != 1 )); then
            echo "ERROR: --print-inputs does not accept additional arguments" >&2
            exit 64
        fi
        print_rootfs_recipe_identity
        printf 'ROOTFS_INPUTS_FILE=%s\n' "$ROOTFS_INPUTS_FILE"
        exit 0
        ;;
    --print-identity)
        if (( $# != 1 )); then
            echo "ERROR: --print-identity does not accept additional arguments" >&2
            exit 64
        fi
        print_rootfs_recipe_identity
        exit 0
        ;;
    --verify-rootfs)
        if (( $# != 2 )); then
            echo "ERROR: --verify-rootfs requires exactly one RootFS path" >&2
            exit 64
        fi
        validate_rootfs "$2"
        exit $?
        ;;
    --verify-bundle)
        if (( $# != 2 )); then
            echo "ERROR: --verify-bundle requires exactly one bundle directory" >&2
            exit 64
        fi
        validate_rootfs_bundle "$2"
        exit $?
        ;;
    -h|--help)
        sed -n '2,/^$/p' "$0"
        exit 0
        ;;
    *)
        echo "ERROR: unknown argument: $1" >&2
        exit 64
        ;;
esac

BUILD_DIR="${BUILD_DIR:-$PKG_ROOT/build}"
case "${ROOTFS_REQUIRE_ABSENT:-0}" in
    0|1) ;;
    *)
        echo "ERROR: ROOTFS_REQUIRE_ABSENT must be 0 or 1" >&2
        exit 64
        ;;
esac
case "${ROOTFS_REUSE_VALID:-0}" in
    0|1) ;;
    *)
        echo "ERROR: ROOTFS_REUSE_VALID must be 0 or 1" >&2
        exit 64
        ;;
esac

mkdir -p "$BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd -P)"
ROOTFS_OUT="$BUILD_DIR/fs"
TARBALL_OUT="$BUILD_DIR/fs.tar.gz"
SUMS_OUT="$BUILD_DIR/SHA256SUMS"
RECEIPT_OUT="$BUILD_DIR/ROOTFS_RECEIPT"
ROOTFS_LOCK="$BUILD_DIR/.rootfs-build.lock"
ROOTFS_LOCK_OWNER_TOKEN=""
ROOTFS_LOCK_OWNER_PID=""
ROOTFS_LOCK_OWNER_START=""
ROOTFS_LOCK_OWNED=0
# macOS still ships Bash 3.2, which has no `exec {var}` dynamic-FD syntax.
# This script owns at most one lock, so reserve a fixed descriptor.
# Non-lock child processes close fd 9 explicitly. Command substitutions also
# start with `exec 9>&-` so their intermediary shell cannot retain the flock or
# pass it to a background descendant after this script exits. fd 8 is reserved
# by callers for the companion generation lock and is isolated the same way.
ROOTFS_LOCK_FD=9
ROOTFS_STAGE=""
PUBLISH_TRANSACTION_ACTIVE=0
PUBLISH_ROLLBACK_FAILED=0
PUBLISH_DEFERRED_EXIT=0

atomic_rename_impl() {
    local operation="$1"
    local source_path="$2"
    local destination_path="$3"
    python3 - "$operation" "$source_path" "$destination_path" 8>&- 9>&- <<'PY'
import ctypes
import os
import signal
import sys

operation = sys.argv[1]
source = os.fsencode(sys.argv[2])
destination = os.fsencode(sys.argv[3])
flags = {"noreplace": 1, "exchange": 2}
if operation not in flags:
    raise SystemExit("invalid atomic rename operation")
libc = ctypes.CDLL(None, use_errno=True)
# A terminal signal is deferred by the parent shell until it journals this
# operation. Block the same signals in this short-lived helper so a process-
# group signal cannot kill it after rename(2) but before its success status.
signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGHUP})

if sys.platform == "darwin":
    darwin_flags = {"noreplace": 0x00000004, "exchange": 0x00000002}
    rename = libc.renamex_np
    rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    result = rename(source, destination, darwin_flags[operation])
elif sys.platform.startswith("linux") and hasattr(libc, "renameat2"):
    at_fdcwd = -100
    rename = libc.renameat2
    rename.argtypes = [
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    ]
    rename.restype = ctypes.c_int
    result = rename(at_fdcwd, source, at_fdcwd, destination, flags[operation])
else:
    print("ERROR: required atomic rename primitive is unavailable", file=sys.stderr)
    raise SystemExit(1)

if result != 0:
    error = ctypes.get_errno()
    print(
        f"ERROR: atomic {operation} rename failed: " + os.strerror(error),
        file=sys.stderr,
    )
    raise SystemExit(1)
PY
}

atomic_rename_noreplace() {
    atomic_rename_impl noreplace "$1" "$2"
}

atomic_rename_exchange() {
    atomic_rename_impl exchange "$1" "$2"
}

process_start_identity() {
    local process_pid="$1"
    python3 - "$process_pid" 8>&- 9>&- <<'PY'
import hashlib
import os
import subprocess
import sys

try:
    pid = int(sys.argv[1])
    if pid <= 0:
        raise ValueError
except ValueError:
    raise SystemExit(1)

if sys.platform.startswith("linux"):
    try:
        raw = open(f"/proc/{pid}/stat", "rb").read()
        tail = raw[raw.rfind(b")") + 2:].split()
        start_ticks = int(tail[19])
    except (OSError, ValueError, IndexError):
        raise SystemExit(1)
    print(f"linux:{start_ticks}")
else:
    try:
        raw = subprocess.check_output(
            ["ps", "-p", str(pid), "-o", "lstart="],
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        raise SystemExit(1)
    if not raw:
        raise SystemExit(1)
    print("darwin:" + hashlib.sha256(raw).hexdigest())
PY
}

new_lock_token() {
    python3 - 8>&- 9>&- <<'PY'
import secrets
print(secrets.token_hex(24))
PY
}

read_lock_owner() {
    local lock_path="$1"
    python3 - "$lock_path" 8>&- 9>&- <<'PY'
import os
import re
import stat
import sys

try:
    info = os.lstat(sys.argv[1])
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise OSError("lock is not a regular file")
    with open(sys.argv[1], "r", encoding="ascii", newline="") as stream:
        lines = stream.read().splitlines()
except (OSError, UnicodeError):
    raise SystemExit(1)
expected = ("LOCK_SCHEMA", "PID", "START_ID", "TOKEN")
if len(lines) != len(expected):
    raise SystemExit(1)
values = {}
for line, key in zip(lines, expected):
    if not line.startswith(key + "="):
        raise SystemExit(1)
    values[key] = line[len(key) + 1:]
if values["LOCK_SCHEMA"] != "1":
    raise SystemExit(1)
if not re.fullmatch(r"[1-9][0-9]*", values["PID"]):
    raise SystemExit(1)
if not re.fullmatch(r"(?:linux:[0-9]+|darwin:[0-9a-f]{64})", values["START_ID"]):
    raise SystemExit(1)
if not re.fullmatch(r"[0-9a-f]{48}", values["TOKEN"]):
    raise SystemExit(1)
print(values["PID"] + "|" + values["START_ID"] + "|" + values["TOKEN"])
PY
}

lock_path_matches_fd() {
    local lock_path="$1"
    local lock_fd="$2"
    # This helper intentionally receives the live lock descriptor. It is one
    # of the few children that must inherit fd 9 while ownership is checked.
    python3 - "$lock_path" "$lock_fd" 8>&- <<'PY'
import os
import stat
import sys

try:
    path_info = os.lstat(sys.argv[1])
    fd_info = os.fstat(int(sys.argv[2]))
except (OSError, ValueError):
    raise SystemExit(1)
if stat.S_ISLNK(path_info.st_mode) or not stat.S_ISREG(path_info.st_mode):
    raise SystemExit(1)
if not stat.S_ISREG(fd_info.st_mode):
    raise SystemExit(1)
if (path_info.st_dev, path_info.st_ino) != (fd_info.st_dev, fd_info.st_ino):
    raise SystemExit(1)
PY
}

acquire_fd_lock() {
    local lock_fd="$1"
    local timeout_seconds="${ROOTFS_LOCK_TIMEOUT_SECONDS:-300}"
    [[ "$timeout_seconds" =~ ^[0-9]+$ ]] || {
        echo "ERROR: ROOTFS_LOCK_TIMEOUT_SECONDS must be a non-negative integer" >&2
        return 64
    }
    # The flock operation must run against the already-open descriptor.
    python3 - "$lock_fd" "$timeout_seconds" 8>&- <<'PY'
import errno
import fcntl
import sys
import time

fd = int(sys.argv[1])
timeout = int(sys.argv[2])
deadline = time.monotonic() + timeout
while True:
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        break
    except OSError as error:
        if error.errno not in (errno.EACCES, errno.EAGAIN):
            raise
        if time.monotonic() >= deadline:
            raise SystemExit(75)
        time.sleep(0.05)
PY
}

write_lock_owner() {
    local lock_fd="$1"
    local owner_pid="$2"
    local owner_start="$3"
    local owner_token="$4"
    # Owner metadata is written through the locked descriptor itself.
    python3 - "$lock_fd" "$owner_pid" "$owner_start" "$owner_token" 8>&- <<'PY'
import os
import re
import sys

fd = int(sys.argv[1])
pid, start, token = sys.argv[2:]
if not re.fullmatch(r"[1-9][0-9]*", pid):
    raise SystemExit(1)
if not re.fullmatch(r"(?:linux:[0-9]+|darwin:[0-9a-f]{64})", start):
    raise SystemExit(1)
if not re.fullmatch(r"[0-9a-f]{48}", token):
    raise SystemExit(1)
payload = (
    "LOCK_SCHEMA=1\n"
    f"PID={pid}\n"
    f"START_ID={start}\n"
    f"TOKEN={token}\n"
).encode("ascii")
os.fchmod(fd, 0o600)
os.lseek(fd, 0, os.SEEK_SET)
os.ftruncate(fd, 0)
while payload:
    written = os.write(fd, payload)
    payload = payload[written:]
os.fsync(fd)
PY
}

kernel_lock_is_held() {
    local lock_path="$1"
    python3 - "$lock_path" 8>&- 9>&- <<'PY'
import errno
import fcntl
import os
import stat
import sys

flags = os.O_RDWR
if hasattr(os, "O_NOFOLLOW"):
    flags |= os.O_NOFOLLOW
try:
    fd = os.open(sys.argv[1], flags)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode):
        raise OSError(errno.EINVAL, "lock is not a regular file")
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as error:
        if error.errno in (errno.EACCES, errno.EAGAIN):
            raise SystemExit(0)
        raise
    raise SystemExit(1)
except OSError:
    raise SystemExit(1)
PY
}

close_lock_fd() {
    local lock_fd="$1"
    [[ "$lock_fd" == 9 ]] || return 1
    exec 9>&-
}

release_rootfs_lock() {
    local owner rc=0
    [[ "$ROOTFS_LOCK_OWNED" == "1" ]] || return 0
    owner="$(exec 8>&- 9>&-; read_lock_owner "$ROOTFS_LOCK" 2>/dev/null || true)"
    if ! lock_path_matches_fd "$ROOTFS_LOCK" "$ROOTFS_LOCK_FD" ||
       [[ "$owner" != "$ROOTFS_LOCK_OWNER_PID|$ROOTFS_LOCK_OWNER_START|$ROOTFS_LOCK_OWNER_TOKEN" ]]; then
        echo "ERROR: refusing to release RootFS lock with changed ownership" >&2
        rc=1
    fi
    ROOTFS_LOCK_OWNED=0
    close_lock_fd "$ROOTFS_LOCK_FD" || rc=1
    return "$rc"
}

cleanup_build() {
    local rc=$?
    trap - EXIT
    trap '' INT TERM HUP
    if [[ "$PUBLISH_TRANSACTION_ACTIVE" == "1" ]]; then
        rollback_publish_transaction || PUBLISH_ROLLBACK_FAILED=1
    fi
    if [[ -n "$ROOTFS_STAGE" && "$PUBLISH_ROLLBACK_FAILED" == "0" ]]; then
        case "$ROOTFS_STAGE" in
            "$BUILD_DIR"/.fs.staging.*)
                rm -rf -- "$ROOTFS_STAGE" 8>&- 9>&-
                ;;
        esac
    fi
    release_rootfs_lock || rc=74
    if [[ "$PUBLISH_ROLLBACK_FAILED" == "1" ]]; then
        echo "ERROR: transaction rollback was incomplete; retained staging at $ROOTFS_STAGE" >&2
        rc=74
    fi
    exit "$rc"
}
trap cleanup_build EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

acquire_rootfs_lock() {
    local lock_rc
    ROOTFS_LOCK_OWNER_PID="${BASHPID:-$$}"
    ROOTFS_LOCK_OWNER_START="$(process_start_identity "$ROOTFS_LOCK_OWNER_PID")"
    ROOTFS_LOCK_OWNER_TOKEN="$(new_lock_token)"
    if [[ -L "$ROOTFS_LOCK" || ( -e "$ROOTFS_LOCK" && ! -f "$ROOTFS_LOCK" ) ]]; then
        echo "ERROR: RootFS lock path is not a regular file: $ROOTFS_LOCK" >&2
        return 74
    fi
    exec 9<>"$ROOTFS_LOCK" || return 74
    if ! lock_path_matches_fd "$ROOTFS_LOCK" "$ROOTFS_LOCK_FD"; then
        echo "ERROR: RootFS lock path changed while it was opened" >&2
        close_lock_fd "$ROOTFS_LOCK_FD" || true
        return 74
    fi
    set +e
    acquire_fd_lock "$ROOTFS_LOCK_FD"
    lock_rc=$?
    set -e
    if [[ "$lock_rc" != 0 ]]; then
        if [[ "$lock_rc" == 75 ]]; then
            echo "ERROR: timed out waiting for RootFS build lock: $ROOTFS_LOCK" >&2
        fi
        close_lock_fd "$ROOTFS_LOCK_FD" || true
        return "$lock_rc"
    fi
    if ! lock_path_matches_fd "$ROOTFS_LOCK" "$ROOTFS_LOCK_FD"; then
        echo "ERROR: RootFS lock path changed during acquisition" >&2
        close_lock_fd "$ROOTFS_LOCK_FD" || true
        return 74
    fi
    write_lock_owner "$ROOTFS_LOCK_FD" \
        "$ROOTFS_LOCK_OWNER_PID" "$ROOTFS_LOCK_OWNER_START" "$ROOTFS_LOCK_OWNER_TOKEN"
    ROOTFS_LOCK_OWNED=1
}

use_inherited_rootfs_lock() {
    local inherited_token="${ROOTFS_INHERITED_LOCK_TOKEN:-}"
    local inherited_pid="${ROOTFS_INHERITED_LOCK_PID:-}"
    local inherited_start owner
    [[ "$inherited_token" =~ ^[0-9a-f]{48}$ ]] || return 1
    [[ "$inherited_pid" =~ ^[1-9][0-9]*$ ]] || return 1
    inherited_start="$(process_start_identity "$inherited_pid" 2>/dev/null || true)"
    [[ -n "$inherited_start" ]] || return 1
    owner="$(read_lock_owner "$ROOTFS_LOCK" 2>/dev/null || true)"
    [[ "$owner" == "$inherited_pid|$inherited_start|$inherited_token" ]] || return 1
    kernel_lock_is_held "$ROOTFS_LOCK" || return 1
    ROOTFS_LOCK_OWNER_PID="$inherited_pid"
    ROOTFS_LOCK_OWNER_START="$inherited_start"
    ROOTFS_LOCK_OWNER_TOKEN="$inherited_token"
    return 0
}

verify_atomic_primitives() {
    local probe="$BUILD_DIR/.rootfs-rename-probe.$(new_lock_token)"
    local rc=0
    mkdir "$probe" 8>&- 9>&-
    printf 'left\n' > "$probe/left"
    printf 'right\n' > "$probe/right"
    if atomic_rename_noreplace "$probe/left" "$probe/right" 2>/dev/null; then
        echo "ERROR: atomic no-replace primitive overwrote an existing target" >&2
        rc=1
    elif [[ "$(< "$probe/left")" != "left" || "$(< "$probe/right")" != "right" ]]; then
        echo "ERROR: failed no-replace probe changed its operands" >&2
        rc=1
    fi
    if [[ "$rc" == "0" ]] && ! atomic_rename_exchange "$probe/left" "$probe/right"; then
        rc=1
    elif [[ "$rc" == "0" ]] && \
         { [[ "$(< "$probe/left")" != "right" ]] || [[ "$(< "$probe/right")" != "left" ]]; }; then
        echo "ERROR: atomic exchange primitive did not swap files" >&2
        rc=1
    fi
    mkdir "$probe/dir-left" "$probe/dir-right" 8>&- 9>&-
    printf 'left-dir\n' > "$probe/dir-left/value"
    printf 'right-dir\n' > "$probe/dir-right/value"
    if [[ "$rc" == "0" ]] && ! atomic_rename_exchange "$probe/dir-left" "$probe/dir-right"; then
        rc=1
    elif [[ "$rc" == "0" ]] && \
         { [[ "$(< "$probe/dir-left/value")" != "right-dir" ]] || \
           [[ "$(< "$probe/dir-right/value")" != "left-dir" ]]; }; then
        echo "ERROR: atomic exchange primitive did not swap directories" >&2
        rc=1
    fi
    rm -rf -- "$probe" 8>&- 9>&-
    return "$rc"
}

PUBLISH_STAGED_PATHS=()
PUBLISH_TARGET_PATHS=()
PUBLISH_MODES=()
PUBLISH_LABELS=()

publish_component() {
    local label="$1"
    local staged_path="$2"
    local target_path="$3"
    local mode rename_rc=0
    if [[ -e "$target_path" || -L "$target_path" ]]; then
        if [[ -L "$target_path" ]]; then
            echo "ERROR: refusing to exchange a symlinked RootFS artifact: $target_path" >&2
            return 74
        fi
        if [[ "$label" == "fs" && ! -d "$target_path" ]]; then
            echo "ERROR: existing RootFS target is not a directory: $target_path" >&2
            return 74
        fi
        if [[ "$label" != "fs" && ! -f "$target_path" ]]; then
            echo "ERROR: existing RootFS bundle target is not a regular file: $target_path" >&2
            return 74
        fi
        mode="exchange"
    else
        mode="new"
    fi

    # A catchable signal must not land after rename(2) but before the inverse
    # operation is journaled. Defer it for this tiny boundary, record the
    # completed operation, then let the normal EXIT cleanup perform rollback.
    PUBLISH_DEFERRED_EXIT=0
    trap 'PUBLISH_DEFERRED_EXIT=130' INT
    trap 'PUBLISH_DEFERRED_EXIT=143' TERM
    trap 'PUBLISH_DEFERRED_EXIT=129' HUP
    set +e
    if [[ "$mode" == "exchange" ]]; then
        atomic_rename_exchange "$staged_path" "$target_path"
        rename_rc=$?
    else
        atomic_rename_noreplace "$staged_path" "$target_path"
        rename_rc=$?
    fi
    set -e
    if [[ "$rename_rc" == 0 ]]; then
        if [[ "${ROOTFS_TEST_READY_IN_JOURNAL_GAP:-}" == "$label" &&
              -n "${ROOTFS_TEST_READY_FILE:-}" ]]; then
            : > "$ROOTFS_TEST_READY_FILE"
        fi
        if [[ "${ROOTFS_TEST_PAUSE_IN_JOURNAL_GAP:-}" == "$label" ]]; then
            while [[ "$PUBLISH_DEFERRED_EXIT" == 0 &&
                     ! -e "${ROOTFS_TEST_CONTINUE_FILE:-/nonexistent}" ]]; do
                sleep 0.05 8>&- 9>&-
            done
        fi
        PUBLISH_STAGED_PATHS+=("$staged_path")
        PUBLISH_TARGET_PATHS+=("$target_path")
        PUBLISH_MODES+=("$mode")
        PUBLISH_LABELS+=("$label")
    fi
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP
    if [[ "$rename_rc" != 0 ]]; then
        return 74
    fi
    if [[ "$PUBLISH_DEFERRED_EXIT" != 0 ]]; then
        return "$PUBLISH_DEFERRED_EXIT"
    fi

    if [[ "${ROOTFS_TEST_READY_AFTER_PUBLISH:-}" == "$label" && \
          -n "${ROOTFS_TEST_READY_FILE:-}" ]]; then
        : > "$ROOTFS_TEST_READY_FILE"
    fi
    if [[ "${ROOTFS_TEST_PAUSE_AFTER_PUBLISH:-}" == "$label" ]]; then
        while :; do sleep 0.1 8>&- 9>&-; done
    fi
    if [[ "${ROOTFS_TEST_FAIL_AFTER_PUBLISH:-}" == "$label" ]]; then
        echo "ERROR: injected failure after publishing $label" >&2
        return 76
    fi
    return 0
}

rollback_publish_transaction() {
    local index mode staged_path target_path rc=0
    for (( index=${#PUBLISH_MODES[@]} - 1; index >= 0; index-- )); do
        mode="${PUBLISH_MODES[$index]}"
        staged_path="${PUBLISH_STAGED_PATHS[$index]}"
        target_path="${PUBLISH_TARGET_PATHS[$index]}"
        if [[ "$mode" == "exchange" ]]; then
            atomic_rename_exchange "$staged_path" "$target_path" || rc=1
        else
            atomic_rename_noreplace "$target_path" "$staged_path" || rc=1
        fi
    done
    if [[ "$rc" == "0" ]]; then
        PUBLISH_TRANSACTION_ACTIVE=0
    fi
    return "$rc"
}

# Publication depends on no-replace and exchange. Verify both before acquiring
# the long-lived build lock so an unsupported filesystem fails without mutation.
verify_atomic_primitives || exit 74

if [[ -n "${ROOTFS_INHERITED_LOCK_TOKEN:-}" || -n "${ROOTFS_INHERITED_LOCK_PID:-}" ]]; then
    if ! use_inherited_rootfs_lock; then
        echo "ERROR: inherited RootFS lock ownership is invalid" >&2
        exit 75
    fi
else
    acquire_rootfs_lock
fi
if [[ "${ROOTFS_REQUIRE_ABSENT:-0}" == "1" ]] && \
   { [[ -e "$ROOTFS_OUT" || -L "$ROOTFS_OUT" ]] || \
     [[ -e "$TARBALL_OUT" || -L "$TARBALL_OUT" ]] || \
     [[ -e "$SUMS_OUT" || -L "$SUMS_OUT" ]] || \
     [[ -e "$RECEIPT_OUT" || -L "$RECEIPT_OUT" ]]; }; then
    echo "ERROR: refusing to replace existing RootFS bundle in $BUILD_DIR" >&2
    exit 73
fi
if [[ "${ROOTFS_REUSE_VALID:-0}" == "1" ]] && \
   validate_rootfs_bundle "$BUILD_DIR"; then
    echo "==> Reusing validated RootFS bundle: $BUILD_DIR"
    exit 0
fi

# All mutable build products live in one unique directory on BUILD_DIR's
# filesystem. The final directory rename therefore cannot cross volumes.
ROOTFS_STAGE="$(exec 8>&- 9>&-; \
    mktemp -d "$BUILD_DIR/.fs.staging.XXXXXX" 8>&- 9>&-)"
STAGED_ROOTFS="$ROOTFS_STAGE/fs"
WORK="$ROOTFS_STAGE/work-rootfs"
MERGED_TGZ="$ROOTFS_STAGE/rootfs-with-fallback-supervisor.tar.gz"
STAGED_TARBALL="$ROOTFS_STAGE/fs.tar.gz"
STAGED_SUMS="$ROOTFS_STAGE/SHA256SUMS"
STAGED_RECEIPT="$ROOTFS_STAGE/ROOTFS_RECEIPT"
mkdir -p "$BUILD_DIR/dl" "$BUILD_DIR/fakefsify" 8>&- 9>&-

echo "==> [1/5] Downloading Alpine ${ALPINE_VERSION} ${ALPINE_ARCH} minirootfs"
ALPINE_PATH="$BUILD_DIR/dl/$ALPINE_TGZ"
if [[ ! -f "$ALPINE_PATH" ]]; then
    curl -fL --retry 3 -o "$ALPINE_PATH.tmp" "$ALPINE_BASE/$ALPINE_TGZ" 8>&- 9>&-
    mv "$ALPINE_PATH.tmp" "$ALPINE_PATH" 8>&- 9>&-
fi
ACTUAL_SHA="$(exec 8>&- 9>&-; shasum -a 256 "$ALPINE_PATH" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"
echo "    sha256: $ACTUAL_SHA"
if [[ "$ACTUAL_SHA" != "$ALPINE_SHA256" ]]; then
    echo "ERROR: sha256 mismatch (expected $ALPINE_SHA256)" >&2
    exit 1
fi

echo "==> [2/5] Building fakefsify (host tool)"
# The default always comes from the fingerprinted iSH checkout. An external
# binary is accepted only through the explicit, identity-bound override.
if [[ -n "$FAKEFSIFY_OVERRIDE" ]]; then
    FAKEFSIFY_SOURCE_BIN="$FAKEFSIFY_OVERRIDE"
else
    if ! command -v meson >/dev/null; then
        echo "ERROR: meson required to build fakefsify" >&2
        exit 1
    fi

    # Homebrew installs libarchive keg-only on macOS, so pkg-config cannot
    # discover it unless its .pc directory is added explicitly. Honour an
    # existing caller-provided PKG_CONFIG_PATH and only add the Homebrew path
    # when libarchive is otherwise unavailable.
    if command -v pkg-config >/dev/null 2>&1 && \
       ! pkg-config --exists libarchive 8>&- 9>&- >/dev/null 2>&1 && \
       command -v brew >/dev/null 2>&1; then
        LIBARCHIVE_PREFIX="$(exec 8>&- 9>&-; \
            brew --prefix libarchive 8>&- 9>&- 2>/dev/null || true)"
        if [[ -n "$LIBARCHIVE_PREFIX" && -d "$LIBARCHIVE_PREFIX/lib/pkgconfig" ]]; then
            export PKG_CONFIG_PATH="$LIBARCHIVE_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        fi
    fi
    meson setup --reconfigure "$BUILD_DIR/ish-host" "$ISH_SRC" \
        -Dguest_arch="$ISH_GUEST_ARCH" 8>&- 9>&- >/dev/null
    meson compile -C "$BUILD_DIR/ish-host" fakefsify 8>&- 9>&-
    FAKEFSIFY_SOURCE_BIN="$BUILD_DIR/ish-host/tools/fakefsify"
fi
if [[ ! -f "$FAKEFSIFY_SOURCE_BIN" || ! -x "$FAKEFSIFY_SOURCE_BIN" ]]; then
    echo "ERROR: fakefsify was not produced as an executable regular file" >&2
    exit 1
fi
FAKEFSIFY_BIN="$ROOTFS_STAGE/fakefsify"
cp "$FAKEFSIFY_SOURCE_BIN" "$FAKEFSIFY_BIN" 8>&- 9>&-
chmod 0755 "$FAKEFSIFY_BIN" 8>&- 9>&-
FAKEFSIFY_BINARY_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$FAKEFSIFY_BIN" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"
if [[ "$FAKEFSIFY_ORIGIN" == "reviewed-binary-override" && \
      "$FAKEFSIFY_BINARY_SHA256" != "$FAKEFSIFY_INPUT_SHA256" ]] || \
   [[ -n "$FAKEFSIFY_EXPECTED_BINARY_OVERRIDE" && \
      "$FAKEFSIFY_BINARY_SHA256" != "$FAKEFSIFY_EXPECTED_BINARY_OVERRIDE" ]]; then
    echo "ERROR: FAKEFSIFY_BIN changed after its recipe identity was computed" >&2
    exit 74
fi
echo "    using $FAKEFSIFY_BIN"

echo "==> [3/5] Building AArch64 musl supervisor compatibility fallback"
make -C "$PKG_ROOT/supervisor" \
    OUT_DIR="$ROOTFS_STAGE/supervisor" \
    TARGET_TRIPLE=aarch64-linux-musl 8>&- 9>&-
SUP_BIN="$ROOTFS_STAGE/supervisor/ishsv"
if [[ ! -x "$SUP_BIN" ]]; then
    echo "ERROR: supervisor not built" >&2
    exit 1
fi
SUPERVISOR_BINARY_SHA256="$(exec 8>&- 9>&-; shasum -a 256 "$SUP_BIN" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"

echo "==> [4b/5] Adding compatibility fallback /sbin/ishsv"
# Published XCFrameworks normally install their bundled supervisor atomically
# at /sbin/.ishsv-ishembed-sha256-<digest>, verify its bytes and mode, and leave
# this RootFS-owned /sbin/ishsv untouched. This copy exists only so a source
# build without a generated supervisor blob can use its documented fallback,
# or so a caller can explicitly choose /sbin/ishsv as a custom override.
# Repacking before fakefsify gives the fallback consistent fakefs metadata.

mkdir -p "$WORK" 8>&- 9>&-
tar -xzf "$ALPINE_PATH" -C "$WORK" 8>&- 9>&-
mkdir -p "$WORK/sbin" 8>&- 9>&-
cp "$SUP_BIN" "$WORK/sbin/ishsv" 8>&- 9>&-
chmod 755 "$WORK/sbin/ishsv" 8>&- 9>&-

# Pre-seed an inline VM template at /srv/vms/.template inside the
# rootfs. New VMs are created by `cp -a /srv/vms/.template /srv/vms/<n>`
# from inside the guest, which is much faster than re-extracting an
# Alpine tarball through the iSH arm64 emulator.
mkdir -p "$WORK/srv/vms/.template" 8>&- 9>&-
tar -xzf "$ALPINE_PATH" -C "$WORK/srv/vms/.template" 8>&- 9>&-
# Patch the template's network config the same way as the base.
mkdir -p "$WORK/srv/vms/.template/etc" 8>&- 9>&-
cat 8>&- 9>&- > "$WORK/srv/vms/.template/etc/resolv.conf" <<'RESOLV'
nameserver 1.1.1.1
nameserver 8.8.8.8
nameserver 9.9.9.9
options timeout:2 attempts:2
RESOLV
cat 8>&- 9>&- > "$WORK/srv/vms/.template/etc/apk/repositories" <<APK
https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/main
https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/community
APK
echo "ish-vm" > "$WORK/srv/vms/.template/etc/hostname"

# --- DNS ---------------------------------------------------------------
# Alpine minirootfs ships without /etc/resolv.conf. iSH does not have
# DHCP, so apk + any networked tool needs explicit nameservers.
# We pin Cloudflare + Google + Quad9 + plain Cloudflare to maximise
# the chance that one is reachable from any cellular / Wi-Fi network.
mkdir -p "$WORK/etc" 8>&- 9>&-
cat 8>&- 9>&- > "$WORK/etc/resolv.conf" <<'RESOLV'
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
    cat 8>&- 9>&- > "$WORK/etc/hosts" <<'HOSTS'
127.0.0.1	localhost localhost.localdomain
::1		localhost localhost.localdomain ip6-localhost ip6-loopback
HOSTS
fi

# Sane default hostname so the shell prompt isn't blank.
echo "ish" > "$WORK/etc/hostname"

# Pin /etc/apk/repositories to https + main+community for the version
# we tested. Edge / dev versions have shipped libc symbols iSH can't
# emulate; if you need newer packages, try the same Alpine MINOR.
cat 8>&- 9>&- > "$WORK/etc/apk/repositories" <<APK
https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/main
https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_MAJOR}/community
APK

"$ROOTFS_ARCHIVER" \
    --source "$WORK" \
    --arcname . \
    --output "$MERGED_TGZ" \
    --mtime "$ROOTFS_SOURCE_DATE_EPOCH" 8>&- 9>&-

echo "==> [4/5] Importing merged RootFS into staged fakefs"
"$FAKEFSIFY_BIN" "$MERGED_TGZ" "$STAGED_ROOTFS" 8>&- 9>&-

CURRENT_ROOTFS_RECIPE_IDENTITY="$(
    exec 8>&- 9>&-
    ROOTFS_INPUTS_FILE="$ROOTFS_INPUTS_FILE" \
    ISH_SRC="$ISH_SRC" \
    FAKEFSIFY_BIN="$FAKEFSIFY_OVERRIDE" \
    FAKEFSIFY_PROVENANCE_SHA256="$FAKEFSIFY_PROVENANCE_OVERRIDE" \
    FAKEFSIFY_EXPECTED_BINARY_SHA256="$FAKEFSIFY_EXPECTED_BINARY_OVERRIDE" \
    ALPINE_VERSION="$ALPINE_VERSION" \
    ALPINE_ARCH="$ALPINE_ARCH" \
    ALPINE_SHA256="$ALPINE_SHA256" \
        "$0" --print-identity 8>&- 9>&-
)"
if [[ "$CURRENT_ROOTFS_RECIPE_IDENTITY" != "$ROOTFS_EXPECTED_RECIPE_IDENTITY" ]]; then
    echo "ERROR: RootFS recipe inputs changed while the staged image was being built" >&2
    exit 74
fi

# Seal the exact initial meta.db/data bytes, then bind stable critical payloads
# separately so a mutable, subsequently-run fakefs can still be reused after a
# full database/storage consistency check.
validate_fakefs_layout "$STAGED_ROOTFS"
BUSYBOX_BINARY_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$STAGED_ROOTFS/data/bin/busybox" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"
ROOTFS_INITIAL_META_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$STAGED_ROOTFS/meta.db" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"
ROOTFS_INITIAL_DATA_SHA256="$(exec 8>&- 9>&-; rootfs_data_sha256 "$STAGED_ROOTFS")"
ROOTFS_INITIAL_CONTENT_SHA256="$(
    exec 8>&- 9>&-
    rootfs_content_sha256 "$ROOTFS_INITIAL_META_SHA256" "$ROOTFS_INITIAL_DATA_SHA256"
)"

ROOTFS_IDENTITY_PATH="$STAGED_ROOTFS/$ROOTFS_IDENTITY_NAME"
ROOTFS_IDENTITY_TMP="$(exec 8>&- 9>&-; \
    mktemp "$STAGED_ROOTFS/${ROOTFS_IDENTITY_NAME}.tmp.XXXXXX" 8>&- 9>&-)"
{
    print_rootfs_recipe_identity
    printf 'SUPERVISOR_BINARY_SHA256=%s\n' "$SUPERVISOR_BINARY_SHA256"
    printf 'BUSYBOX_BINARY_SHA256=%s\n' "$BUSYBOX_BINARY_SHA256"
    printf 'ROOTFS_INITIAL_META_SHA256=%s\n' "$ROOTFS_INITIAL_META_SHA256"
    printf 'ROOTFS_INITIAL_DATA_SHA256=%s\n' "$ROOTFS_INITIAL_DATA_SHA256"
    printf 'ROOTFS_INITIAL_CONTENT_SHA256=%s\n' "$ROOTFS_INITIAL_CONTENT_SHA256"
} > "$ROOTFS_IDENTITY_TMP"
chmod 0644 "$ROOTFS_IDENTITY_TMP" 8>&- 9>&-
mv -f "$ROOTFS_IDENTITY_TMP" "$ROOTFS_IDENTITY_PATH" 8>&- 9>&-

if ! validate_rootfs "$STAGED_ROOTFS" 1; then
    echo "ERROR: staged RootFS failed final validation; existing output is unchanged" >&2
    exit 74
fi

echo "==> [5/5] Packaging tarball + checksums"
"$ROOTFS_ARCHIVER" \
    --source "$STAGED_ROOTFS" \
    --arcname fs \
    --output "$STAGED_TARBALL" \
    --mtime "$ROOTFS_SOURCE_DATE_EPOCH" 8>&- 9>&-
STAGED_TARBALL_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$STAGED_TARBALL" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"
printf '%s  fs.tar.gz\n' "$STAGED_TARBALL_SHA256" > "$STAGED_SUMS"
STAGED_SUMS_SHA256="$(exec 8>&- 9>&-; shasum -a 256 "$STAGED_SUMS" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"
STAGED_IDENTITY_SHA256="$(exec 8>&- 9>&-; \
    shasum -a 256 "$ROOTFS_IDENTITY_PATH" 8>&- 9>&- | \
    awk '{print $1}' 8>&- 9>&-)"
{
    printf 'ROOTFS_RECEIPT_SCHEMA=1\n'
    printf 'ROOTFS_RECIPE_SHA256=%s\n' "$ROOTFS_RECIPE_SHA256"
    printf 'ROOTFS_IDENTITY_SHA256=%s\n' "$STAGED_IDENTITY_SHA256"
    printf 'ROOTFS_TARBALL_SHA256=%s\n' "$STAGED_TARBALL_SHA256"
    printf 'ROOTFS_SUMS_SHA256=%s\n' "$STAGED_SUMS_SHA256"
} > "$STAGED_RECEIPT"
chmod 0644 "$STAGED_SUMS" "$STAGED_RECEIPT" 8>&- 9>&-

# Exchange keeps build/fs continuously present for replacements. The receipt
# is published last and is the commit point for fs + tarball + sums. Until then,
# a failure or signal reverses every completed operation in cleanup_build.
PUBLISH_TRANSACTION_ACTIVE=1
publish_component fs "$STAGED_ROOTFS" "$ROOTFS_OUT" || exit $?
publish_component tar "$STAGED_TARBALL" "$TARBALL_OUT" || exit $?
publish_component sums "$STAGED_SUMS" "$SUMS_OUT" || exit $?
publish_component receipt "$STAGED_RECEIPT" "$RECEIPT_OUT" || exit $?

if ! validate_rootfs "$ROOTFS_OUT" 1 || ! validate_rootfs_bundle "$BUILD_DIR"; then
    echo "ERROR: committed RootFS bundle failed final validation" >&2
    exit 74
fi
PUBLISH_TRANSACTION_ACTIVE=0
ROOTFS_IDENTITY_PATH="$ROOTFS_OUT/$ROOTFS_IDENTITY_NAME"

echo
echo "Done."
echo "  Rootfs dir: $ROOTFS_OUT"
echo "  Identity:   $ROOTFS_IDENTITY_PATH"
echo "  Tarball:    $TARBALL_OUT"
echo "  Checksums:  $SUMS_OUT"
echo "  Receipt:    $RECEIPT_OUT"
echo
echo "Before distribution, independently review the RootFS provenance,"
echo "licenses, final size, and SHA-256. Install it transactionally into a"
echo "writable sandbox, then pass that directory to IshInstance.boot(rootfsPath:)."
