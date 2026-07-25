#!/usr/bin/env bash

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="$PKG_ROOT/scripts/run-host-tests.sh"
ROOTFS_BUILDER="$PKG_ROOT/scripts/build-rootfs.sh"
ROOTFS_PIN="$PKG_ROOT/scripts/alpine-rootfs-pin.sh"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-host-runner.XXXXXX")"
SHADOW_REPO="$TEST_ROOT/repo"
FAKE_BIN="$TEST_ROOT/bin"

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    if [[ -n "${RUNNER_LEAK_PID:-}" ]]; then
        kill "$RUNNER_LEAK_PID" 2>/dev/null || true
    fi
    if [[ "${KEEP_TEST_ROOT:-0}" == 1 ]]; then
        printf 'retained test root: %s\n' "$TEST_ROOT" >&2
    else
        rm -rf "$TEST_ROOT"
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM HUP

unset \
    ALPINE_VERSION \
    ALPINE_ARCH \
    ALPINE_SHA256 \
    SOURCE_DATE_EPOCH \
    ROOTFS_ARCHIVER \
    ROOTFS_INPUTS_FILE

"$ROOTFS_BUILDER" --print-inputs > "$TEST_ROOT/rootfs-inputs.log"
grep -q '^ROOTFS_IDENTITY_SCHEMA=3$' "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'default RootFS identity schema is missing\n' >&2
    sed -n '1,80p' "$TEST_ROOT/rootfs-inputs.log" >&2
    exit 1
}
grep -q '^ROOTFS_RECIPE=alpine-fakefs-ishsv-v3$' \
    "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'default RootFS recipe identity is missing\n' >&2
    sed -n '1,80p' "$TEST_ROOT/rootfs-inputs.log" >&2
    exit 1
}
grep -Eq '^ROOTFS_RECIPE_SHA256=[0-9a-f]{64}$' \
    "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'RootFS recipe digest is missing or malformed\n' >&2
    sed -n '1,80p' "$TEST_ROOT/rootfs-inputs.log" >&2
    exit 1
}
for recipe_field in \
    ROOTFS_BUILDER_SHA256 \
    SUPERVISOR_SOURCE_SHA256 \
    SUPERVISOR_MAKEFILE_SHA256 \
    PROTOCOL_HEADER_SHA256 \
    ISHEMBED_HEADER_SHA256 \
    ROOTFS_ARCHIVER_SHA256 \
    ISH_WORKTREE_SHA256 \
    ISH_SUBMODULES_SHA256 \
    FAKEFSIFY_INPUT_SHA256; do
    grep -Eq "^${recipe_field}=[0-9a-f]{64}$" \
        "$TEST_ROOT/rootfs-inputs.log" || {
        printf 'RootFS recipe field %s is missing or malformed\n' \
            "$recipe_field" >&2
        sed -n '1,100p' "$TEST_ROOT/rootfs-inputs.log" >&2
        exit 1
    }
done
grep -q '^ROOTFS_SOURCE_DATE_EPOCH=1704067200$' \
    "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'RootFS deterministic archive epoch is missing\n' >&2
    sed -n '1,100p' "$TEST_ROOT/rootfs-inputs.log" >&2
    exit 1
}
grep -Eq '^ISH_REVISION=([0-9a-f]{40,64}|unversioned)$' \
    "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'iSH source revision is missing or malformed\n' >&2
    exit 1
}
grep -q '^FAKEFSIFY_ORIGIN=bundled-ish-source$' \
    "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'default fakefsify provenance is not bound to the iSH checkout\n' >&2
    exit 1
}
grep -q '^ALPINE_ARCH=aarch64$' "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'default RootFS Alpine architecture is missing\n' >&2
    sed -n '1,80p' "$TEST_ROOT/rootfs-inputs.log" >&2
    exit 1
}
grep -q '^ISH_GUEST_ARCH=arm64$' "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'default RootFS guest architecture is missing\n' >&2
    sed -n '1,80p' "$TEST_ROOT/rootfs-inputs.log" >&2
    exit 1
}
grep -q '^ALPINE_URL=https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/aarch64/alpine-minirootfs-3.19.1-aarch64.tar.gz$' \
    "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'default RootFS source URL did not match the reviewed pin\n' >&2
    sed -n '1,80p' "$TEST_ROOT/rootfs-inputs.log" >&2
    exit 1
}
grep -q '^ALPINE_SHA256=7ef5eef3a5b1d198dfb1610cde1ef5b0755ff5d838fb1e5e1b9f42b59214820f$' \
    "$TEST_ROOT/rootfs-inputs.log" || {
    printf 'default RootFS digest did not match the reviewed pin\n' >&2
    sed -n '1,80p' "$TEST_ROOT/rootfs-inputs.log" >&2
    exit 1
}

set +e
ALPINE_VERSION=3.20.0 ALPINE_SHA256=not-a-digest \
    "$ROOTFS_BUILDER" --print-inputs > "$TEST_ROOT/invalid-inputs.log" 2>&1
invalid_inputs_rc=$?
set -e
[[ "$invalid_inputs_rc" == 64 ]] || {
    printf 'expected invalid RootFS digest status 64, got %s\n' \
        "$invalid_inputs_rc" >&2
    sed -n '1,80p' "$TEST_ROOT/invalid-inputs.log" >&2
    exit 1
}

set +e
ALPINE_VERSION=3.20.0 \
    "$ROOTFS_BUILDER" --print-inputs > "$TEST_ROOT/unpaired-inputs.log" 2>&1
unpaired_inputs_rc=$?
set -e
[[ "$unpaired_inputs_rc" == 64 ]] || {
    printf 'expected unpaired RootFS override status 64, got %s\n' \
        "$unpaired_inputs_rc" >&2
    sed -n '1,80p' "$TEST_ROOT/unpaired-inputs.log" >&2
    exit 1
}
grep -q 'override ALPINE_VERSION and reviewed ALPINE_SHA256 together' \
    "$TEST_ROOT/unpaired-inputs.log" || {
    printf 'unpaired RootFS override did not explain the required digest\n' >&2
    sed -n '1,80p' "$TEST_ROOT/unpaired-inputs.log" >&2
    exit 1
}

set +e
ROOTFS_INPUTS_FILE="$TEST_ROOT/missing-rootfs-pin.sh" \
    "$ROOTFS_BUILDER" --print-inputs > "$TEST_ROOT/missing-pin.log" 2>&1
missing_pin_rc=$?
set -e
[[ "$missing_pin_rc" == 64 ]] || {
    printf 'expected missing RootFS pin status 64, got %s\n' \
        "$missing_pin_rc" >&2
    sed -n '1,80p' "$TEST_ROOT/missing-pin.log" >&2
    exit 1
}
grep -q 'reviewed RootFS input pin is not readable' \
    "$TEST_ROOT/missing-pin.log" || {
    printf 'missing RootFS pin did not produce a provenance-gate error\n' >&2
    sed -n '1,80p' "$TEST_ROOT/missing-pin.log" >&2
    exit 1
}

mkdir -p "$TEST_ROOT/existing-build/fs"
: > "$TEST_ROOT/existing-build/fs/sentinel"
set +e
BUILD_DIR="$TEST_ROOT/existing-build" ROOTFS_REQUIRE_ABSENT=1 \
    "$ROOTFS_BUILDER" > "$TEST_ROOT/refuse-replace.log" 2>&1
refuse_replace_rc=$?
set -e
[[ "$refuse_replace_rc" == 73 ]] || {
    printf 'expected existing RootFS refusal status 73, got %s\n' \
        "$refuse_replace_rc" >&2
    sed -n '1,80p' "$TEST_ROOT/refuse-replace.log" >&2
    exit 1
}
[[ -f "$TEST_ROOT/existing-build/fs/sentinel" ]] || {
    printf 'ROOTFS_REQUIRE_ABSENT overwrote the existing RootFS\n' >&2
    exit 1
}

# Every source that can change the generated RootFS must alter the recipe
# identity before any cached tree is considered reusable.
RECIPE_REPO="$TEST_ROOT/recipe-repo"
mkdir -p \
    "$RECIPE_REPO/scripts" \
    "$RECIPE_REPO/supervisor" \
    "$RECIPE_REPO/protocol" \
    "$RECIPE_REPO/include"
cp "$ROOTFS_BUILDER" "$RECIPE_REPO/scripts/build-rootfs.sh"
cp "$ROOTFS_PIN" "$RECIPE_REPO/scripts/alpine-rootfs-pin.sh"
cp "$PKG_ROOT/scripts/create-deterministic-tar.py" \
    "$RECIPE_REPO/scripts/create-deterministic-tar.py"
