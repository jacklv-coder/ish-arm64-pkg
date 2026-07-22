#!/usr/bin/env bash
# Transactionally produce build/fs-codex from the validated clean RootFS.
#
# The derived identity binds the exact clean RootFS marker, clean transaction
# receipt, package request, provision script, provision binary and verifier.
# Reuse also reads the installed package.json and checks its actual version.
#
# Usage:
#   scripts/provision-codex-rootfs.sh
#   scripts/provision-codex-rootfs.sh --verify
#
# Env knobs:
#   CODEX_VERSION   exact version or npm tag (default: latest)
#   CODEX_PKG       package name (default: @openai/codex)
#   CODEX_VM_NAME   VM directory below /srv/vms (default: codex)
#   CODEX_BIN_NAME  installed command name (default: package basename)
#   FORCE=1         rebuild and atomically replace a valid derived tree

set -euo pipefail

repo="$(cd "$(dirname "$0" 8>&- 9>&-)/.." && pwd)"
build_dir="$repo/build"
src="$build_dir/fs"
dst="$build_dir/fs-codex"
log="$build_dir/fs-codex.provision.log"
provision_bin="$repo/build-host/provision_codex"
rootfs_builder="$repo/scripts/build-rootfs.sh"
rootfs_identity_name=".ishembed-rootfs-identity"
codex_identity_name=".ishembed-codex-identity"
rootfs_lock="$build_dir/.rootfs-build.lock"
codex_lock="$build_dir/.fs-codex.lock"
mode="provision"

case "${1:-}" in
    "") ;;
    --verify)
        [[ $# -eq 1 ]] || {
            echo "ERROR: --verify does not accept additional arguments" >&2
            exit 64
        }
        mode="verify"
        ;;
    -h|--help)
        sed -n '2,/^$/p' "$0" 8>&- 9>&-
        exit 0
        ;;
    *)
        echo "ERROR: unknown argument: $1" >&2
        exit 64
        ;;
esac

case "${FORCE:-0}" in
    0|1) ;;
    *)
        echo "ERROR: FORCE must be 0 or 1" >&2
        exit 64
        ;;
esac

CODEX_PKG="${CODEX_PKG:-@openai/codex}"
CODEX_VERSION_RAW="${CODEX_VERSION:-}"
CODEX_VM_NAME="${CODEX_VM_NAME:-codex}"
CODEX_BIN_NAME="${CODEX_BIN_NAME:-${CODEX_PKG##*/}}"
if [[ -z "$CODEX_VERSION_RAW" || "$CODEX_VERSION_RAW" == "latest" ]]; then
    CODEX_REQUESTED_VERSION="latest"
    CODEX_INSTALL_VERSION=""
else
    CODEX_REQUESTED_VERSION="$CODEX_VERSION_RAW"
    CODEX_INSTALL_VERSION="$CODEX_VERSION_RAW"
fi

# Validate every value before it reaches provision_codex.c, where it is used in
# guest shell commands. The emitted values are the package's node_modules
# suffix and whether the request is an exact SemVer or an npm dist-tag.
CODEX_VALIDATED_INPUTS="$(python3 - \
    "$CODEX_PKG" "$CODEX_REQUESTED_VERSION" "$CODEX_VM_NAME" "$CODEX_BIN_NAME" \
    8>&- 9>&- <<'PY'
import re
import sys

package, version, vm_name, bin_name = sys.argv[1:]
package_re = re.compile(
    r"(?:@[A-Za-z0-9][A-Za-z0-9._-]{0,63}/)?"
    r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}"
)
version_re = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,127}")
name_re = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
semver_re = re.compile(
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
)
if not package_re.fullmatch(package) or any(
    part in (".", "..") for part in package.split("/")
):
    print("ERROR: CODEX_PKG is not a safe npm package name", file=sys.stderr)
    raise SystemExit(64)
if not version_re.fullmatch(version):
    print("ERROR: CODEX_VERSION is not a safe exact version or tag", file=sys.stderr)
    raise SystemExit(64)
for label, value in (("CODEX_VM_NAME", vm_name), ("CODEX_BIN_NAME", bin_name)):
    if not name_re.fullmatch(value) or value in (".", ".."):
        print(f"ERROR: {label} contains unsafe characters", file=sys.stderr)
        raise SystemExit(64)
semver = semver_re.fullmatch(version)
if semver:
    prerelease = semver.group(4)
    if prerelease and any(
        identifier.isdigit() and len(identifier) > 1 and identifier.startswith("0")
        for identifier in prerelease.split(".")
    ):
        semver = None
print(package + "|" + ("exact" if semver else "tag"))
PY
)"
CODEX_PACKAGE_RELATIVE="${CODEX_VALIDATED_INPUTS%|*}"
CODEX_REQUEST_KIND="${CODEX_VALIDATED_INPUTS##*|}"
unset CODEX_VALIDATED_INPUTS

