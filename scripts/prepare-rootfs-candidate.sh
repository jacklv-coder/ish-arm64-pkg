#!/usr/bin/env bash
#
# Build the pinned RootFS twice with one content-addressed host fakefsify tool
# and require the complete candidate archives to match byte for byte.
#
# This prepares a local, unapproved distribution candidate. It does not create
# a GitHub Release or upload RootFS bytes.

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
ROOTFS_BUILDER="$PKG_ROOT/scripts/build-rootfs.sh"
ROOTFS_PIN="$PKG_ROOT/scripts/alpine-rootfs-pin.sh"
MODE=""
OUTPUT_DIR=""

usage() {
    printf 'usage: %s --verify-only | --output /absolute/outside-repository/path\n' \
        "$0"
}

case "${1:-}" in
    --verify-only)
        (( $# == 1 )) || {
            usage >&2
            exit 64
        }
        MODE="verify"
        ;;
    --output)
        (( $# == 2 )) || {
            usage >&2
            exit 64
        }
        MODE="output"
        OUTPUT_DIR="$2"
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 64
        ;;
esac

# Candidate inputs are repository-owned. Do not let an ambient development
# override silently change the reviewed pin, source tree, archiver, or locks.
unset \
    ALPINE_VERSION \
    ALPINE_ARCH \
    ALPINE_SHA256 \
    SOURCE_DATE_EPOCH \
    ROOTFS_ARCHIVER \
    ISH_SRC \
    FAKEFSIFY_BIN \
    ROOTFS_INPUTS_FILE \
    BUILD_DIR \
    ROOTFS_REUSE_VALID \
    ROOTFS_INHERITED_LOCK_TOKEN \
    ROOTFS_INHERITED_LOCK_PID

for tool in git meson ninja python3 shasum zig; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'ERROR: required RootFS candidate tool is missing: %s\n' \
            "$tool" >&2
        exit 69
    }
done

git -C "$PKG_ROOT" diff --quiet --ignore-submodules=none
git -C "$PKG_ROOT" diff --cached --quiet --ignore-submodules=none
if [[ -n "$(git -C "$PKG_ROOT" status --porcelain --untracked-files=normal)" ]]; then
    printf 'ERROR: RootFS candidate preparation requires a clean worktree\n' >&2
    exit 65
fi

HEAD_REVISION="$(git -C "$PKG_ROOT" rev-parse HEAD)"
ISH_REVISION="$(git -C "$PKG_ROOT/third_party/ish" rev-parse HEAD)"
EXPECTED_ISH_REVISION="$(
    git -C "$PKG_ROOT" ls-tree HEAD third_party/ish | awk '{print $3}'
)"
[[ "$ISH_REVISION" == "$EXPECTED_ISH_REVISION" ]] || {
    printf 'ERROR: checked-out iSH revision does not match the parent gitlink\n' >&2
    exit 65
}
PIN_SHA256="$(shasum -a 256 "$ROOTFS_PIN" | awk '{print $1}')"
CANDIDATE_SCRIPT_SHA256="$(shasum -a 256 "$0" | awk '{print $1}')"

