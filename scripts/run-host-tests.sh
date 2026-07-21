#!/usr/bin/env bash
# Orchestrator for the host-side test loop.
#
# 1. Audits or safely rebuilds $REPO/build/fs from its input identity.
# 2. Ensures $REPO/build-check (iSH static libs) exists.
# 3. Configures $REPO/build-host (embed + tests) if missing.
# 4. Builds all host-test binaries and provisions fs-codex on demand.
# 5. Runs requested procfs/smoke stages against the clean RootFS and
#    codex_test against the provisioned one.
#
# Flags:
#   --no-codex     skip codex provisioning + codex_test (fast loop for
#                  procfs/syscall iteration)
#   --reprovision  FORCE=1 redo apk + npm install
#   --smoke        also run ishembed_smoke
#
# Usage:
#   scripts/run-host-tests.sh                  # full loop
#   scripts/run-host-tests.sh --no-codex       # ~30s loop
#   scripts/run-host-tests.sh --no-codex --smoke
#   scripts/run-host-tests.sh --reprovision    # rebuild fs-codex
#
# A missing development RootFS is built from scripts/alpine-rootfs-pin.sh.
# For a reviewed alternate input, set ALPINE_VERSION and ALPINE_SHA256 together.
#
# Re-run after editing third_party/ish/ — this script reconfigures and
# rebuilds incrementally.

set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

want_codex=1
want_smoke=0
force_provision=0
procfs_rc=0
smoke_rc=0
provision_rc=0
codex_rc=0
codex_ran=0

for arg in "$@"; do
    case "$arg" in
        --no-codex)     want_codex=0 ;;
        --reprovision)  force_provision=1 ;;
        --smoke)        want_smoke=1 ;;
        -h|--help)
            sed -n '2,/^$/p' "$0"
            exit 0 ;;
        *)
            echo "unknown flag: $arg" >&2
            exit 2 ;;
    esac
done

ish_src="$repo/third_party/ish"
# We reuse the developer's existing iSH build only when Meson proves that its
# source and guest architecture match this arm64 package.
ish_build="$repo/build-check"
embed_build="$repo/build-host"
fs_clean="$repo/build/fs"
fs_codex="$repo/build/fs-codex"
rootfs_inputs="${ROOTFS_INPUTS_FILE:-$repo/scripts/alpine-rootfs-pin.sh}"
generation_lock_dir="$repo/build"

# The runner holds stable-inode kernel locks from validation through the last
# consumer. This closes the validate-then-replace race with a concurrent RootFS
# builder or Codex provisioner. Owner metadata is diagnostic and authenticates
# nested scripts; stale/corrupt bytes never determine whether a lock is live.
LOCK_PATHS=()
LOCK_FDS=()
LOCK_TOKENS=()
LOCK_PIDS=()
LOCK_STARTS=()
ACQUIRED_LOCK_TOKEN=""

generation_process_start() {
    local process_pid="$1"
    python3 - "$process_pid" <<'PY'
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
    python3 - <<'PY'
import secrets
print(secrets.token_hex(24))
PY
}

read_generation_lock_owner() {
    local lock_path="$1"
    python3 - "$lock_path" <<'PY'
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

close_generation_lock_fd() {
    local lock_fd="$1"
    case "$lock_fd" in
        8) exec 8>&- ;;
        9) exec 9>&- ;;
        *) return 1 ;;
    esac
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
    case "${#LOCK_FDS[@]}" in
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
    LOCK_PATHS+=("$lock_path")
    LOCK_FDS+=("$lock_fd")
    LOCK_TOKENS+=("$owner_token")
    LOCK_PIDS+=("$owner_pid")
    LOCK_STARTS+=("$owner_start")
    ACQUIRED_LOCK_TOKEN="$owner_token"
}