mkdir -p "$build_dir" 8>&- 9>&-
build_dir="$(cd "$build_dir" && pwd -P)"
src="$build_dir/fs"
dst="$build_dir/fs-codex"
log="$build_dir/fs-codex.provision.log"
rootfs_lock="$build_dir/.rootfs-build.lock"
codex_lock="$build_dir/.fs-codex.lock"

OWNED_LOCK_PATHS=()
OWNED_LOCK_FDS=()
OWNED_LOCK_TOKENS=()
OWNED_LOCK_PIDS=()
OWNED_LOCK_STARTS=()
ACQUIRED_LOCK_TOKEN=""
CODEX_STAGE=""
CODEX_PUBLISH_ACTIVE=0
CODEX_PUBLISH_MODE=""
CODEX_ROLLBACK_FAILED=0
CODEX_DEFERRED_EXIT=0

atomic_generation_rename() {
    local operation="$1"
    local source_path="$2"
    local destination_path="$3"
    python3 - "$operation" "$source_path" "$destination_path" \
        8>&- 9>&- <<'PY'
import ctypes
import errno
import os
import signal
import sys

operation = sys.argv[1]
source = os.fsencode(sys.argv[2])
destination = os.fsencode(sys.argv[3])
linux_flags = {"noreplace": 1, "exchange": 2}
if operation not in linux_flags:
    raise SystemExit("invalid atomic rename operation")
libc = ctypes.CDLL(None, use_errno=True)
signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGHUP})
if sys.platform == "darwin":
    flags = {"noreplace": 0x00000004, "exchange": 0x00000002}
    rename = libc.renamex_np
    rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    result = rename(source, destination, flags[operation])
elif sys.platform.startswith("linux") and hasattr(libc, "renameat2"):
    rename = libc.renameat2
    rename.argtypes = [
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    ]
    rename.restype = ctypes.c_int
    result = rename(-100, source, -100, destination, linux_flags[operation])
else:
    print("ERROR: required atomic rename primitive is unavailable", file=sys.stderr)
    raise SystemExit(74)
if result != 0:
    error = ctypes.get_errno()
    if operation == "noreplace" and error in (errno.EEXIST, errno.ENOTEMPTY):
        raise SystemExit(17)
    print(
        f"ERROR: atomic {operation} rename failed: " + os.strerror(error),
        file=sys.stderr,
    )
    raise SystemExit(74)
PY
}

atomic_generation_noreplace() {
    atomic_generation_rename noreplace "$1" "$2"
}

atomic_generation_exchange() {
    atomic_generation_rename exchange "$1" "$2"
}