cp "$PKG_ROOT/supervisor/ishsv.c" "$RECIPE_REPO/supervisor/ishsv.c"
cp "$PKG_ROOT/supervisor/Makefile" "$RECIPE_REPO/supervisor/Makefile"
cp "$PKG_ROOT/protocol/proto.h" "$RECIPE_REPO/protocol/proto.h"
cp "$PKG_ROOT/include/ishembed.h" "$RECIPE_REPO/include/ishembed.h"
chmod +x \
    "$RECIPE_REPO/scripts/build-rootfs.sh" \
    "$RECIPE_REPO/scripts/create-deterministic-tar.py"

recipe_digest() {
    ISH_SRC="${1:-$PKG_ROOT/third_party/ish}" \
        ROOTFS_INPUTS_FILE="$RECIPE_REPO/scripts/alpine-rootfs-pin.sh" \
        "$RECIPE_REPO/scripts/build-rootfs.sh" --print-identity | \
        awk -F= '$1 == "ROOTFS_RECIPE_SHA256" { print $2 }'
}

baseline_recipe_digest="$(recipe_digest)"
for recipe_relative in \
    scripts/create-deterministic-tar.py \
    supervisor/ishsv.c \
    supervisor/Makefile \
    protocol/proto.h \
    include/ishembed.h; do
    cp "$PKG_ROOT/$recipe_relative" "$RECIPE_REPO/$recipe_relative"
    printf '\n# identity mutation probe\n' >> "$RECIPE_REPO/$recipe_relative"
    changed_recipe_digest="$(recipe_digest)"
    [[ "$changed_recipe_digest" != "$baseline_recipe_digest" ]] || {
        printf 'RootFS recipe did not change after editing %s\n' \
            "$recipe_relative" >&2
        exit 1
    }
    cp "$PKG_ROOT/$recipe_relative" "$RECIPE_REPO/$recipe_relative"
done

ISH_FIXTURE="$TEST_ROOT/ish-source"
mkdir -p "$ISH_FIXTURE"
printf 'fakefsify input v1\n' > "$ISH_FIXTURE/source.txt"
baseline_ish_digest="$(recipe_digest "$ISH_FIXTURE")"
printf 'fakefsify input v2\n' >> "$ISH_FIXTURE/source.txt"
changed_ish_digest="$(recipe_digest "$ISH_FIXTURE")"
[[ "$changed_ish_digest" != "$baseline_ish_digest" ]] || {
    printf 'RootFS recipe did not change after editing the iSH/fakefsify source\n' >&2
    exit 1
}

# A byte-perfect recipe marker copied onto an empty fakefs must not be enough
# to pass reuse admission. The verifier requires a valid SQLite/path layout and
# content-bound artifact fields in addition to the marker prefix.
FORGED_ROOTFS="$TEST_ROOT/forged-rootfs"
mkdir -p "$FORGED_ROOTFS/data"
: > "$FORGED_ROOTFS/meta.db"
"$ROOTFS_BUILDER" --print-identity \
    > "$FORGED_ROOTFS/.ishembed-rootfs-identity"
for artifact_field in \
    FAKEFSIFY_BINARY_SHA256 \
    SUPERVISOR_BINARY_SHA256 \
    BUSYBOX_BINARY_SHA256 \
    ROOTFS_INITIAL_META_SHA256 \
    ROOTFS_INITIAL_DATA_SHA256 \
    ROOTFS_INITIAL_CONTENT_SHA256; do
    printf '%s=%064d\n' "$artifact_field" 0 \
        >> "$FORGED_ROOTFS/.ishembed-rootfs-identity"
done
set +e
"$ROOTFS_BUILDER" --verify-rootfs "$FORGED_ROOTFS" \
    > "$TEST_ROOT/forged-marker.log" 2>&1
forged_marker_rc=$?
set -e
[[ "$forged_marker_rc" -ne 0 ]] || {
    printf 'forged RootFS marker was accepted without valid meta.db/data content\n' >&2
    exit 1
}
grep -Eq 'meta\.db|fakefs' "$TEST_ROOT/forged-marker.log" || {
    printf 'forged RootFS rejection did not report a content validation error\n' >&2
    sed -n '1,120p' "$TEST_ROOT/forged-marker.log" >&2
    exit 1
}

write_exit_program() {
    local path="$1"
    local status="$2"
    printf '%s\n' '#!/usr/bin/env bash' "exit $status" > "$path"
    chmod +x "$path"
}

write_ish_build_metadata() {
    local arch="$1"
    local source_dir="$2"
    local c_args_mode="${3:-required}"
    python3 - \
        "$SHADOW_REPO/build-check/meson-info/intro-buildoptions.json" \
        "$SHADOW_REPO/build-check/meson-info/meson-info.json" \
        "$arch" "$source_dir" "$SHADOW_REPO/build-check" "$c_args_mode" <<'PY'
import json
import os
import sys

options_path, info_path, arch, source_dir, build_dir, c_args_mode = sys.argv[1:]
os.makedirs(os.path.dirname(options_path), exist_ok=True)
with open(options_path, "w", encoding="utf-8") as handle:
    c_args = ["-DISH_DISABLE_SKIP_BRK=1"] if c_args_mode == "required" else []
    json.dump([
        {"name": "guest_arch", "value": arch},
        {"name": "c_args", "value": c_args},
    ], handle)
with open(info_path, "w", encoding="utf-8") as handle:
    json.dump({"directories": {
        "source": source_dir,
        "build": build_dir,
        "info": os.path.dirname(info_path),
    }}, handle)
PY
}

write_embed_build_metadata() {
    local arch="$1"
    local ish_source="$2"
    local ish_build_dir="$3"
    local project_source="$4"
    python3 - \
        "$SHADOW_REPO/build-host/meson-info/intro-buildoptions.json" \
        "$SHADOW_REPO/build-host/meson-info/meson-info.json" \
        "$arch" "$ish_source" "$ish_build_dir" "$project_source" \
        "$SHADOW_REPO/build-host" <<'PY'
import json
import os
import sys

(options_path, info_path, arch, ish_source, ish_build_dir,
 project_source, build_dir) = sys.argv[1:]
os.makedirs(os.path.dirname(options_path), exist_ok=True)
with open(options_path, "w", encoding="utf-8") as handle:
    json.dump([
        {"name": "guest_arch", "value": arch},
        {"name": "ish_src", "value": ish_source},
        {"name": "ish_build", "value": ish_build_dir},
    ], handle)
with open(info_path, "w", encoding="utf-8") as handle:
    json.dump({"directories": {
        "source": project_source,
        "build": build_dir,
        "info": os.path.dirname(info_path),
    }}, handle)
PY
}

expect_cache_rejection() {
    local case_name="$1"
    local expected_message="$2"
    local log="$TEST_ROOT/cache-${case_name}.log"
    local rc
    set +e
    PATH="$FAKE_BIN:$PATH" \
        "$SHADOW_REPO/scripts/run-host-tests.sh" \
        > "$log" 2>&1
    rc=$?
    set -e
    [[ "$rc" == 65 ]] || {
        printf 'expected Meson cache rejection status 65 for %s, got %s\n' \
            "$case_name" "$rc" >&2
        sed -n '1,180p' "$log" >&2
        exit 1
    }
    grep -Fq "$expected_message" "$log" || {
        printf 'Meson cache rejection for %s did not explain the mismatch\n' \
            "$case_name" >&2
        sed -n '1,180p' "$log" >&2
        exit 1
    }
}