release_generation_locks() {
    local index lock_path lock_fd expected owner rc=0
    for (( index=${#LOCK_PATHS[@]} - 1; index >= 0; index-- )); do
        lock_path="${LOCK_PATHS[$index]}"
        lock_fd="${LOCK_FDS[$index]}"
        expected="${LOCK_PIDS[$index]}|${LOCK_STARTS[$index]}|${LOCK_TOKENS[$index]}"
        owner="$(read_generation_lock_owner "$lock_path" 2>/dev/null || true)"
        if ! generation_lock_path_matches_fd "$lock_path" "$lock_fd" ||
           [[ "$owner" != "$expected" ]]; then
            echo "ERROR: refusing to release generation lock with changed ownership: $lock_path" >&2
            rc=1
        fi
        close_generation_lock_fd "$lock_fd" || rc=1
    done
    LOCK_PATHS=()
    LOCK_FDS=()
    return "$rc"
}

cleanup_host_runner() {
    local rc=$?
    trap - EXIT INT TERM HUP
    release_generation_locks || rc=74
    exit "$rc"
}

# Cached Meson directories are architecture-bearing build artifacts. Read their
# introspection strictly: guessing a default after malformed or missing JSON can
# link an x86 FFI shim to arm64 iSH state and crash before a test can diagnose it.
meson_option_value() {
    local build_dir="$1"
    local option_name="$2"
    local options_file="$build_dir/meson-info/intro-buildoptions.json"
    [[ -r "$options_file" ]] || return 1
    python3 - "$options_file" "$option_name" 8>&- 9>&- <<'PY'
import json
import sys

try:
    with open(sys.argv[1], "r", encoding="utf-8") as handle:
        entries = json.load(handle)
    matches = [
        entry for entry in entries
        if isinstance(entry, dict) and entry.get("name") == sys.argv[2]
    ]
    if len(matches) != 1 or not isinstance(matches[0].get("value"), str):
        raise ValueError("missing, duplicate, or non-string Meson option")
    value = matches[0]["value"]
    if "\n" in value or "\r" in value:
        raise ValueError("invalid newline in Meson option")
    sys.stdout.write(value)
except (OSError, ValueError, TypeError, AttributeError, json.JSONDecodeError):
    raise SystemExit(1)
PY
}

meson_source_value() {
    local build_dir="$1"
    local info_file="$build_dir/meson-info/meson-info.json"
    [[ -r "$info_file" ]] || return 1
    python3 - "$info_file" 8>&- 9>&- <<'PY'
import json
import sys

try:
    with open(sys.argv[1], "r", encoding="utf-8") as handle:
        info = json.load(handle)
    source = info.get("directories", {}).get("source")
    if not isinstance(source, str) or not source or "\n" in source or "\r" in source:
        raise ValueError("missing or invalid Meson source directory")
    sys.stdout.write(source)
except (OSError, ValueError, TypeError, AttributeError, json.JSONDecodeError):
    raise SystemExit(1)
PY
}

meson_array_option_contains() {
    local build_dir="$1"
    local option_name="$2"
    local required_value="$3"
    local options_file="$build_dir/meson-info/intro-buildoptions.json"
    [[ -r "$options_file" ]] || return 1
    python3 - "$options_file" "$option_name" "$required_value" 8>&- 9>&- <<'PY'
import json
import sys

try:
    with open(sys.argv[1], "r", encoding="utf-8") as handle:
        entries = json.load(handle)
    matches = [
        entry for entry in entries
        if isinstance(entry, dict) and entry.get("name") == sys.argv[2]
    ]
    if len(matches) != 1:
        raise ValueError("missing or duplicate Meson array option")
    value = matches[0].get("value")
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ValueError("Meson array option is not a string list")
    if sys.argv[3] not in value:
        raise SystemExit(1)
except (OSError, ValueError, TypeError, AttributeError, json.JSONDecodeError):
    raise SystemExit(1)
PY
}

validate_ish_build_cache() {
    local cached_arch cached_source
    if ! cached_arch="$(meson_option_value "$ish_build" guest_arch)" ||
       ! cached_source="$(meson_source_value "$ish_build")"; then
        echo "ERROR: cannot verify cached iSH build at $ish_build: Meson introspection is missing or invalid" >&2
        return 1
    fi
    if [[ "$cached_arch" != "$expected_guest_arch" ]]; then
        echo "ERROR: cached iSH build guest_arch=$cached_arch, expected $expected_guest_arch" >&2
        return 1
    fi
    if [[ "$cached_source" != "$ish_src" ]]; then
        echo "ERROR: cached iSH build source=$cached_source, expected $ish_src" >&2
        return 1
    fi
    if ! meson_array_option_contains \
        "$ish_build" c_args -DISH_DISABLE_SKIP_BRK=1; then
        echo "ERROR: cached iSH build c_args is missing -DISH_DISABLE_SKIP_BRK=1" >&2
        echo "Reconfigure this dedicated cache with -Dc_args=-DISH_DISABLE_SKIP_BRK=1 before retrying." >&2
        return 1
    fi
    return 0
}

validate_embed_build_cache() {
    local cached_arch cached_ish_src cached_ish_build cached_source
    if ! cached_arch="$(meson_option_value "$embed_build" guest_arch)" ||
       ! cached_ish_src="$(meson_option_value "$embed_build" ish_src)" ||
       ! cached_ish_build="$(meson_option_value "$embed_build" ish_build)" ||
       ! cached_source="$(meson_source_value "$embed_build")"; then
        echo "ERROR: cannot verify cached host build at $embed_build: Meson introspection is missing or invalid" >&2
        return 1
    fi
    if [[ "$cached_arch" != "$expected_guest_arch" ]]; then
        echo "ERROR: cached host build guest_arch=$cached_arch, expected $expected_guest_arch" >&2
        return 1
    fi
    if [[ "$cached_ish_src" != "$ish_src" ]]; then
        echo "ERROR: cached host build ish_src=$cached_ish_src, expected $ish_src" >&2
        return 1
    fi
    if [[ "$cached_ish_build" != "$ish_build" ]]; then
        echo "ERROR: cached host build ish_build=$cached_ish_build, expected $ish_build" >&2
        return 1
    fi
    if [[ "$cached_source" != "$repo" ]]; then
        echo "ERROR: cached host build source=$cached_source, expected $repo" >&2
        return 1
    fi
    return 0
}

# ---- 1. RootFS identity -------------------------------------------------

expected_rootfs_identity="$(
    ROOTFS_INPUTS_FILE="$rootfs_inputs" \
        "$repo/scripts/build-rootfs.sh" --print-identity
)"
expected_guest_arch="$(
    printf '%s\n' "$expected_rootfs_identity" | \
        awk -F= '$1 == "ISH_GUEST_ARCH" { print $2 }'
)"
if [[ -z "$expected_guest_arch" || "$expected_guest_arch" != "arm64" ]]; then
    echo "ERROR: invalid expected RootFS guest architecture: ${expected_guest_arch:-missing}" >&2
    exit 65
fi
ish_guest_arch="$expected_guest_arch"
if [[ -f "$ish_build/build.ninja" ]] && ! validate_ish_build_cache; then
    echo "Remove or safely reconfigure the exact build-check directory before retrying." >&2
    exit 65
fi
if [[ -f "$embed_build/build.ninja" ]] && ! validate_embed_build_cache; then
    echo "Remove or safely reconfigure the exact build-host directory before retrying." >&2
    exit 65
fi
echo "[host-tests] iSH guest_arch = $ish_guest_arch"

mkdir -p "$generation_lock_dir"
generation_lock_dir="$(cd "$generation_lock_dir" && pwd -P)"
trap cleanup_host_runner EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

acquire_generation_lock "$generation_lock_dir/.rootfs-build.lock" rootfs
rootfs_lock_token="$ACQUIRED_LOCK_TOKEN"
codex_lock_token=""
if [[ $want_codex -eq 1 ]]; then
    acquire_generation_lock "$generation_lock_dir/.fs-codex.lock" codex
    codex_lock_token="$ACQUIRED_LOCK_TOKEN"
fi

rootfs_stale_reason=""
rootfs_matches_expected() {
    local rootfs_path="${1:-$fs_clean}"
    local verify_mode="--verify-rootfs"
    local verify_path="$rootfs_path"
    if [[ "$rootfs_path" == "$fs_clean" ]]; then
        verify_mode="--verify-bundle"
        verify_path="$generation_lock_dir"
    fi
    if ! rootfs_stale_reason="$(
        ROOTFS_INPUTS_FILE="$rootfs_inputs" ISH_SRC="$ish_src" \
            "$repo/scripts/build-rootfs.sh" "$verify_mode" "$verify_path" \
            8>&- 9>&- 2>&1
    )"; then
        rootfs_stale_reason="${rootfs_stale_reason:-RootFS validation failed}"
        return 1
    fi
    rootfs_stale_reason=""
    return 0
}