generation_process_start() {
    local process_pid="$1"
    python3 - "$process_pid" 8>&- 9>&- <<'PY'
import hashlib
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
        fields = raw[raw.rfind(b")") + 2:].split()
        start_ticks = int(fields[19])
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

new_generation_token() {
    python3 - 8>&- 9>&- <<'PY'
import secrets
print(secrets.token_hex(24))
PY
}

read_generation_lock_owner() {
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
keys = ("LOCK_SCHEMA", "PID", "START_ID", "TOKEN")
if len(lines) != len(keys):
    raise SystemExit(1)
values = {}
for line, key in zip(lines, keys):
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

generation_lock_path_matches_fd() {
    local lock_path="$1"
    local lock_fd="$2"
    python3 - "$lock_path" "$lock_fd" <<'PY'
import os
import stat
import sys

try:
    path_info = os.lstat(sys.argv[1])
    lock_fd = int(sys.argv[2])
    for inherited_fd in (8, 9):
        if inherited_fd != lock_fd:
            try:
                os.close(inherited_fd)
            except OSError:
                pass
    fd_info = os.fstat(lock_fd)
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

acquire_generation_fd_lock() {
    local lock_fd="$1"
    local timeout_seconds="${GENERATION_LOCK_TIMEOUT_SECONDS:-300}"
    [[ "$timeout_seconds" =~ ^[0-9]+$ ]] || {
        echo "ERROR: GENERATION_LOCK_TIMEOUT_SECONDS must be a non-negative integer" >&2
        return 64
    }
    python3 - "$lock_fd" "$timeout_seconds" <<'PY'
import errno
import fcntl
import os
import sys
import time

fd = int(sys.argv[1])
for inherited_fd in (8, 9):
    if inherited_fd != fd:
        try:
            os.close(inherited_fd)
        except OSError:
            pass
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

write_generation_lock_owner() {
    local lock_fd="$1"
    local owner_pid="$2"
    local owner_start="$3"
    local owner_token="$4"
    python3 - "$lock_fd" "$owner_pid" "$owner_start" "$owner_token" <<'PY'
import os
import re
import sys

fd = int(sys.argv[1])
for inherited_fd in (8, 9):
    if inherited_fd != fd:
        try:
            os.close(inherited_fd)
        except OSError:
            pass
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

kernel_generation_lock_is_held() {
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
    if not stat.S_ISREG(os.fstat(fd).st_mode):
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

close_generation_lock_fd() {
    local lock_fd="$1"
    case "$lock_fd" in
        8) exec 8>&- ;;
        9) exec 9>&- ;;
        *) return 1 ;;
    esac
}

verify_generation_rename_primitives() {
    local probe="$build_dir/.codex-lock-probe.$(new_generation_token)"
    local rc=0 rename_rc
    mkdir "$probe" 8>&- 9>&-
    printf 'left\n' > "$probe/left"
    printf 'right\n' > "$probe/right"
    set +e
    atomic_generation_noreplace "$probe/left" "$probe/right" 2>/dev/null
    rename_rc=$?
    set -e
    if [[ "$rename_rc" != 17 || "$(< "$probe/left")" != "left" || \
          "$(< "$probe/right")" != "right" ]]; then
        echo "ERROR: atomic no-replace generation-lock probe failed" >&2
        rc=1
    elif ! atomic_generation_exchange "$probe/left" "$probe/right"; then
        rc=1
    elif [[ "$(< "$probe/left")" != "right" || \
            "$(< "$probe/right")" != "left" ]]; then
        echo "ERROR: atomic exchange generation-lock probe failed" >&2
        rc=1
    fi
    mkdir "$probe/dir-left" "$probe/dir-right" 8>&- 9>&-
    printf 'left-dir\n' > "$probe/dir-left/value"
    printf 'right-dir\n' > "$probe/dir-right/value"
    if [[ "$rc" == 0 ]] && \
       ! atomic_generation_exchange "$probe/dir-left" "$probe/dir-right"; then
        rc=1
    elif [[ "$rc" == 0 && \
            ("$(< "$probe/dir-left/value")" != "right-dir" || \
             "$(< "$probe/dir-right/value")" != "left-dir") ]]; then
        echo "ERROR: atomic directory-exchange probe failed" >&2
        rc=1
    fi
    rm -rf -- "$probe" 8>&- 9>&-
    return "$rc"
}

acquire_generation_lock() {
    local lock_path="$1"
    local lock_label="$2"
    local owner_pid owner_start owner_token lock_fd lock_rc
    owner_pid="$$"
    owner_start="$(generation_process_start "$owner_pid")"
    owner_token="$(new_generation_token)"
    if [[ -L "$lock_path" || ( -e "$lock_path" && ! -f "$lock_path" ) ]]; then
        echo "ERROR: $lock_label lock path is not a regular file: $lock_path" >&2
        return 74
    fi
    case "${#OWNED_LOCK_FDS[@]}" in
        0) lock_fd=8; exec 8<>"$lock_path" || return 74 ;;
        1) lock_fd=9; exec 9<>"$lock_path" || return 74 ;;
        *) echo "ERROR: no reserved generation-lock descriptor is available" >&2; return 74 ;;
    esac
    if ! generation_lock_path_matches_fd "$lock_path" "$lock_fd"; then
        echo "ERROR: $lock_label lock path changed while it was opened" >&2
        close_generation_lock_fd "$lock_fd" || true
        return 74
    fi
    set +e
    acquire_generation_fd_lock "$lock_fd"
    lock_rc=$?
    set -e
    if [[ "$lock_rc" != 0 ]]; then
        [[ "$lock_rc" != 75 ]] || \
            echo "ERROR: timed out waiting for generation lock: $lock_path" >&2
        close_generation_lock_fd "$lock_fd" || true
        return "$lock_rc"
    fi
    if ! generation_lock_path_matches_fd "$lock_path" "$lock_fd"; then
        echo "ERROR: $lock_label lock path changed during acquisition" >&2
        close_generation_lock_fd "$lock_fd" || true
        return 74
    fi
    write_generation_lock_owner "$lock_fd" "$owner_pid" "$owner_start" "$owner_token"
    OWNED_LOCK_PATHS+=("$lock_path")
    OWNED_LOCK_FDS+=("$lock_fd")
    OWNED_LOCK_TOKENS+=("$owner_token")
    OWNED_LOCK_PIDS+=("$owner_pid")
    OWNED_LOCK_STARTS+=("$owner_start")
    ACQUIRED_LOCK_TOKEN="$owner_token"
}

verify_inherited_lock() {
    local lock_path="$1"
    local inherited_pid="$2"
    local inherited_token="$3"
    local inherited_start owner
    [[ "$inherited_pid" =~ ^[1-9][0-9]*$ ]] || return 1
    [[ "$inherited_token" =~ ^[0-9a-f]{48}$ ]] || return 1
    inherited_start="$(generation_process_start "$inherited_pid" 2>/dev/null || true)"
    [[ -n "$inherited_start" ]] || return 1
    owner="$(read_generation_lock_owner "$lock_path" 2>/dev/null || true)"
    [[ "$owner" == "$inherited_pid|$inherited_start|$inherited_token" ]] || return 1
    kernel_generation_lock_is_held "$lock_path"
}