write_fake_rootfs_builder() {
    local builder_path="$1"
    cat > "$builder_path" <<'BUILD_ROOTFS'
#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
expected_pin="$repo/scripts/alpine-rootfs-pin.sh"
provided_pin="${ROOTFS_INPUTS_FILE:-}"
if [[ -n "$provided_pin" ]]; then
    provided_pin="$(cd "$(dirname "$provided_pin")" && pwd)/$(basename "$provided_pin")"
fi
[[ "$provided_pin" == "$expected_pin" ]] || exit 71
grep -qx 'PINNED_ALPINE_VERSION="3.19.1"' "$expected_pin" || exit 72
grep -qx 'PINNED_ALPINE_ARCH="aarch64"' "$expected_pin" || exit 73
grep -qx 'PINNED_ALPINE_SHA256="7ef5eef3a5b1d198dfb1610cde1ef5b0755ff5d838fb1e5e1b9f42b59214820f"' \
    "$expected_pin" || exit 74

print_identity() {
    pin_sha="$(shasum -a 256 "$expected_pin" | awk '{print $1}')"
    recipe_sha="$(shasum -a 256 "$0" | awk '{print $1}')"
    printf '%s\n' \
        'ROOTFS_IDENTITY_SCHEMA=2' \
        'ROOTFS_RECIPE=test-fakefs-ishsv-v2'
    printf 'ROOTFS_RECIPE_SHA256=%s\n' "$recipe_sha"
    printf '%s\n' \
        'ALPINE_VERSION=3.19.1' \
        'ALPINE_ARCH=aarch64' \
        'ISH_GUEST_ARCH=arm64' \
        'ALPINE_URL=https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/aarch64/alpine-minirootfs-3.19.1-aarch64.tar.gz' \
        'ALPINE_SHA256=7ef5eef3a5b1d198dfb1610cde1ef5b0755ff5d838fb1e5e1b9f42b59214820f'
    printf 'ROOTFS_INPUTS_SHA256=%s\n' "$pin_sha"
}

verify_rootfs() {
    local rootfs_path="$1"
    local identity="$rootfs_path/.ishembed-rootfs-identity"
    [[ -d "$rootfs_path" && ! -L "$rootfs_path" ]] || {
        printf 'ERROR: missing or unsafe RootFS directory\n' >&2
        return 1
    }
    [[ -f "$rootfs_path/meta.db" && ! -L "$rootfs_path/meta.db" ]] || {
        printf 'ERROR: missing or unsafe fakefs meta.db\n' >&2
        return 1
    }
    [[ -d "$rootfs_path/data" && ! -L "$rootfs_path/data" ]] || {
        printf 'ERROR: missing or unsafe fakefs data directory\n' >&2
        return 1
    }
    [[ -f "$identity" && ! -L "$identity" ]] || {
        printf 'ERROR: missing or unsafe RootFS identity marker\n' >&2
        return 1
    }
    cmp -s "$identity" <(print_identity) || {
        printf 'ERROR: RootFS recipe identity does not match reviewed inputs\n' >&2
        return 1
    }
}

verify_bundle() {
    verify_rootfs "$repo/build/fs" || return 1
    local tarball="$repo/build/fs.tar.gz"
    local sums="$repo/build/SHA256SUMS"
    local receipt="$repo/build/ROOTFS_RECEIPT"
    [[ -f "$tarball" && ! -L "$tarball" &&
       -f "$sums" && ! -L "$sums" &&
       -f "$receipt" && ! -L "$receipt" ]] || return 1
    local tar_sha sums_sha identity_sha recipe_sha
    tar_sha="$(shasum -a 256 "$tarball" | awk '{print $1}')"
    sums_sha="$(shasum -a 256 "$sums" | awk '{print $1}')"
    identity_sha="$(shasum -a 256 "$repo/build/fs/.ishembed-rootfs-identity" | awk '{print $1}')"
    recipe_sha="$(print_identity | awk -F= '$1 == "ROOTFS_RECIPE_SHA256" { print $2 }')"
    [[ "$(< "$sums")" == "$tar_sha  fs.tar.gz" ]] || return 1
    cmp -s "$receipt" <(printf '%s\n' \
        'ROOTFS_RECEIPT_SCHEMA=1' \
        "ROOTFS_RECIPE_SHA256=$recipe_sha" \
        "ROOTFS_IDENTITY_SHA256=$identity_sha" \
        "ROOTFS_TARBALL_SHA256=$tar_sha" \
        "ROOTFS_SUMS_SHA256=$sums_sha")
}

case "${1:-}" in
    --print-identity)
        [[ $# -eq 1 ]] || exit 75
        print_identity
        exit 0
        ;;
    --verify-rootfs)
        [[ $# -eq 2 ]] || exit 75
        verify_rootfs "$2"
        exit $?
        ;;
    --verify-bundle)
        [[ $# -eq 2 ]] || exit 75
        [[ "$(cd "$2" && pwd -P)" == "$(cd "$repo/build" && pwd -P)" ]] || exit 75
        verify_bundle
        exit $?
        ;;
    "") ;;
    *) exit 75 ;;
esac

rootfs="$repo/build/fs"
if [[ "${ROOTFS_REUSE_VALID:-0}" == "1" ]] && verify_bundle 2>/dev/null; then
    printf 'reusing validated fake RootFS\n'
    exit 0
fi
stage="$(mktemp -d "$repo/build/.fake-rootfs-stage.XXXXXX")"
mkdir -p "$stage/fs/data"
: > "$stage/fs/meta.db"
print_identity > "$stage/fs/.ishembed-rootfs-identity"
if [[ -e "$rootfs" || -L "$rootfs" ]]; then
    rm -rf -- "$rootfs"
fi
mv -n "$stage/fs" "$rootfs"
rmdir "$stage"
tar -C "$repo/build" -czf "$repo/build/fs.tar.gz" fs
tar_sha="$(shasum -a 256 "$repo/build/fs.tar.gz" | awk '{print $1}')"
printf '%s  fs.tar.gz\n' "$tar_sha" > "$repo/build/SHA256SUMS"
sums_sha="$(shasum -a 256 "$repo/build/SHA256SUMS" | awk '{print $1}')"
identity_sha="$(shasum -a 256 "$rootfs/.ishembed-rootfs-identity" | awk '{print $1}')"
recipe_sha="$(print_identity | awk -F= '$1 == "ROOTFS_RECIPE_SHA256" { print $2 }')"
printf '%s\n' \
    'ROOTFS_RECEIPT_SCHEMA=1' \
    "ROOTFS_RECIPE_SHA256=$recipe_sha" \
    "ROOTFS_IDENTITY_SHA256=$identity_sha" \
    "ROOTFS_TARBALL_SHA256=$tar_sha" \
    "ROOTFS_SUMS_SHA256=$sums_sha" \
    > "$repo/build/ROOTFS_RECEIPT"
: > "$repo/build/rootfs-builder-called"
BUILD_ROOTFS
    chmod +x "$builder_path"
}

mkdir -p \
    "$SHADOW_REPO/scripts" \
    "$SHADOW_REPO/third_party/ish" \
    "$SHADOW_REPO/build-check/meson-info" \
    "$SHADOW_REPO/build-host/meson-info" \
    "$SHADOW_REPO/build" \
    "$FAKE_BIN"
shadow_repo_identity="$(cd "$SHADOW_REPO" && pwd)"
cp "$RUNNER" "$SHADOW_REPO/scripts/run-host-tests.sh"
cp "$ROOTFS_BUILDER" "$SHADOW_REPO/scripts/build-rootfs.sh"
cp "$ROOTFS_PIN" "$SHADOW_REPO/scripts/alpine-rootfs-pin.sh"
write_fake_rootfs_builder "$SHADOW_REPO/scripts/build-rootfs.sh"
chmod +x \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    "$SHADOW_REPO/scripts/build-rootfs.sh"
: > "$SHADOW_REPO/build-check/build.ninja"
: > "$SHADOW_REPO/build-host/build.ninja"
ROOTFS_INPUTS_FILE="$SHADOW_REPO/scripts/alpine-rootfs-pin.sh" \
    "$SHADOW_REPO/scripts/build-rootfs.sh"
rm -f "$SHADOW_REPO/build/rootfs-builder-called"
write_ish_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish"
write_embed_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish" \
    "$shadow_repo_identity/build-check" "$shadow_repo_identity"
write_exit_program "$FAKE_BIN/ninja" 0
write_exit_program "$SHADOW_REPO/build-host/dirty_page_test" 0
write_exit_program "$SHADOW_REPO/build-host/procfs_test" 0
write_exit_program "$SHADOW_REPO/build-host/ishembed_smoke" 23

set +e
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" --smoke \
    > "$TEST_ROOT/runner.log" 2>&1
runner_rc=$?
set -e

[[ "$runner_rc" == 23 ]] || {
    printf 'expected smoke failure status 23, got %s\n' "$runner_rc" >&2
    sed -n '1,160p' "$TEST_ROOT/runner.log" >&2
    exit 1
}
grep -q '^procfs_test : 0$' "$TEST_ROOT/runner.log" || {
    printf 'runner summary did not preserve successful procfs status\n' >&2
    sed -n '1,160p' "$TEST_ROOT/runner.log" >&2
    exit 1
}
grep -q '^smoke       : 23$' "$TEST_ROOT/runner.log" || {
    printf 'runner summary did not preserve failing smoke status\n' >&2
    sed -n '1,160p' "$TEST_ROOT/runner.log" >&2
    exit 1
}

# The deterministic dirty-page executable is a real runner gate, not merely a
# Meson target that CI might forget to execute.
write_exit_program "$SHADOW_REPO/build-host/dirty_page_test" 31
set +e
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    > "$TEST_ROOT/dirty-page-gate.log" 2>&1
dirty_page_gate_rc=$?
set -e
[[ "$dirty_page_gate_rc" == 31 ]] || {
    printf 'expected dirty-page gate status 31, got %s\n' \
        "$dirty_page_gate_rc" >&2
    sed -n '1,180p' "$TEST_ROOT/dirty-page-gate.log" >&2
    exit 1
}
grep -q '^dirty pages : 31$' "$TEST_ROOT/dirty-page-gate.log" || {
    printf 'runner summary did not preserve failing dirty-page status\n' >&2
    sed -n '1,180p' "$TEST_ROOT/dirty-page-gate.log" >&2
    exit 1
}
write_exit_program "$SHADOW_REPO/build-host/dirty_page_test" 0

# Cached Meson state is executable architecture metadata, not an optional
# optimization. Every unreadable or contradictory cache must stop before a
# host binary can be built or launched.
rm -f "$SHADOW_REPO/build-check/meson-info/intro-buildoptions.json"
expect_cache_rejection \
    ish-missing-introspection \
    'cannot verify cached iSH build'
write_ish_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish"

printf '{broken json\n' \
    > "$SHADOW_REPO/build-check/meson-info/intro-buildoptions.json"
expect_cache_rejection \
    ish-corrupt-introspection \
    'cannot verify cached iSH build'
write_ish_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish"

write_ish_build_metadata \
    x86 "$shadow_repo_identity/third_party/ish"
expect_cache_rejection \
    ish-wrong-arch \
    'cached iSH build guest_arch=x86, expected arm64'
write_ish_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish"

write_ish_build_metadata \
    arm64 "$shadow_repo_identity/not-the-ish-source"
expect_cache_rejection \
    ish-wrong-source \
    'cached iSH build source='
write_ish_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish"

write_ish_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish" missing
expect_cache_rejection \
    ish-missing-sigtrap-policy \
    'cached iSH build c_args is missing -DISH_DISABLE_SKIP_BRK=1'
write_ish_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish"

rm -f "$SHADOW_REPO/build-host/meson-info/intro-buildoptions.json"
expect_cache_rejection \
    host-missing-introspection \
    'cannot verify cached host build'
write_embed_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish" \
    "$shadow_repo_identity/build-check" "$shadow_repo_identity"

printf '[] trailing garbage\n' \
    > "$SHADOW_REPO/build-host/meson-info/intro-buildoptions.json"
expect_cache_rejection \
    host-corrupt-introspection \
    'cannot verify cached host build'
write_embed_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish" \
    "$shadow_repo_identity/build-check" "$shadow_repo_identity"

write_embed_build_metadata \
    x86 "$shadow_repo_identity/third_party/ish" \
    "$shadow_repo_identity/build-check" "$shadow_repo_identity"
expect_cache_rejection \
    host-wrong-arch \
    'cached host build guest_arch=x86, expected arm64'

write_embed_build_metadata \
    arm64 "$shadow_repo_identity/not-the-ish-source" \
    "$shadow_repo_identity/build-check" "$shadow_repo_identity"
expect_cache_rejection \
    host-wrong-ish-src \
    'cached host build ish_src='

write_embed_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish" \
    "$shadow_repo_identity/not-build-check" "$shadow_repo_identity"
expect_cache_rejection \
    host-wrong-ish-build \
    'cached host build ish_build='

write_embed_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish" \
    "$shadow_repo_identity/build-check" "$shadow_repo_identity/not-this-project"
expect_cache_rejection \
    host-wrong-source \
    'cached host build source='
write_embed_build_metadata \
    arm64 "$shadow_repo_identity/third_party/ish" \
    "$shadow_repo_identity/build-check" "$shadow_repo_identity"

rm -rf "$SHADOW_REPO/build/fs"
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    > "$TEST_ROOT/clean-checkout.log" 2>&1
[[ -f "$SHADOW_REPO/build/rootfs-builder-called" ]] || {
    printf 'runner did not invoke the pinned RootFS builder for a clean checkout\n' >&2
    sed -n '1,180p' "$TEST_ROOT/clean-checkout.log" >&2
    exit 1
}
grep -Fq 'building clean RootFS from ' "$TEST_ROOT/clean-checkout.log" && \
    grep -Fq 'scripts/alpine-rootfs-pin.sh' \
    "$TEST_ROOT/clean-checkout.log" || {
    printf 'runner did not report the reviewed RootFS pin\n' >&2
    sed -n '1,180p' "$TEST_ROOT/clean-checkout.log" >&2
    exit 1
}

rm -f "$SHADOW_REPO/build/rootfs-builder-called"
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    > "$TEST_ROOT/matching-rootfs.log" 2>&1
[[ ! -e "$SHADOW_REPO/build/rootfs-builder-called" ]] || {
    printf 'runner rebuilt a matching RootFS instead of reusing it\n' >&2
    sed -n '1,180p' "$TEST_ROOT/matching-rootfs.log" >&2
    exit 1
}
grep -q 'RootFS recipe and fakefs content validation passed' \
    "$TEST_ROOT/matching-rootfs.log" || {
    printf 'runner did not report the matching RootFS identity\n' >&2
    sed -n '1,180p' "$TEST_ROOT/matching-rootfs.log" >&2
    exit 1
}

# The runner must retain the clean-RootFS kernel lock through the final test
# consumer, not merely through validation. Pause procfs_test and prove a
# separate open file description cannot acquire the same lock.
cat > "$SHADOW_REPO/build-host/procfs_test" <<'PAUSED_PROCFS'
#!/usr/bin/env bash
set -euo pipefail
: > "${RUNNER_USE_READY:?}"
while [[ ! -e "${RUNNER_USE_CONTINUE:?}" ]]; do sleep 0.05; done
PAUSED_PROCFS
chmod +x "$SHADOW_REPO/build-host/procfs_test"
RUNNER_USE_READY="$TEST_ROOT/runner-use.ready" \
RUNNER_USE_CONTINUE="$TEST_ROOT/runner-use.continue" \
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    > "$TEST_ROOT/runner-use-lock.log" 2>&1 &
runner_use_pid=$!
for _ in $(seq 1 200); do
    [[ -e "$TEST_ROOT/runner-use.ready" ]] && break
    sleep 0.05
done
[[ -e "$TEST_ROOT/runner-use.ready" ]] || {
    printf 'paused runner never reached its RootFS consumer\n' >&2
    kill "$runner_use_pid" 2>/dev/null || true
    wait "$runner_use_pid" 2>/dev/null || true
    exit 1
}
python3 - "$SHADOW_REPO/build/.rootfs-build.lock" <<'PY'
import errno
import fcntl
import os
import sys

fd = os.open(sys.argv[1], os.O_RDWR)
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except OSError as error:
    if error.errno in (errno.EACCES, errno.EAGAIN):
        raise SystemExit(0)
    raise
raise SystemExit("runner released RootFS lock before its consumer finished")
PY
: > "$TEST_ROOT/runner-use.continue"
wait "$runner_use_pid" || {
    printf 'runner failed after its use-lock fixture was released\n' >&2
    sed -n '1,200p' "$TEST_ROOT/runner-use-lock.log" >&2
    exit 1
}

# A consumer may intentionally leave its own background process alive. That
# process must not inherit the runner's generation-lock descriptors; once the
# runner exits, a fresh builder must be able to take the lock immediately.
cat > "$SHADOW_REPO/build-host/procfs_test" <<'BACKGROUND_PROCFS'
#!/usr/bin/env bash
set -euo pipefail
/bin/sleep 30 </dev/null >/dev/null 2>&1 &
printf '%s\n' "$!" > "${RUNNER_LEAK_PID_FILE:?}"
BACKGROUND_PROCFS
chmod +x "$SHADOW_REPO/build-host/procfs_test"
RUNNER_LEAK_PID_FILE="$TEST_ROOT/runner-leak.pid" \
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    > "$TEST_ROOT/runner-fd-isolation.log" 2>&1
RUNNER_LEAK_PID="$(cat "$TEST_ROOT/runner-leak.pid")"
kill -0 "$RUNNER_LEAK_PID" 2>/dev/null || {
    printf 'background consumer fixture did not remain alive after runner exit\n' >&2
    exit 1
}
set +e
python3 - "$SHADOW_REPO/build/.rootfs-build.lock" <<'PY'
import errno
import fcntl
import os
import sys

fd = os.open(sys.argv[1], os.O_RDWR)
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except OSError as error:
    if error.errno in (errno.EACCES, errno.EAGAIN):
        raise SystemExit(75)
    raise
PY
runner_reacquire_rc=$?
set -e
kill "$RUNNER_LEAK_PID" 2>/dev/null || true
RUNNER_LEAK_PID=""
[[ "$runner_reacquire_rc" == 0 ]] || {
    printf 'background consumer inherited the released RootFS lock (rc=%s)\n' \
        "$runner_reacquire_rc" >&2
    sed -n '1,200p' "$TEST_ROOT/runner-fd-isolation.log" >&2
    exit 1
}
write_exit_program "$SHADOW_REPO/build-host/procfs_test" 0

sed 's/^ISH_GUEST_ARCH=arm64$/ISH_GUEST_ARCH=i386/' \
    "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity" \
    > "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity.tmp"
mv "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity.tmp" \
    "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity"
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    > "$TEST_ROOT/stale-rootfs.log" 2>&1
[[ -f "$SHADOW_REPO/build/rootfs-builder-called" ]] || {
    printf 'runner did not rebuild a stale i386 RootFS identity\n' >&2
    sed -n '1,220p' "$TEST_ROOT/stale-rootfs.log" >&2
    exit 1
}
grep -q '^ISH_GUEST_ARCH=arm64$' \
    "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity" || {
    printf 'rebuilt RootFS did not publish the arm64 identity\n' >&2
    exit 1
}

rm -f "$SHADOW_REPO/build/rootfs-builder-called"
sed 's/^ROOTFS_INPUTS_SHA256=.*$/ROOTFS_INPUTS_SHA256=0000000000000000000000000000000000000000000000000000000000000000/' \
    "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity" \
    > "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity.tmp"
mv "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity.tmp" \
    "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity"
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    > "$TEST_ROOT/stale-pin.log" 2>&1
[[ -f "$SHADOW_REPO/build/rootfs-builder-called" ]] || {
    printf 'runner did not rebuild a RootFS with a stale input pin\n' >&2
    sed -n '1,220p' "$TEST_ROOT/stale-pin.log" >&2
    exit 1
}
rm -f \
    "$SHADOW_REPO/build/rootfs-builder-called" \
    "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity"
PATH="$FAKE_BIN:$PATH" \
    "$SHADOW_REPO/scripts/run-host-tests.sh" \
    > "$TEST_ROOT/legacy-rootfs.log" 2>&1
[[ -f "$SHADOW_REPO/build/rootfs-builder-called" ]] || {
    printf 'runner did not rebuild a legacy RootFS without an identity marker\n' >&2
    sed -n '1,220p' "$TEST_ROOT/legacy-rootfs.log" >&2
    exit 1
}
grep -q 'missing or unsafe RootFS identity marker' \
    "$TEST_ROOT/legacy-rootfs.log" || {
    printf 'runner did not classify a missing identity marker as stale\n' >&2
    sed -n '1,220p' "$TEST_ROOT/legacy-rootfs.log" >&2
    exit 1
}
[[ -f "$SHADOW_REPO/build/fs/.ishembed-rootfs-identity" ]] || {
    printf 'legacy RootFS rebuild did not publish a new identity marker\n' >&2
    exit 1
}

# Exercise the real builder's lock, same-volume staging, content seal and
# no-clobber publication without downloading Alpine or invoking a toolchain.
ATOMIC_ROOT="$TEST_ROOT/atomic-builder"
ATOMIC_BUILD="$ATOMIC_ROOT/build"
ATOMIC_SOURCE="$ATOMIC_ROOT/alpine"
ATOMIC_BIN="$ATOMIC_ROOT/bin"
ATOMIC_FAKEFSIFY="$ATOMIC_BIN/fakefsify-fixture"
ATOMIC_COUNT="$ATOMIC_ROOT/fakefsify-count"
mkdir -p \
    "$ATOMIC_BUILD/dl" \
    "$ATOMIC_SOURCE/bin" \
    "$ATOMIC_SOURCE/etc/apk" \
    "$ATOMIC_SOURCE/usr/share/identity-probe" \
    "$ATOMIC_BIN"

python3 - "$ATOMIC_SOURCE/bin/busybox" <<'PY'
import sys
header = bytearray(20)
header[:4] = b"\x7fELF"
header[4] = 2
header[5] = 1
with open(sys.argv[1], "wb") as stream:
    stream.write(header)
    stream.write(b"test busybox payload\n")
PY
chmod 0755 "$ATOMIC_SOURCE/bin/busybox"
printf 'ID=alpine\nVERSION_ID=3.19.1\n' > "$ATOMIC_SOURCE/etc/os-release"
for probe_index in $(seq 1 120); do
    printf 'probe %s\n' "$probe_index" \
        > "$ATOMIC_SOURCE/usr/share/identity-probe/$probe_index"
done

cat > "$ATOMIC_BIN/make" <<'FAKE_MAKE'
#!/usr/bin/env bash
set -euo pipefail
out_dir=""
for argument in "$@"; do
    case "$argument" in
        OUT_DIR=*) out_dir="${argument#OUT_DIR=}" ;;
    esac