need_rootfs_build=0
if [[ -L "$fs_clean" ]]; then
    echo "ERROR: refusing to follow RootFS symlink: $fs_clean" >&2
    exit 65
elif [[ ! -e "$fs_clean" ]]; then
    need_rootfs_build=1
elif [[ ! -d "$fs_clean" ]]; then
    echo "ERROR: refusing to replace non-directory RootFS path: $fs_clean" >&2
    exit 65
elif rootfs_matches_expected "$fs_clean"; then
    echo "[host-tests] RootFS recipe and fakefs content validation passed"
else
    echo "[host-tests] stale RootFS at $fs_clean: $rootfs_stale_reason" >&2
    need_rootfs_build=1
fi

if [[ $need_rootfs_build -eq 1 ]]; then
    echo "[host-tests] building clean RootFS from $rootfs_inputs"
    ROOTFS_INPUTS_FILE="$rootfs_inputs" ISH_SRC="$ish_src" ROOTFS_REUSE_VALID=1 \
        ROOTFS_INHERITED_LOCK_PID="$$" \
        ROOTFS_INHERITED_LOCK_TOKEN="$rootfs_lock_token" \
        "$repo/scripts/build-rootfs.sh" 8>&- 9>&-
    rootfs_stale_reason=""
    if ! rootfs_matches_expected "$fs_clean"; then
        echo "ERROR: RootFS builder returned success without matching identity: $rootfs_stale_reason" >&2
        exit 74
    fi
    # Every fs-codex tree derives from the clean RootFS. Do not let its older
    # timestamp-based stamp reuse a pre-transition architecture after a rebuild.
    force_provision=1