release_generation_locks() {
    local index lock_path lock_fd expected owner rc=0
    for (( index=${#OWNED_LOCK_PATHS[@]} - 1; index >= 0; index-- )); do
        lock_path="${OWNED_LOCK_PATHS[$index]}"
        lock_fd="${OWNED_LOCK_FDS[$index]}"
        expected="${OWNED_LOCK_PIDS[$index]}|${OWNED_LOCK_STARTS[$index]}|${OWNED_LOCK_TOKENS[$index]}"
        owner="$(read_generation_lock_owner "$lock_path" 2>/dev/null || true)"
        if ! generation_lock_path_matches_fd "$lock_path" "$lock_fd" ||
           [[ "$owner" != "$expected" ]]; then
            echo "ERROR: refusing to release lock with changed ownership: $lock_path" >&2
            rc=1
        fi
        close_generation_lock_fd "$lock_fd" || rc=1
    done
    OWNED_LOCK_PATHS=()
    OWNED_LOCK_FDS=()
    return "$rc"
}

identity_value() {
    local identity_path="$1"
    local key="$2"
    awk -F= -v wanted="$key" \
        '$1 == wanted { count += 1; value = substr($0, length($1) + 2) }
         END { if (count == 1) print value; else exit 1 }' "$identity_path" \
        8>&- 9>&-
}

clean_rootfs_data_sha256() {
    local rootfs_path="$1"
    python3 - "$rootfs_path/data" 8>&- 9>&- <<'PY'
import hashlib
import os
import stat
import sys

root = os.fsencode(os.path.realpath(sys.argv[1]))
entries = []
for directory, names, files in os.walk(root, topdown=True, followlinks=False):
    names.sort()
    files.sort()
    for name in names + files:
        full = os.path.join(directory, name)
        entries.append((os.path.relpath(full, root), full))
digest = hashlib.sha256()
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
        raise SystemExit("unsafe non-file entry in clean fakefs data")
print(digest.hexdigest())
PY
}

refresh_codex_inputs() {
    local clean_identity="$src/$rootfs_identity_name"
    local clean_receipt="$build_dir/ROOTFS_RECEIPT"
    for input_path in "$clean_identity" "$clean_receipt" "$0" \
        "$provision_bin" "$rootfs_builder"; do
        if [[ ! -f "$input_path" || -L "$input_path" ]]; then
            echo "ERROR: missing or unsafe Codex provision input: $input_path" >&2
            return 1
        fi
    done
    CLEAN_ROOTFS_IDENTITY_SHA256="$(shasum -a 256 "$clean_identity" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
    CLEAN_ROOTFS_RECEIPT_SHA256="$(shasum -a 256 "$clean_receipt" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
    CLEAN_ROOTFS_META_SHA256="$(shasum -a 256 "$src/meta.db" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
    CLEAN_ROOTFS_DATA_SHA256="$(clean_rootfs_data_sha256 "$src")"
    CLEAN_ROOTFS_CONTENT_SHA256="$({
        printf 'meta.db=%s\n' "$CLEAN_ROOTFS_META_SHA256"
        printf 'data=%s\n' "$CLEAN_ROOTFS_DATA_SHA256"
    } 8>&- 9>&- | shasum -a 256 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
    CODEX_PROVISION_SCRIPT_SHA256="$(shasum -a 256 "$0" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
    CODEX_PROVISION_BINARY_SHA256="$(shasum -a 256 "$provision_bin" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
    ROOTFS_VERIFIER_SHA256="$(shasum -a 256 "$rootfs_builder" 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
    CODEX_INPUT_SHA256="$({
        printf 'CLEAN_ROOTFS_IDENTITY_SHA256=%s\n' "$CLEAN_ROOTFS_IDENTITY_SHA256"
        printf 'CLEAN_ROOTFS_RECEIPT_SHA256=%s\n' "$CLEAN_ROOTFS_RECEIPT_SHA256"
        printf 'CLEAN_ROOTFS_META_SHA256=%s\n' "$CLEAN_ROOTFS_META_SHA256"
        printf 'CLEAN_ROOTFS_DATA_SHA256=%s\n' "$CLEAN_ROOTFS_DATA_SHA256"
        printf 'CLEAN_ROOTFS_CONTENT_SHA256=%s\n' "$CLEAN_ROOTFS_CONTENT_SHA256"
        printf 'CODEX_PKG=%s\n' "$CODEX_PKG"
        printf 'CODEX_REQUESTED_VERSION=%s\n' "$CODEX_REQUESTED_VERSION"
        printf 'CODEX_REQUEST_KIND=%s\n' "$CODEX_REQUEST_KIND"
        printf 'CODEX_VM_NAME=%s\n' "$CODEX_VM_NAME"
        printf 'CODEX_BIN_NAME=%s\n' "$CODEX_BIN_NAME"
        printf 'CODEX_PROVISION_SCRIPT_SHA256=%s\n' "$CODEX_PROVISION_SCRIPT_SHA256"
        printf 'CODEX_PROVISION_BINARY_SHA256=%s\n' "$CODEX_PROVISION_BINARY_SHA256"
        printf 'ROOTFS_VERIFIER_SHA256=%s\n' "$ROOTFS_VERIFIER_SHA256"
    } 8>&- 9>&- | shasum -a 256 8>&- 9>&- | awk '{print $1}' 8>&- 9>&-)"
}

