#!/usr/bin/env bash
# Orchestrator for the host-side test loop.
#
# 1. Ensures $REPO/build-check (iSH static libs) exists.
# 2. Configures $REPO/build-host (embed + tests) if missing.
# 3. Builds all host-test binaries.
# 4. Provisions $REPO/build/fs-codex on demand.
# 5. Runs procfs_test against the clean rootfs and codex_test against
#    the provisioned one.
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
#   scripts/run-host-tests.sh --reprovision    # rebuild fs-codex
#
# Re-run after editing third_party/ish/ — this script reconfigures and
# rebuilds incrementally.

set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

want_codex=1
want_smoke=0
force_provision=0

for arg in "$@"; do
    case "$arg" in
        --no-codex)     want_codex=0 ;;
        --reprovision)  force_provision=1 ;;
        --smoke)        want_smoke=1 ;;
        -h|--help)
            sed -n '2,30p' "$0"
            exit 0 ;;
        *)
            echo "unknown flag: $arg" >&2
            exit 2 ;;
    esac
done

ish_src="$repo/third_party/ish"
# We reuse the developer's existing iSH build at build-check/. The
# embed-chroot-containment branch targets arm64 guest (matches iClaw +
# the public fs.tar.gz), so we follow whatever build-check is set to.
ish_build="$repo/build-check"
embed_build="$repo/build-host"
fs_clean="$repo/build/fs"

# Mirror whatever guest_arch build-check uses so libish.a and ish_ffi.c
# agree on the cpu_state layout. (Mismatch -> SIGSEGV at boot.)
ish_guest_arch="$(
    python3 -c "import json,sys;[print(o['value']) for o in json.load(sys.stdin) if o['name']=='guest_arch']" \
        < "$ish_build/meson-info/intro-buildoptions.json" 2>/dev/null || true
)"
ish_guest_arch="${ish_guest_arch:-arm64}"
echo "[host-tests] iSH guest_arch = $ish_guest_arch"

# ---- 1. iSH static libs -------------------------------------------------

if [[ ! -f "$ish_build/build.ninja" ]]; then
    echo "[host-tests] configuring iSH build at $ish_build"
    meson setup "$ish_build" "$ish_src" -Dguest_arch="$ish_guest_arch"
fi
echo "[host-tests] building iSH (libish.a libish_emu.a libfakefs.a)"
ninja -C "$ish_build" libish.a libish_emu.a libfakefs.a

# ---- 2. ishembed + tests -----------------------------------------------

if [[ ! -f "$embed_build/build.ninja" ]]; then
    echo "[host-tests] configuring embed build at $embed_build (guest_arch=$ish_guest_arch)"
    meson setup "$embed_build" "$repo" \
        -Dish_src="$ish_src" \
        -Dish_build="$ish_build" \
        -Dguest_arch="$ish_guest_arch"
fi
echo "[host-tests] building host-tests"
ninja -C "$embed_build"

# ---- 3. rootfs sanity --------------------------------------------------

if [[ ! -f "$fs_clean/meta.db" ]]; then
    echo "[host-tests] no clean rootfs at $fs_clean — running scripts/build-rootfs.sh"
    "$repo/scripts/build-rootfs.sh"
fi

# ---- 4. procfs / smoke (no codex needed) -------------------------------

echo
echo "================================================================"
echo "= procfs_test (against clean rootfs)"
echo "================================================================"
ISH_EMBED_ROOTFS="$fs_clean" "$embed_build/procfs_test" || procfs_rc=$?
procfs_rc=${procfs_rc:-0}

if [[ $want_smoke -eq 1 ]]; then
    echo
    echo "================================================================"
    echo "= ishembed_smoke"
    echo "================================================================"
    ISH_EMBED_ROOTFS="$fs_clean" "$embed_build/ishembed_smoke" || smoke_rc=$?
    smoke_rc=${smoke_rc:-0}
fi

# ---- 5. codex ----------------------------------------------------------

codex_rc=0
if [[ $want_codex -eq 1 ]]; then
    if [[ $force_provision -eq 1 ]]; then export FORCE=1; fi
    echo
    echo "================================================================"
    echo "= provisioning fs-codex (apk add nodejs npm; npm i -g @openai/codex)"
    echo "================================================================"
    "$repo/scripts/provision-codex-rootfs.sh"

    echo
    echo "================================================================"
    echo "= codex_test"
    echo "================================================================"
    ISH_EMBED_ROOTFS="$repo/build/fs-codex" "$embed_build/codex_test" || codex_rc=$?
fi

# ---- summary -----------------------------------------------------------

echo
echo "================================================================"
echo "= summary"
echo "================================================================"
echo "procfs_test : $procfs_rc"
[[ $want_smoke -eq 1 ]] && echo "smoke       : ${smoke_rc:-?}"
[[ $want_codex -eq 1 ]] && echo "codex_test  : $codex_rc"

# Suite passes if procfs is green. codex is informational (it tracks
# whether the plan's fixes are landing); flip this to hard once
# codex --version is expected to work.
exit "$procfs_rc"