fi

if [[ $want_codex -eq 1 ]]; then
    codex_rootfs_matches_expected() {
        local verify_output
        if ! verify_output="$(
            ROOTFS_INHERITED_LOCK_PID="$$" \
            ROOTFS_INHERITED_LOCK_TOKEN="$rootfs_lock_token" \
            CODEX_INHERITED_LOCK_PID="$$" \
            CODEX_INHERITED_LOCK_TOKEN="$codex_lock_token" \
                "$repo/scripts/provision-codex-rootfs.sh" --verify \
                8>&- 9>&- 2>&1
        )"; then
            rootfs_stale_reason="${verify_output:-provisioned RootFS validation failed}"
            return 1
        fi
        rootfs_stale_reason=""
        return 0
    }
    if [[ -L "$fs_codex" ]]; then
        echo "ERROR: refusing to follow provisioned RootFS symlink: $fs_codex" >&2
        exit 65
    elif [[ -e "$fs_codex" && ! -d "$fs_codex" ]]; then
        echo "ERROR: refusing to replace non-directory provisioned RootFS: $fs_codex" >&2
        exit 65
    elif [[ -d "$fs_codex" ]] && ! codex_rootfs_matches_expected; then
        echo "[host-tests] provisioned RootFS identity is stale; forcing reprovision" >&2
        force_provision=1
    fi
fi

# ---- 2. iSH static libs -------------------------------------------------

if [[ ! -f "$ish_build/build.ninja" ]]; then
    echo "[host-tests] configuring iSH build at $ish_build"
    meson setup "$ish_build" "$ish_src" \
        -Dguest_arch="$ish_guest_arch" \
        -Dc_args=-DISH_DISABLE_SKIP_BRK=1 \
        8>&- 9>&-
fi
if ! validate_ish_build_cache; then
    echo "Remove or safely reconfigure the exact build-check directory before retrying." >&2
    exit 65