print_expected_codex_identity() {
    printf 'CODEX_ROOTFS_IDENTITY_SCHEMA=2\n'
    printf 'CLEAN_ROOTFS_IDENTITY_SHA256=%s\n' "$CLEAN_ROOTFS_IDENTITY_SHA256"
    printf 'CLEAN_ROOTFS_RECEIPT_SHA256=%s\n' "$CLEAN_ROOTFS_RECEIPT_SHA256"
    printf 'CLEAN_ROOTFS_META_SHA256=%s\n' "$CLEAN_ROOTFS_META_SHA256"
    printf 'CLEAN_ROOTFS_DATA_SHA256=%s\n' "$CLEAN_ROOTFS_DATA_SHA256"
    printf 'CLEAN_ROOTFS_CONTENT_SHA256=%s\n' "$CLEAN_ROOTFS_CONTENT_SHA256"
    printf 'CODEX_PKG=%s\n' "$CODEX_PKG"
    printf 'CODEX_REQUESTED_VERSION=%s\n' "$CODEX_REQUESTED_VERSION"
    printf 'CODEX_REQUEST_KIND=%s\n' "$CODEX_REQUEST_KIND"
    printf 'CODEX_VM_NAME=%s\n' "$CODEX_VM_NAME"
    printf 'CODEX_BIN_NAME=%s\n' "$CODEX_BIN_NAME"
    printf 'CODEX_PROVISION_SCRIPT_SHA256=%s\n' "$CODEX_PROVISION_SCRIPT_SHA256"
    printf 'CODEX_PROVISION_BINARY_SHA256=%s\n' "$CODEX_PROVISION_BINARY_SHA256"
    printf 'ROOTFS_VERIFIER_SHA256=%s\n' "$ROOTFS_VERIFIER_SHA256"
    printf 'CODEX_INPUT_SHA256=%s\n' "$CODEX_INPUT_SHA256"
}