if [[ "$MODE" == "output" ]]; then
    case "$OUTPUT_DIR" in
        /*) ;;
        *)
            printf 'ERROR: RootFS candidate output must be an absolute path\n' >&2
            exit 64
            ;;
    esac
    OUTPUT_PARENT="$(dirname "$OUTPUT_DIR")"
    [[ -d "$OUTPUT_PARENT" && ! -L "$OUTPUT_PARENT" ]] || {
        printf 'ERROR: RootFS candidate output parent is not a real directory: %s\n' \
            "$OUTPUT_PARENT" >&2
        exit 64
    }
    OUTPUT_PARENT="$(cd "$OUTPUT_PARENT" && pwd -P)"
    OUTPUT_DIR="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
    case "$OUTPUT_DIR/" in
        "$PKG_ROOT/"*)
            printf 'ERROR: RootFS candidate output must stay outside the repository\n' >&2
            exit 64
            ;;
    esac
    [[ ! -e "$OUTPUT_DIR" && ! -L "$OUTPUT_DIR" ]] || {
        printf 'ERROR: refusing to replace existing candidate output: %s\n' \
            "$OUTPUT_DIR" >&2
        exit 73
    }
fi

WORK_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-rootfs-candidate.XXXXXX")"
CANDIDATE_STAGE=""

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    rm -rf -- "$WORK_ROOT"
    if [[ -n "$CANDIDATE_STAGE" && \
          ( -e "$CANDIDATE_STAGE" || -L "$CANDIDATE_STAGE" ) ]]; then
        rm -rf -- "$CANDIDATE_STAGE"
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM HUP

printf '==> Building one reviewed host fakefsify tool\n'
if command -v pkg-config >/dev/null 2>&1 && \
   ! pkg-config --exists libarchive >/dev/null 2>&1 && \
   command -v brew >/dev/null 2>&1; then
    LIBARCHIVE_PREFIX="$(brew --prefix libarchive 2>/dev/null || true)"
    if [[ -n "$LIBARCHIVE_PREFIX" && -d "$LIBARCHIVE_PREFIX/lib/pkgconfig" ]]; then
        export PKG_CONFIG_PATH="$LIBARCHIVE_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    fi
fi
meson setup "$WORK_ROOT/ish-host" "$PKG_ROOT/third_party/ish" \
    -Dguest_arch=arm64 >/dev/null
meson compile -C "$WORK_ROOT/ish-host" fakefsify
FAKEFSIFY="$WORK_ROOT/ish-host/tools/fakefsify"
[[ -f "$FAKEFSIFY" && -x "$FAKEFSIFY" && ! -L "$FAKEFSIFY" ]] || {
    printf 'ERROR: candidate fakefsify build did not produce a safe executable\n' >&2
    exit 70
}
FAKEFSIFY_SHA256="$(shasum -a 256 "$FAKEFSIFY" | awk '{print $1}')"

printf '==> Building first RootFS candidate\n'
FAKEFSIFY_BIN="$FAKEFSIFY" \
ROOTFS_INPUTS_FILE="$ROOTFS_PIN" \
BUILD_DIR="$WORK_ROOT/first" \
ROOTFS_REQUIRE_ABSENT=1 \
    "$ROOTFS_BUILDER"

mkdir -p "$WORK_ROOT/second/dl"
cp "$WORK_ROOT/first/dl/"alpine-minirootfs-*.tar.gz "$WORK_ROOT/second/dl/"

printf '==> Building independent second RootFS candidate\n'
FAKEFSIFY_BIN="$FAKEFSIFY" \
ROOTFS_INPUTS_FILE="$ROOTFS_PIN" \
BUILD_DIR="$WORK_ROOT/second" \
ROOTFS_REQUIRE_ABSENT=1 \
    "$ROOTFS_BUILDER"

for generation in first second; do
    FAKEFSIFY_BIN="$FAKEFSIFY" \
    ROOTFS_INPUTS_FILE="$ROOTFS_PIN" \
        "$ROOTFS_BUILDER" --verify-bundle "$WORK_ROOT/$generation"
done

for artifact in \
    fs.tar.gz \
    SHA256SUMS \
    ROOTFS_RECEIPT \
    fs/.ishembed-rootfs-identity \
    fs/meta.db; do
    cmp "$WORK_ROOT/first/$artifact" "$WORK_ROOT/second/$artifact" || {
        printf 'ERROR: RootFS candidate is not reproducible: %s differs\n' \
            "$artifact" >&2
        exit 70
    }
done
diff -qr "$WORK_ROOT/first/fs/data" "$WORK_ROOT/second/fs/data"

ROOTFS_SHA256="$(shasum -a 256 "$WORK_ROOT/first/fs.tar.gz" | awk '{print $1}')"
IDENTITY_SHA256="$(
    shasum -a 256 "$WORK_ROOT/first/fs/.ishembed-rootfs-identity" |
        awk '{print $1}'
)"
SUMS_SHA256="$(shasum -a 256 "$WORK_ROOT/first/SHA256SUMS" | awk '{print $1}')"
RECEIPT_SHA256="$(
    shasum -a 256 "$WORK_ROOT/first/ROOTFS_RECEIPT" | awk '{print $1}'
)"
ROOTFS_SIZE="$(stat -f '%z' "$WORK_ROOT/first/fs.tar.gz" 2>/dev/null || \
    stat -c '%s' "$WORK_ROOT/first/fs.tar.gz")"

printf '==> Reproducibility gate passed\n'
printf '    fs.tar.gz sha256: %s\n' "$ROOTFS_SHA256"
printf '    fs.tar.gz bytes:  %s\n' "$ROOTFS_SIZE"

if [[ "$MODE" == "verify" ]]; then
    printf 'No RootFS artifact was retained or uploaded.\n'
    exit 0
fi

CANDIDATE_STAGE="$(mktemp -d "$OUTPUT_PARENT/.rootfs-candidate.XXXXXX")"
cp "$WORK_ROOT/first/fs.tar.gz" "$CANDIDATE_STAGE/fs.tar.gz"
cp "$WORK_ROOT/first/SHA256SUMS" "$CANDIDATE_STAGE/SHA256SUMS"
cp "$WORK_ROOT/first/ROOTFS_RECEIPT" "$CANDIDATE_STAGE/ROOTFS_RECEIPT"
cp "$WORK_ROOT/first/fs/.ishembed-rootfs-identity" \
    "$CANDIDATE_STAGE/ROOTFS_IDENTITY"

python3 - \
    "$CANDIDATE_STAGE/ROOTFS_CANDIDATE.json" \
    "$HEAD_REVISION" \
    "$ISH_REVISION" \
    "$PIN_SHA256" \
    "$CANDIDATE_SCRIPT_SHA256" \
    "$FAKEFSIFY_SHA256" \
    "$ROOTFS_SHA256" \
    "$ROOTFS_SIZE" \
    "$IDENTITY_SHA256" \
    "$SUMS_SHA256" \
    "$RECEIPT_SHA256" <<'PY'
import json
import pathlib
import sys

(
    output,
    revision,
    ish_revision,
    pin_sha256,
    candidate_script_sha256,
    fakefsify_sha256,
    rootfs_sha256,
    rootfs_size,
    identity_sha256,
    sums_sha256,
    receipt_sha256,
) = sys.argv[1:]

document = {
    "schemaVersion": 1,
    "status": "local-unapproved-candidate",
    "reproducibility": {
        "buildCount": 2,
        "comparison": "byte-for-byte",
        "sharedHostToolSHA256": fakefsify_sha256,
    },
    "source": {
        "repository": "https://github.com/jacklv-coder/ish-arm64-pkg",
        "revision": revision,
        "ishRevision": ish_revision,
        "rootfsPinSHA256": pin_sha256,
        "candidateScriptSHA256": candidate_script_sha256,
    },
    "artifacts": {
        "fs.tar.gz": {
            "sha256": rootfs_sha256,
            "size": int(rootfs_size),
        },
        "ROOTFS_IDENTITY": {
            "sha256": identity_sha256,
        },
        "SHA256SUMS": {
            "sha256": sums_sha256,
        },
        "ROOTFS_RECEIPT": {
            "sha256": receipt_sha256,
        },
    },
    "distributionAuthorized": False,
}
pathlib.Path(output).write_text(
    json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY

chmod 0644 "$CANDIDATE_STAGE/"*
mv -n "$CANDIDATE_STAGE" "$OUTPUT_DIR"
[[ ! -e "$CANDIDATE_STAGE" && ! -L "$CANDIDATE_STAGE" ]] || {
    printf 'ERROR: candidate output appeared during publication: %s\n' \
        "$OUTPUT_DIR" >&2
    exit 73
}
CANDIDATE_STAGE=""

printf 'Local unapproved RootFS candidate: %s\n' "$OUTPUT_DIR"
printf 'This command did not create or modify a GitHub Release.\n'