done
[[ -n "$out_dir" ]] || exit 64
mkdir -p "$out_dir"
python3 - "$out_dir/ishsv" <<'PY'
import sys
header = bytearray(20)
header[:4] = b"\x7fELF"
header[4] = 2
header[5] = 1
header[18:20] = (183).to_bytes(2, "little")
with open(sys.argv[1], "wb") as stream:
    stream.write(header)
    stream.write(b"test supervisor payload\n")
PY
chmod 0755 "$out_dir/ishsv"
FAKE_MAKE
chmod +x "$ATOMIC_BIN/make"

cat > "$ATOMIC_FAKEFSIFY" <<'FAKEFSIFY'
#!/usr/bin/env bash
set -euo pipefail
archive="$1"
rootfs="$2"
printf 'import\n' >> "${FAKEFSIFY_COUNT:?}"
sleep "${FAKEFSIFY_DELAY:-0}"
mkdir -p "$rootfs/data"
tar -xzf "$archive" -C "$rootfs/data"
python3 - "$rootfs" <<'PY'
import os
import sqlite3
import sys

root = sys.argv[1]
data = os.path.join(root, "data")
connection = sqlite3.connect(os.path.join(root, "meta.db"))
connection.executescript("""
pragma user_version = 3;
create table meta (id integer unique default 0, db_inode integer);
create table stats (inode integer primary key, stat blob);
create table paths (path blob primary key, inode integer references stats(inode));
create index inode_to_path on paths (inode, path);
""")
entries = [(b"", data)]
for directory, names, files in os.walk(os.fsencode(data), topdown=True):
    names.sort()
    files.sort()
    for name in names + files:
        full = os.path.join(directory, name)
        relative = os.path.relpath(full, os.fsencode(data))
        entries.append((b"/" + relative, full))