installed_package_version() {
    local rootfs_path="$1"
    local package_json="$rootfs_path/data/srv/vms/$CODEX_VM_NAME/usr/local/lib/node_modules/$CODEX_PACKAGE_RELATIVE/package.json"
    python3 - "$rootfs_path" "$package_json" "$CODEX_PKG" \
        "$CODEX_VM_NAME" "$CODEX_BIN_NAME" "$CODEX_PACKAGE_RELATIVE" \
        8>&- 9>&- <<'PY'
import json
import os
import re
import sqlite3
import stat
import struct
import sys

rootfs, path, expected_name, vm_name, bin_name, package_relative = sys.argv[1:]


def safe_backing_file(candidate, label):
    data_root = os.path.join(rootfs, "data")
    relative = os.path.relpath(candidate, data_root)
    if relative == os.pardir or relative.startswith(os.pardir + os.sep):
        raise ValueError(f"{label} escapes fakefs data")
    current = data_root
    root_info = os.lstat(current)
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise ValueError("fakefs data is not a safe directory")
    parts = relative.split(os.sep)
    for component in parts[:-1]:
        if component in ("", ".", ".."):
            raise ValueError(f"{label} has an unsafe backing path")
        current = os.path.join(current, component)
        info = os.lstat(current)
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
            raise ValueError(f"{label} has an unsafe backing parent")
    info = os.lstat(candidate)
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise ValueError(f"{label} is not a safe backing file")
    return info


def normalize_bin_target(value):
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError("package.json bin target is missing or unsafe")
    if value.startswith("./"):
        value = value[2:]
    components = value.split("/")
    if (
        not value
        or value.startswith("/")
        or "\\" in value
        or any(component in ("", ".", "..") for component in components)
        or any(any(ord(character) < 32 for character in component)
               for component in components)
    ):
        raise ValueError("package.json bin target is not a safe relative path")
    return "/".join(components)


def guest_stat(database, guest_path, label):
    rows = database.execute(
        "select stats.inode, stats.stat from paths "
        "join stats on stats.inode = paths.inode where paths.path = ?",
        (sqlite3.Binary(guest_path.encode("utf-8")),),
    ).fetchall()
    if len(rows) != 1:
        raise ValueError(f"{label} is missing from fakefs metadata")
    inode, raw = rows[0]
    if not isinstance(inode, int) or inode <= 0 or not isinstance(raw, bytes) or len(raw) != 16:
        raise ValueError(f"{label} has invalid fakefs metadata")
    mode = struct.unpack_from("<I", raw)[0]
    if mode & 0o111 == 0:
        raise ValueError(f"{label} is not guest-executable")
    return inode, mode


try:
    package_info = safe_backing_file(path, "package.json")
    if package_info.st_size > 8 * 1024 * 1024:
        raise ValueError("package.json is unreasonably large")
    with open(path, "r", encoding="utf-8") as stream:
        package = json.load(stream)
    name = package.get("name")
    version = package.get("version")
    if name != expected_name:
        raise ValueError("installed package name does not match CODEX_PKG")
    if not isinstance(version, str) or not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9._+-]{0,255}", version
    ):
        raise ValueError("installed package version is missing or unsafe")
    bin_mapping = package.get("bin")
    if isinstance(bin_mapping, str):
        if bin_name != expected_name.rsplit("/", 1)[-1]:
            raise ValueError("package.json string bin does not map CODEX_BIN_NAME")
        bin_target = bin_mapping
    elif isinstance(bin_mapping, dict):
        bin_target = bin_mapping.get(bin_name)
    else:
        raise ValueError("package.json does not define a usable bin mapping")
    bin_target = normalize_bin_target(bin_target)

    package_dir = os.path.dirname(path)
    target_backing = os.path.join(package_dir, *bin_target.split("/"))
    target_info = safe_backing_file(target_backing, "package bin target")
    global_backing = os.path.join(
        rootfs, "data", "srv", "vms", vm_name, "usr", "local", "bin", bin_name
    )
    global_info = safe_backing_file(global_backing, "global command entry")

    package_guest_dir = (
        f"/srv/vms/{vm_name}/usr/local/lib/node_modules/{package_relative}"
    )
    target_guest = package_guest_dir + "/" + bin_target
    global_guest = f"/srv/vms/{vm_name}/usr/local/bin/{bin_name}"
    database_path = os.path.join(rootfs, "meta.db")
    database_info = os.lstat(database_path)
    if stat.S_ISLNK(database_info.st_mode) or not stat.S_ISREG(database_info.st_mode):
        raise ValueError("fakefs metadata database is unsafe")
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    try:
        target_inode, target_mode = guest_stat(
            database, target_guest, "package bin target"
        )
        if stat.S_IFMT(target_mode) != stat.S_IFREG:
            raise ValueError("package bin target is not a guest regular file")
        global_inode, global_mode = guest_stat(
            database, global_guest, "global command entry"
        )
    finally:
        database.close()

    if stat.S_IFMT(global_mode) == stat.S_IFLNK:
        if global_info.st_size > 4096:
            raise ValueError("global command symlink target is unreasonably large")
        with open(global_backing, "rb") as stream:
            raw_target = stream.read()
        try:
            link_target = raw_target.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValueError("global command symlink target is not UTF-8") from error
        if not link_target or "\x00" in link_target:
            raise ValueError("global command symlink target is unsafe")
        vm_guest_root = f"/srv/vms/{vm_name}"
        if link_target.startswith("/"):
            # The entry is used from inside the VM chroot, so an absolute
            # symlink is rooted at that VM rather than at the outer fakefs.
            resolved_guest = os.path.normpath(vm_guest_root + link_target)
        else:
            resolved_guest = os.path.normpath(
                os.path.join(os.path.dirname(global_guest), link_target)
            )
        if (
            resolved_guest != target_guest
            or not resolved_guest.startswith(vm_guest_root + "/")
        ):
            raise ValueError("global command symlink does not resolve to package bin")
    elif stat.S_IFMT(global_mode) == stat.S_IFREG:
        if (
            global_inode != target_inode
            or (global_info.st_dev, global_info.st_ino)
            != (target_info.st_dev, target_info.st_ino)
        ):
            raise ValueError("global regular command is not the package bin hard link")
    else:
        raise ValueError("global command entry has an unsafe guest file type")
except (
    OSError,
    ValueError,
    TypeError,
    AttributeError,
    json.JSONDecodeError,
    sqlite3.Error,
) as error:
    print("ERROR: cannot verify installed Codex package: " + str(error), file=sys.stderr)
    raise SystemExit(1)
print(version)
PY
}