fi
echo "[host-tests] building iSH (libish.a libish_emu.a libfakefs.a)"
ninja -C "$ish_build" libish.a libish_emu.a libfakefs.a 8>&- 9>&-

# ---- 3. ishembed + tests -----------------------------------------------

if [[ ! -f "$embed_build/build.ninja" ]]; then
    echo "[host-tests] configuring embed build at $embed_build (guest_arch=$ish_guest_arch)"
    meson setup "$embed_build" "$repo" \
        -Dish_src="$ish_src" \
        -Dish_build="$ish_build" \
        -Dguest_arch="$ish_guest_arch" \
        8>&- 9>&-
fi
if ! validate_embed_build_cache; then
    echo "Remove or safely reconfigure the exact build-host directory before retrying." >&2
    exit 65
fi
echo "[host-tests] building host-tests"
ninja -C "$embed_build" 8>&- 9>&-

# ---- 4. procfs / smoke (no codex needed) -------------------------------

echo
echo "================================================================"
echo "= procfs_test (against clean rootfs)"
echo "================================================================"
ISH_EMBED_ROOTFS="$fs_clean" "$embed_build/procfs_test" \
    8>&- 9>&- || procfs_rc=$?

if [[ $want_smoke -eq 1 ]]; then
    echo
    echo "================================================================"
    echo "= ishembed_smoke"
    echo "================================================================"
    ISH_EMBED_ROOTFS="$fs_clean" "$embed_build/ishembed_smoke" \
        8>&- 9>&- || smoke_rc=$?
fi

# ---- 5. codex ----------------------------------------------------------

if [[ $want_codex -eq 1 ]]; then
    if [[ $force_provision -eq 1 ]]; then export FORCE=1; fi
    echo
    echo "================================================================"
    echo "= provisioning fs-codex (apk add nodejs npm; npm i -g @openai/codex)"
    echo "================================================================"
    ROOTFS_INHERITED_LOCK_PID="$$" \
    ROOTFS_INHERITED_LOCK_TOKEN="$rootfs_lock_token" \
    CODEX_INHERITED_LOCK_PID="$$" \
    CODEX_INHERITED_LOCK_TOKEN="$codex_lock_token" \
        "$repo/scripts/provision-codex-rootfs.sh" \
        8>&- 9>&- || provision_rc=$?

    if [[ $provision_rc -eq 0 ]]; then
        if ! codex_rootfs_matches_expected; then
            echo "ERROR: provisioning returned success without a matching arm64 RootFS identity" >&2
            provision_rc=74
        else
            codex_ran=1
            echo
            echo "================================================================"
            echo "= codex_test"
            echo "================================================================"
            ISH_EMBED_ROOTFS="$fs_codex" "$embed_build/codex_test" \
                8>&- 9>&- || codex_rc=$?
        fi
    fi
fi

# ---- summary -----------------------------------------------------------

echo
echo "================================================================"
echo "= summary"
echo "================================================================"
echo "procfs_test : $procfs_rc"
[[ $want_smoke -eq 1 ]] && echo "smoke       : $smoke_rc"
if [[ $want_codex -eq 1 ]]; then
    echo "provision   : $provision_rc"
    if [[ $codex_ran -eq 1 ]]; then
        echo "codex_test  : $codex_rc"
    else
        echo "codex_test  : not run (provision failed)"
    fi
fi

# Preserve the first non-zero status while still running every requested stage
# whose prerequisites succeeded. A zero exit therefore proves that every stage
# named by this invocation passed, rather than only procfs_test.
suite_rc=$procfs_rc
if [[ $want_smoke -eq 1 && $suite_rc -eq 0 && $smoke_rc -ne 0 ]]; then
    suite_rc=$smoke_rc
fi
if [[ $want_codex -eq 1 ]]; then
    if [[ $suite_rc -eq 0 && $provision_rc -ne 0 ]]; then
        suite_rc=$provision_rc
    fi
    if [[ $codex_ran -eq 1 && $suite_rc -eq 0 && $codex_rc -ne 0 ]]; then
        suite_rc=$codex_rc
    fi
fi
exit "$suite_rc"