for inode, (path, _) in enumerate(entries, 1000):
    connection.execute("insert into stats(inode, stat) values (?, ?)", (inode, bytes(16)))
    connection.execute("insert into paths(path, inode) values (?, ?)", (path, inode))
connection.execute("insert into meta(id, db_inode) values (0, 1000)")
connection.commit()
connection.close()
PY
FAKEFSIFY
chmod +x "$ATOMIC_FAKEFSIFY"

ATOMIC_TARBALL="$ATOMIC_BUILD/dl/alpine-minirootfs-3.19.1-aarch64.tar.gz"
tar -C "$ATOMIC_SOURCE" -czf "$ATOMIC_TARBALL" .
ATOMIC_ALPINE_SHA="$(shasum -a 256 "$ATOMIC_TARBALL" | awk '{print $1}')"

run_atomic_builder() {
    PATH="$ATOMIC_BIN:$PATH" \
        BUILD_DIR="$ATOMIC_BUILD" \
        ISH_SRC="$PKG_ROOT/third_party/ish" \
        FAKEFSIFY_BIN="$ATOMIC_FAKEFSIFY" \
        FAKEFSIFY_COUNT="$ATOMIC_COUNT" \
        FAKEFSIFY_DELAY="${FAKEFSIFY_DELAY_OVERRIDE:-1}" \
        ROOTFS_REUSE_VALID="${ROOTFS_REUSE_VALID_OVERRIDE:-1}" \
        ROOTFS_LOCK_TIMEOUT_SECONDS="${ROOTFS_LOCK_TIMEOUT_SECONDS_OVERRIDE:-300}" \
        ROOTFS_TEST_FAIL_AFTER_PUBLISH="${ROOTFS_TEST_FAIL_AFTER_PUBLISH:-}" \
        ROOTFS_TEST_READY_AFTER_PUBLISH="${ROOTFS_TEST_READY_AFTER_PUBLISH:-}" \
        ROOTFS_TEST_READY_IN_JOURNAL_GAP="${ROOTFS_TEST_READY_IN_JOURNAL_GAP:-}" \
        ROOTFS_TEST_PAUSE_IN_JOURNAL_GAP="${ROOTFS_TEST_PAUSE_IN_JOURNAL_GAP:-}" \
        ROOTFS_TEST_READY_FILE="${ROOTFS_TEST_READY_FILE:-}" \
        ROOTFS_TEST_CONTINUE_FILE="${ROOTFS_TEST_CONTINUE_FILE:-}" \
        ALPINE_VERSION=3.19.1 \
        ALPINE_SHA256="$ATOMIC_ALPINE_SHA" \
        "$ROOTFS_BUILDER"
}