validate_codex_rootfs() {
    local rootfs_path="$1"
    local identity_path="$rootfs_path/$codex_identity_name"
    local expected_prefix expected_lines total_lines recorded_version actual_version
    if ! ROOTFS_INPUTS_FILE="${ROOTFS_INPUTS_FILE:-$repo/scripts/alpine-rootfs-pin.sh}" \
         ISH_SRC="${ISH_SRC:-$repo/third_party/ish}" \
         "$rootfs_builder" --verify-rootfs "$rootfs_path" 8>&- 9>&-; then
        return 1
    fi
    if [[ ! -f "$identity_path" || -L "$identity_path" ]]; then
        echo "ERROR: missing or unsafe Codex RootFS identity: $identity_path" >&2
        return 1
    fi
    if ! cmp -s "$rootfs_path/$rootfs_identity_name" \
        "$src/$rootfs_identity_name" 8>&- 9>&-; then
        echo "ERROR: Codex RootFS was derived from a different clean RootFS marker" >&2
        return 1
    fi
    expected_prefix="$(print_expected_codex_identity)"
    expected_lines="$(printf '%s\n' "$expected_prefix" 8>&- 9>&- | wc -l 8>&- 9>&- | tr -d ' ' 8>&- 9>&-)"
    if ! cmp -s \
        <(sed -n "1,${expected_lines}p" "$identity_path" 8>&- 9>&-) \
        <(printf '%s\n' "$expected_prefix" 8>&- 9>&-) 8>&- 9>&-; then
        echo "ERROR: Codex RootFS provision identity does not match current inputs" >&2
        return 1
    fi
    total_lines="$(wc -l < "$identity_path" 8>&- 9>&- | tr -d ' ' 8>&- 9>&-)"
    if (( total_lines != expected_lines + 1 )); then
        echo "ERROR: Codex RootFS identity has an invalid field set" >&2
        return 1
    fi
    recorded_version="$(identity_value "$identity_path" CODEX_ACTUAL_VERSION || true)"
    if [[ ! "$recorded_version" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,255}$ ]]; then
        echo "ERROR: Codex RootFS identity has an invalid actual version" >&2
        return 1
    fi
    actual_version="$(installed_package_version "$rootfs_path")" || return 1
    if [[ "$actual_version" != "$recorded_version" ]]; then
        echo "ERROR: installed Codex version does not match its identity" >&2
        return 1
    fi
    if [[ "$CODEX_REQUEST_KIND" == "exact" &&
          "$actual_version" != "$CODEX_REQUESTED_VERSION" ]]; then
        echo "ERROR: installed Codex version does not match exact request" >&2
        return 1
    fi
    return 0
}

rollback_codex_publish() {
    [[ "$CODEX_PUBLISH_ACTIVE" == 1 ]] || return 0
    if [[ "$CODEX_PUBLISH_MODE" == "exchange" ]]; then
        atomic_generation_exchange "$CODEX_STAGE/fs-codex" "$dst"
    else
        atomic_generation_noreplace "$dst" "$CODEX_STAGE/fs-codex"
    fi
    CODEX_PUBLISH_ACTIVE=0
}

cleanup_provision() {
    local rc=$?
    trap - EXIT
    trap '' INT TERM HUP
    if [[ "$CODEX_PUBLISH_ACTIVE" == 1 ]]; then
        rollback_codex_publish || CODEX_ROLLBACK_FAILED=1
    fi
    if [[ -n "$CODEX_STAGE" && "$CODEX_ROLLBACK_FAILED" == 0 ]]; then
        case "$CODEX_STAGE" in
            "$build_dir"/.fs-codex.staging.*) rm -rf -- "$CODEX_STAGE" 8>&- 9>&- ;;
        esac
    fi
    release_generation_locks || rc=74
    if [[ "$CODEX_ROLLBACK_FAILED" == 1 ]]; then
        echo "ERROR: Codex RootFS rollback was incomplete; retained $CODEX_STAGE" >&2
        rc=74
    fi
    exit "$rc"
}

trap cleanup_provision EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# Codex-tree publication relies on no-replace/exchange. Verify support before
# acquiring the long-lived kernel locks or staging any guest-side work.
verify_generation_rename_primitives || exit 74

if [[ -n "${ROOTFS_INHERITED_LOCK_PID:-}" || \
      -n "${ROOTFS_INHERITED_LOCK_TOKEN:-}" ]]; then
    if ! verify_inherited_lock "$rootfs_lock" \
        "${ROOTFS_INHERITED_LOCK_PID:-}" "${ROOTFS_INHERITED_LOCK_TOKEN:-}"; then
        echo "ERROR: inherited clean RootFS lock ownership is invalid" >&2
        exit 75
    fi
else
    acquire_generation_lock "$rootfs_lock" rootfs
fi

if [[ -n "${CODEX_INHERITED_LOCK_PID:-}" || \
      -n "${CODEX_INHERITED_LOCK_TOKEN:-}" ]]; then
    if ! verify_inherited_lock "$codex_lock" \
        "${CODEX_INHERITED_LOCK_PID:-}" "${CODEX_INHERITED_LOCK_TOKEN:-}"; then
        echo "ERROR: inherited Codex RootFS lock ownership is invalid" >&2
        exit 75
    fi
else
    acquire_generation_lock "$codex_lock" codex
fi

if ! ROOTFS_INPUTS_FILE="${ROOTFS_INPUTS_FILE:-$repo/scripts/alpine-rootfs-pin.sh}" \
     ISH_SRC="${ISH_SRC:-$repo/third_party/ish}" \
     "$rootfs_builder" --verify-bundle "$build_dir" 8>&- 9>&-; then
    echo "ERROR: clean RootFS bundle is missing, stale, or incomplete" >&2
    exit 74
fi
if [[ ! -x "$provision_bin" || ! -f "$provision_bin" || -L "$provision_bin" ]]; then
    echo "ERROR: $provision_bin is missing or unsafe; build it with run-host-tests.sh" >&2
    exit 2