verify_atomic_rootfs() {
    PATH="$ATOMIC_BIN:$PATH" \
        ISH_SRC="$PKG_ROOT/third_party/ish" \
        FAKEFSIFY_BIN="$ATOMIC_FAKEFSIFY" \
        ALPINE_VERSION=3.19.1 \
        ALPINE_SHA256="$ATOMIC_ALPINE_SHA" \
        "$ROOTFS_BUILDER" --verify-rootfs "$ATOMIC_BUILD/fs"
}

verify_atomic_bundle() {
    PATH="$ATOMIC_BIN:$PATH" \
        ISH_SRC="$PKG_ROOT/third_party/ish" \
        FAKEFSIFY_BIN="$ATOMIC_FAKEFSIFY" \
        ALPINE_VERSION=3.19.1 \
        ALPINE_SHA256="$ATOMIC_ALPINE_SHA" \
        "$ROOTFS_BUILDER" --verify-bundle "$ATOMIC_BUILD"
}

tree_sha256() {
    local tree_path="$1"
    python3 - "$tree_path" <<'PY'
import hashlib
import os
import stat
import sys

root = os.fsencode(os.path.realpath(sys.argv[1]))
digest = hashlib.sha256()
entries = []
for directory, names, files in os.walk(root, topdown=True, followlinks=False):
    names.sort()
    files.sort()
    for name in names + files:
        full = os.path.join(directory, name)
        entries.append((os.path.relpath(full, root), full))
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
        raise SystemExit("unexpected non-file tree entry")
print(digest.hexdigest())
PY
}

atomic_bundle_snapshot() {
    printf '%s|%s|%s|%s\n' \
        "$(tree_sha256 "$ATOMIC_BUILD/fs")" \
        "$(shasum -a 256 "$ATOMIC_BUILD/fs.tar.gz" | awk '{print $1}')" \
        "$(shasum -a 256 "$ATOMIC_BUILD/SHA256SUMS" | awk '{print $1}')" \
        "$(shasum -a 256 "$ATOMIC_BUILD/ROOTFS_RECEIPT" | awk '{print $1}')"
}

run_atomic_builder > "$ATOMIC_ROOT/build-one.log" 2>&1 &
atomic_pid_one=$!
run_atomic_builder > "$ATOMIC_ROOT/build-two.log" 2>&1 &
atomic_pid_two=$!
set +e
wait "$atomic_pid_one"
atomic_rc_one=$?
wait "$atomic_pid_two"
atomic_rc_two=$?
set -e
[[ "$atomic_rc_one" == 0 && "$atomic_rc_two" == 0 ]] || {
    printf 'concurrent RootFS builders failed: first=%s second=%s\n' \
        "$atomic_rc_one" "$atomic_rc_two" >&2
    sed -n '1,240p' "$ATOMIC_ROOT/build-one.log" >&2
    sed -n '1,240p' "$ATOMIC_ROOT/build-two.log" >&2
    exit 1
}
[[ "$(wc -l < "$ATOMIC_COUNT" | tr -d ' ')" == 1 ]] || {
    printf 'RootFS build lock did not collapse concurrent imports\n' >&2
    sed -n '1,240p' "$ATOMIC_ROOT/build-one.log" >&2
    sed -n '1,240p' "$ATOMIC_ROOT/build-two.log" >&2
    exit 1
}
grep -q 'Reusing validated RootFS' \
    "$ATOMIC_ROOT/build-one.log" "$ATOMIC_ROOT/build-two.log" || {
    printf 'second RootFS builder did not reuse the first atomic publication\n' >&2
    exit 1
}

PATH="$ATOMIC_BIN:$PATH" \
    ISH_SRC="$PKG_ROOT/third_party/ish" \
    FAKEFSIFY_BIN="$ATOMIC_FAKEFSIFY" \
    ALPINE_VERSION=3.19.1 \
    ALPINE_SHA256="$ATOMIC_ALPINE_SHA" \
    "$ROOTFS_BUILDER" --verify-rootfs "$ATOMIC_BUILD/fs"
[[ -f "$ATOMIC_BUILD/.rootfs-build.lock" && ! -L "$ATOMIC_BUILD/.rootfs-build.lock" ]] || {
    printf 'RootFS build lock is not a stable regular file after concurrent builders\n' >&2
    exit 1
}
ATOMIC_LOCK_INODE="$(python3 - "$ATOMIC_BUILD/.rootfs-build.lock" <<'PY'
import os
import sys
info = os.lstat(sys.argv[1])
print(f"{info.st_dev}:{info.st_ino}")
PY
)"

# Lock liveness comes exclusively from flock on this stable inode. Empty,
# corrupt, and PID-reuse-looking owner bytes from a dead holder must therefore
# be overwritten safely without moving the inode or repeating the import.
for stale_lock_kind in empty corrupt pid-reuse; do
    case "$stale_lock_kind" in
        empty) : > "$ATOMIC_BUILD/.rootfs-build.lock" ;;
        corrupt) printf 'not a lock owner\n' > "$ATOMIC_BUILD/.rootfs-build.lock" ;;
        pid-reuse)
            printf '%s\n' \
                'LOCK_SCHEMA=1' \
                "PID=$$" \
                'START_ID=darwin:0000000000000000000000000000000000000000000000000000000000000000' \
                'TOKEN=000000000000000000000000000000000000000000000000' \
                > "$ATOMIC_BUILD/.rootfs-build.lock"
            ;;
    esac
    FAKEFSIFY_DELAY_OVERRIDE=0 run_atomic_builder \
        > "$ATOMIC_ROOT/stale-lock-${stale_lock_kind}.log" 2>&1 || {
        printf 'builder did not safely recover %s lock metadata\n' "$stale_lock_kind" >&2
        sed -n '1,200p' "$ATOMIC_ROOT/stale-lock-${stale_lock_kind}.log" >&2
        exit 1
    }
    grep -Eq '^LOCK_SCHEMA=1$' "$ATOMIC_BUILD/.rootfs-build.lock" || {
        printf 'builder did not rewrite %s lock metadata\n' "$stale_lock_kind" >&2
        exit 1
    }
done
[[ "$(wc -l < "$ATOMIC_COUNT" | tr -d ' ')" == 1 ]] || {
    printf 'stale lock metadata recovery rebuilt a reusable RootFS\n' >&2
    exit 1
}
[[ "$(python3 - "$ATOMIC_BUILD/.rootfs-build.lock" <<'PY'
import os
import sys
info = os.lstat(sys.argv[1])
print(f"{info.st_dev}:{info.st_ino}")
PY
)" == "$ATOMIC_LOCK_INODE" ]] || {
    printf 'stale lock metadata recovery replaced the stable lock inode\n' >&2
    exit 1
}

rm -f "$ATOMIC_ROOT/live-lock.ready" "$ATOMIC_ROOT/live-lock.continue"
python3 - \
    "$ATOMIC_BUILD/.rootfs-build.lock" \
    "$ATOMIC_ROOT/live-lock.ready" \
    "$ATOMIC_ROOT/live-lock.continue" <<'PY' &
import fcntl
import os
import sys
import time

lock_path, ready, proceed = sys.argv[1:]
with open(lock_path, "r+") as stream:
    fcntl.flock(stream, fcntl.LOCK_EX)
    stream.seek(0)
    stream.truncate()
    stream.write("corrupt-but-live\n")
    stream.flush()
    os.fsync(stream.fileno())
    open(ready, "wb").close()
    while not os.path.exists(proceed):
        time.sleep(0.05)
PY
live_lock_pid=$!
for _ in $(seq 1 200); do
    [[ -e "$ATOMIC_ROOT/live-lock.ready" ]] && break
    sleep 0.05
done
[[ -e "$ATOMIC_ROOT/live-lock.ready" ]] || {
    printf 'live-lock interleaving fixture did not acquire flock\n' >&2
    kill "$live_lock_pid" 2>/dev/null || true
    wait "$live_lock_pid" 2>/dev/null || true
    exit 1
}
set +e
ROOTFS_LOCK_TIMEOUT_SECONDS_OVERRIDE=0 FAKEFSIFY_DELAY_OVERRIDE=0 \
    run_atomic_builder > "$ATOMIC_ROOT/live-lock-contender.log" 2>&1
live_contender_rc=$?
set -e
[[ "$live_contender_rc" == 75 ]] || {
    printf 'contender reclaimed a live corrupt-metadata lock (rc=%s)\n' \
        "$live_contender_rc" >&2
    exit 1
}
: > "$ATOMIC_ROOT/live-lock.continue"
wait "$live_lock_pid"
FAKEFSIFY_DELAY_OVERRIDE=0 run_atomic_builder \
    > "$ATOMIC_ROOT/live-lock-after-release.log" 2>&1
[[ "$(wc -l < "$ATOMIC_COUNT" | tr -d ' ')" == 1 ]] || {
    printf 'live-lock contention rebuilt a reusable RootFS\n' >&2
    exit 1
}
if find "$ATOMIC_BUILD" -maxdepth 1 -name '.fs.staging.*' -print | grep . >/dev/null; then
    printf 'RootFS staging directory remained after publication\n' >&2
    exit 1
fi
verify_atomic_bundle

# fakefs reads a versioned, exact SQLite schema and each stats.stat value as a
# fixed 16-byte record. Reject schema drift and malformed rows before native
# code can dereference the database.
cp "$ATOMIC_BUILD/fs/meta.db" "$ATOMIC_ROOT/meta.db.valid"
for sqlite_case in \
    user-version missing-index missing-pk reordered-columns missing-fk \
    stat-length nonpositive-inode duplicate-root; do
    cp "$ATOMIC_ROOT/meta.db.valid" "$ATOMIC_BUILD/fs/meta.db"
    python3 - "$ATOMIC_BUILD/fs/meta.db" "$sqlite_case" <<'PY'
import sqlite3
import sys

path, case = sys.argv[1:]
connection = sqlite3.connect(path)
if case == "user-version":
    connection.execute("pragma user_version = 2")
elif case == "missing-index":
    connection.execute("drop index inode_to_path")
elif case == "missing-pk":
    connection.executescript("""
        alter table stats rename to stats_old;
        create table stats (inode integer, stat blob);
        insert into stats select inode, stat from stats_old;
        drop table stats_old;
    """)
elif case == "reordered-columns":
    connection.executescript("""
        alter table paths rename to paths_old;
        create table paths (
            inode integer references stats(inode),
            path blob primary key
        );
        insert into paths(inode, path) select inode, path from paths_old;
        drop table paths_old;
        create index inode_to_path on paths (inode, path);
    """)
elif case == "missing-fk":
    connection.executescript("""
        alter table paths rename to paths_old;
        create table paths (path blob primary key, inode integer);
        insert into paths(path, inode) select path, inode from paths_old;
        drop table paths_old;
        create index inode_to_path on paths (inode, path);
    """)
elif case == "stat-length":
    connection.execute(
        "update stats set stat = ? where inode = (select min(inode) from stats)",
        (b"short",),
    )
elif case == "nonpositive-inode":
    inode = connection.execute("select max(inode) from stats").fetchone()[0]
    connection.execute("update stats set inode = -1 where inode = ?", (inode,))
    connection.execute("update paths set inode = -1 where inode = ?", (inode,))
elif case == "duplicate-root":
    root_inode = connection.execute(
        "select inode from paths where typeof(path) = 'blob' and length(path) = 0"
    ).fetchone()[0]
    connection.executescript("""
        alter table paths rename to paths_old;
        create table paths (path blob, inode integer);
        insert into paths select path, inode from paths_old;
        drop table paths_old;
    """)
    connection.execute("insert into paths(path, inode) values (?, ?)", (b"", root_inode))
else:
    raise SystemExit("unknown malformed SQLite case")
connection.commit()
connection.close()
PY
    set +e
    verify_atomic_rootfs > "$ATOMIC_ROOT/sqlite-${sqlite_case}.log" 2>&1
    sqlite_case_rc=$?
    set -e
    [[ "$sqlite_case_rc" -ne 0 ]] || {
        printf 'malformed SQLite fixture %s passed RootFS verification\n' "$sqlite_case" >&2
        exit 1
    }
done
cp "$ATOMIC_ROOT/meta.db.valid" "$ATOMIC_BUILD/fs/meta.db"
verify_atomic_rootfs

# A non-empty WAL may contain committed rows absent from meta.db, and any
# rollback journal denotes an ambiguous recovery state. Both must fail closed;
# empty WAL/SHM placeholders remain compatible with current fakefs output.
for sqlite_sidecar in meta.db-wal meta.db-journal; do
    printf 'uncommitted sqlite sidecar\n' \
        > "$ATOMIC_BUILD/fs/$sqlite_sidecar"
    set +e
    verify_atomic_rootfs \
        > "$ATOMIC_ROOT/sqlite-${sqlite_sidecar}.log" 2>&1
    sqlite_sidecar_rc=$?
    set -e
    [[ "$sqlite_sidecar_rc" -ne 0 ]] || {
        printf 'unsafe SQLite sidecar %s passed RootFS verification\n' \
            "$sqlite_sidecar" >&2
        exit 1
    }
    rm -f "$ATOMIC_BUILD/fs/$sqlite_sidecar"
done
: > "$ATOMIC_BUILD/fs/meta.db-wal"
: > "$ATOMIC_BUILD/fs/meta.db-shm"
verify_atomic_rootfs
rm -f "$ATOMIC_BUILD/fs/meta.db-wal" "$ATOMIC_BUILD/fs/meta.db-shm"

# A valid runtime mutation changes the original meta/data seal but retains full
# fakefs consistency; reuse verification must accept it rather than rebuilding
# after every smoke run.
python3 - "$ATOMIC_BUILD/fs" <<'PY'
import os
import sqlite3
import sys

root = sys.argv[1]
path = os.path.join(root, "data", "var", "runtime-state")
os.makedirs(os.path.dirname(path), exist_ok=True)
with open(path, "wb") as stream:
    stream.write(b"runtime mutation\n")
connection = sqlite3.connect(os.path.join(root, "meta.db"))
next_inode = connection.execute("select max(inode) + 1 from stats").fetchone()[0]
for fake_path in (b"/var", b"/var/runtime-state"):
    if connection.execute("select 1 from paths where path = ?", (fake_path,)).fetchone():
        continue
    connection.execute("insert into stats(inode, stat) values (?, ?)", (next_inode, bytes(16)))
    connection.execute("insert into paths(path, inode) values (?, ?)", (fake_path, next_inode))
    next_inode += 1
connection.commit()
connection.close()
PY
PATH="$ATOMIC_BIN:$PATH" \
    ISH_SRC="$PKG_ROOT/third_party/ish" \
    FAKEFSIFY_BIN="$ATOMIC_FAKEFSIFY" \
    ALPINE_VERSION=3.19.1 \
    ALPINE_SHA256="$ATOMIC_ALPINE_SHA" \
    "$ROOTFS_BUILDER" --verify-rootfs "$ATOMIC_BUILD/fs"

# Reuse admission is for the committed four-artifact bundle, not just fs/.
# Corrupting any companion must invalidate the generation receipt.
for bundle_case in tar sums receipt; do
    case "$bundle_case" in
        tar) bundle_path="$ATOMIC_BUILD/fs.tar.gz" ;;
        sums) bundle_path="$ATOMIC_BUILD/SHA256SUMS" ;;
        receipt) bundle_path="$ATOMIC_BUILD/ROOTFS_RECEIPT" ;;
    esac
    cp "$bundle_path" "$ATOMIC_ROOT/${bundle_case}.saved"
    printf 'tampered %s\n' "$bundle_case" >> "$bundle_path"
    set +e
    verify_atomic_bundle > "$ATOMIC_ROOT/tampered-${bundle_case}.log" 2>&1
    bundle_case_rc=$?
    set -e
    [[ "$bundle_case_rc" -ne 0 ]] || {
        printf 'tampered bundle component %s passed verification\n' "$bundle_case" >&2
        exit 1
    }
    cp "$ATOMIC_ROOT/${bundle_case}.saved" "$bundle_path"
done
verify_atomic_bundle