fi
refresh_codex_inputs
EXPECTED_CODEX_IDENTITY="$(print_expected_codex_identity)"

if [[ "$mode" == "verify" ]]; then
    validate_codex_rootfs "$dst"
    exit $?
fi

if [[ "${FORCE:-0}" != 1 ]] && validate_codex_rootfs "$dst" 2>/dev/null; then
    echo "[provision] reusing validated $dst"
    exit 0
fi

CODEX_STAGE="$(mktemp -d "$build_dir/.fs-codex.staging.XXXXXX" 8>&- 9>&-)"
staged_rootfs="$CODEX_STAGE/fs-codex"
mkdir "$staged_rootfs" 8>&- 9>&-
echo "[provision] staging $staged_rootfs from validated $src"
if cp -a --reflink=auto "$src/." "$staged_rootfs/" 8>&- 9>&- 2>/dev/null; then
    :
else
    cp -a "$src/." "$staged_rootfs/" 8>&- 9>&-
fi
rm -f -- "$staged_rootfs/meta.db-shm" "$staged_rootfs/meta.db-wal" 8>&- 9>&-

echo "[provision] running guest apk + npm install (this can take several minutes)"
echo "[provision] tail -f $log to watch"
: > "$log"
set +e
ISH_EMBED_ROOTFS="$staged_rootfs" \
    VM_NAME="$CODEX_VM_NAME" \
    NPM_PKG="$CODEX_PKG" \
    NPM_VERSION="$CODEX_INSTALL_VERSION" \
    BIN_NAME="$CODEX_BIN_NAME" \
    CODEX_VM_NAME="$CODEX_VM_NAME" \
    CODEX_PKG="$CODEX_PKG" \
    CODEX_VERSION="$CODEX_INSTALL_VERSION" \
    "$provision_bin" 8>&- 9>&- 2>&1 | tee -a "$log" 8>&- 9>&-
provision_rc=${PIPESTATUS[0]}
set -e
if [[ $provision_rc -ne 0 ]]; then
    echo "[provision] FAILED (rc=$provision_rc). Existing $dst is unchanged; see $log" >&2
    exit "$provision_rc"
fi

actual_version="$(installed_package_version "$staged_rootfs")"
if [[ "$CODEX_REQUEST_KIND" == "exact" &&
      "$actual_version" != "$CODEX_REQUESTED_VERSION" ]]; then
    echo "ERROR: npm installed $actual_version for exact request $CODEX_REQUESTED_VERSION" >&2
    exit 74
fi
refresh_codex_inputs
if [[ "$(print_expected_codex_identity)" != "$EXPECTED_CODEX_IDENTITY" ]]; then
    echo "ERROR: clean RootFS or Codex provision inputs changed during provisioning" >&2
    exit 74
fi
identity_tmp="$(mktemp "$staged_rootfs/${codex_identity_name}.tmp.XXXXXX" 8>&- 9>&-)"
{
    print_expected_codex_identity
    printf 'CODEX_ACTUAL_VERSION=%s\n' "$actual_version"
} > "$identity_tmp"
chmod 0644 "$identity_tmp" 8>&- 9>&-
mv -f -- "$identity_tmp" "$staged_rootfs/$codex_identity_name" 8>&- 9>&-

if ! validate_codex_rootfs "$staged_rootfs"; then
    echo "ERROR: staged Codex RootFS failed final validation" >&2
    exit 74
fi

if [[ -e "$dst" || -L "$dst" ]]; then
    if [[ ! -d "$dst" || -L "$dst" ]]; then
        echo "ERROR: refusing to replace unsafe Codex RootFS target: $dst" >&2
        exit 74
    fi
    CODEX_PUBLISH_MODE="exchange"
else
    CODEX_PUBLISH_MODE="new"
fi

# Defer catchable signals across the rename-to-journal boundary. Once the
# operation is recorded, the normal exit trap can reverse it deterministically.
trap 'CODEX_DEFERRED_EXIT=130' INT
trap 'CODEX_DEFERRED_EXIT=143' TERM
trap 'CODEX_DEFERRED_EXIT=129' HUP
if [[ "$CODEX_PUBLISH_MODE" == "exchange" ]]; then
    atomic_generation_exchange "$staged_rootfs" "$dst"
else
    atomic_generation_noreplace "$staged_rootfs" "$dst"
fi
CODEX_PUBLISH_ACTIVE=1
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
if [[ "$CODEX_DEFERRED_EXIT" != 0 ]]; then
    exit "$CODEX_DEFERRED_EXIT"
fi

if ! validate_codex_rootfs "$dst"; then
    echo "ERROR: published Codex RootFS failed final validation" >&2
    exit 74
fi
CODEX_PUBLISH_ACTIVE=0

echo "[provision] OK — $dst is ready (actual $CODEX_PKG version $actual_version)"