cp "$ATOMIC_BUILD/fs/data/sbin/ishsv" "$ATOMIC_ROOT/ishsv.saved"
printf 'tampered supervisor\n' >> "$ATOMIC_BUILD/fs/data/sbin/ishsv"
set +e
PATH="$ATOMIC_BIN:$PATH" \
    ISH_SRC="$PKG_ROOT/third_party/ish" \
    FAKEFSIFY_BIN="$ATOMIC_FAKEFSIFY" \
    ALPINE_VERSION=3.19.1 \
    ALPINE_SHA256="$ATOMIC_ALPINE_SHA" \
    "$ROOTFS_BUILDER" --verify-rootfs "$ATOMIC_BUILD/fs" \
    > "$ATOMIC_ROOT/tampered-supervisor.log" 2>&1
tampered_supervisor_rc=$?
set -e
[[ "$tampered_supervisor_rc" -ne 0 ]] || {
    printf 'valid marker admitted a tampered RootFS supervisor\n' >&2
    exit 1
}
grep -q 'supervisor does not match its content identity' \
    "$ATOMIC_ROOT/tampered-supervisor.log" || {
    printf 'tampered supervisor rejection did not identify the stale binary\n' >&2
    sed -n '1,160p' "$ATOMIC_ROOT/tampered-supervisor.log" >&2
    exit 1
}
cp "$ATOMIC_ROOT/ishsv.saved" "$ATOMIC_BUILD/fs/data/sbin/ishsv"

# A staged failure must leave the previously published, mutable RootFS intact.
cat > "$ATOMIC_BIN/fakefsify-fail" <<'FAKEFSIFY_FAIL'
#!/usr/bin/env bash
exit 88
FAKEFSIFY_FAIL
chmod +x "$ATOMIC_BIN/fakefsify-fail"
set +e
PATH="$ATOMIC_BIN:$PATH" \
    BUILD_DIR="$ATOMIC_BUILD" \
    ISH_SRC="$PKG_ROOT/third_party/ish" \
    FAKEFSIFY_BIN="$ATOMIC_BIN/fakefsify-fail" \
    FAKEFSIFY_COUNT="$ATOMIC_COUNT" \
    ALPINE_VERSION=3.19.1 \
    ALPINE_SHA256="$ATOMIC_ALPINE_SHA" \
    "$ROOTFS_BUILDER" > "$ATOMIC_ROOT/failed-build.log" 2>&1
failed_build_rc=$?
set -e
[[ "$failed_build_rc" == 88 ]] || {
    printf 'expected staged fakefsify failure 88, got %s\n' "$failed_build_rc" >&2
    sed -n '1,240p' "$ATOMIC_ROOT/failed-build.log" >&2
    exit 1
}
[[ -f "$ATOMIC_BUILD/fs/data/var/runtime-state" ]] || {
    printf 'failed staged build clobbered the previously published RootFS\n' >&2
    exit 1
}
[[ -f "$ATOMIC_BUILD/.rootfs-build.lock" && ! -L "$ATOMIC_BUILD/.rootfs-build.lock" ]] || {
    printf 'RootFS build lock is not a stable regular file after failed cleanup\n' >&2
    exit 1
}
[[ "$(python3 - "$ATOMIC_BUILD/.rootfs-build.lock" <<'PY'
import os
import sys
info = os.lstat(sys.argv[1])
print(f"{info.st_dev}:{info.st_ino}")
PY
)" == "$ATOMIC_LOCK_INODE" ]] || {
    printf 'RootFS lock inode was replaced across acquisitions\n' >&2
    exit 1
}
if find "$ATOMIC_BUILD" -maxdepth 1 -name '.fs.staging.*' -print | grep . >/dev/null; then
    printf 'failed RootFS build left a staging directory behind\n' >&2
    exit 1
fi

# Every publication step is journaled and reversible. Inject a failure after
# each component, including the receipt commit point, and require the exact old
# mutable tree plus all three companion files to survive.
transaction_baseline="$(atomic_bundle_snapshot)"
for publish_step in fs tar sums receipt; do
    set +e
    ROOTFS_REUSE_VALID_OVERRIDE=0 \
    FAKEFSIFY_DELAY_OVERRIDE=0 \
    ROOTFS_TEST_FAIL_AFTER_PUBLISH="$publish_step" \
        run_atomic_builder > "$ATOMIC_ROOT/fail-after-${publish_step}.log" 2>&1
    publish_failure_rc=$?
    set -e
    [[ "$publish_failure_rc" == 76 ]] || {
        printf 'expected publish-step %s failure 76, got %s\n' \
            "$publish_step" "$publish_failure_rc" >&2
        sed -n '1,260p' "$ATOMIC_ROOT/fail-after-${publish_step}.log" >&2
        exit 1
    }
    [[ "$(atomic_bundle_snapshot)" == "$transaction_baseline" ]] || {
        printf 'publish-step %s failure did not restore the prior bundle\n' \
            "$publish_step" >&2
        exit 1
    }
    verify_atomic_bundle
done

# Exercise the otherwise tiny rename-to-journal signal window deterministically.
# Exchange means fs/ remains continuously addressable while the builder pauses;
# deferred TERM is delivered only after the inverse operation is recorded.
rm -f "$ATOMIC_ROOT/signal-gap.ready" "$ATOMIC_ROOT/signal-gap.continue"
env \
    PATH="$ATOMIC_BIN:$PATH" \
    BUILD_DIR="$ATOMIC_BUILD" \
    ISH_SRC="$PKG_ROOT/third_party/ish" \
    FAKEFSIFY_BIN="$ATOMIC_FAKEFSIFY" \
    FAKEFSIFY_COUNT="$ATOMIC_COUNT" \
    FAKEFSIFY_DELAY=0 \
    ROOTFS_REUSE_VALID=0 \
    ROOTFS_TEST_READY_IN_JOURNAL_GAP=fs \
    ROOTFS_TEST_PAUSE_IN_JOURNAL_GAP=fs \
    ROOTFS_TEST_READY_FILE="$ATOMIC_ROOT/signal-gap.ready" \
    ROOTFS_TEST_CONTINUE_FILE="$ATOMIC_ROOT/signal-gap.continue" \
    ALPINE_VERSION=3.19.1 \
    ALPINE_SHA256="$ATOMIC_ALPINE_SHA" \
    "$ROOTFS_BUILDER" > "$ATOMIC_ROOT/signal-gap.log" 2>&1 &
signal_builder_pid=$!
for _ in $(seq 1 400); do
    [[ -e "$ATOMIC_ROOT/signal-gap.ready" ]] && break
    sleep 0.05
done
[[ -e "$ATOMIC_ROOT/signal-gap.ready" ]] || {
    printf 'builder never reached the rename-to-journal signal fixture\n' >&2
    kill "$signal_builder_pid" 2>/dev/null || true
    wait "$signal_builder_pid" 2>/dev/null || true
    sed -n '1,260p' "$ATOMIC_ROOT/signal-gap.log" >&2
    exit 1
}
for _ in $(seq 1 200); do
    [[ -d "$ATOMIC_BUILD/fs" && ! -L "$ATOMIC_BUILD/fs" ]] || {
        printf 'RootFS target had a visible absence during replacement\n' >&2
        kill "$signal_builder_pid" 2>/dev/null || true
        wait "$signal_builder_pid" 2>/dev/null || true
        exit 1
    }
done
kill -TERM "$signal_builder_pid"
set +e
wait "$signal_builder_pid"
signal_builder_rc=$?
set -e
[[ "$signal_builder_rc" == 143 ]] || {
    printf 'expected deferred signal status 143, got %s\n' "$signal_builder_rc" >&2
    sed -n '1,300p' "$ATOMIC_ROOT/signal-gap.log" >&2
    exit 1
}
[[ "$(atomic_bundle_snapshot)" == "$transaction_baseline" ]] || {
    printf 'signal in rename-to-journal gap did not restore the prior bundle\n' >&2
    exit 1
}
verify_atomic_bundle

# Successful replacement discards the exchanged old generation after commit;
# failures preserve it transactionally, while success does not accumulate an
# implicit previous tree that callers might mistake for a supported backup.
ROOTFS_REUSE_VALID_OVERRIDE=0 FAKEFSIFY_DELAY_OVERRIDE=0 run_atomic_builder \
    > "$ATOMIC_ROOT/successful-replacement.log" 2>&1
verify_atomic_bundle
[[ ! -e "$ATOMIC_BUILD/fs/data/var/runtime-state" ]] || {
    printf 'successful RootFS replacement retained the old mutable generation\n' >&2
    exit 1
}
if find "$ATOMIC_BUILD" -maxdepth 1 \
    \( -name '.fs.staging.*' -o -name 'fs.previous.*' \) -print | grep . >/dev/null; then
    printf 'successful RootFS replacement retained a staging/previous generation\n' >&2
    exit 1
fi

printf 'host-test identity and status regressions passed.\n'
