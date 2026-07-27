#!/usr/bin/env bash
#
# Build the pinned RootFS twice with one content-addressed host fakefsify tool
# and require the complete candidate archives to match byte for byte.
#
# This prepares a local, unapproved distribution candidate. It does not create
# a GitHub Release or upload RootFS bytes.

set -euo pipefail

# Re-enter once with a deliberately small environment. Tool lookup is restricted
# to system locations plus the Homebrew prefix matching the active macOS
# architecture; proxy/CA variables affect only the SHA-256-verified download
# transport. A disposable HOME prevents user tool configuration from silently
# changing the build.
UNAME_BIN="/usr/bin/uname"
[[ -x "$UNAME_BIN" ]] || UNAME_BIN="/bin/uname"
[[ -x "$UNAME_BIN" ]] || {
    printf 'ERROR: cannot locate the system uname executable\n' >&2
    exit 69
}
MKTEMP_BIN="/usr/bin/mktemp"
[[ -x "$MKTEMP_BIN" ]] || MKTEMP_BIN="/bin/mktemp"
[[ -x "$MKTEMP_BIN" ]] || {
    printf 'ERROR: cannot locate the system mktemp executable\n' >&2
    exit 69
}
CHMOD_BIN="/bin/chmod"
[[ -x "$CHMOD_BIN" ]] || CHMOD_BIN="/usr/bin/chmod"
[[ -x "$CHMOD_BIN" ]] || {
    printf 'ERROR: cannot locate the system chmod executable\n' >&2
    exit 69
}
RM_BIN="/bin/rm"
[[ -x "$RM_BIN" ]] || RM_BIN="/usr/bin/rm"
[[ -x "$RM_BIN" ]] || {
    printf 'ERROR: cannot locate the system rm executable\n' >&2
    exit 69
}
HOST_SYSTEM="$("$UNAME_BIN" -s)"
HOST_MACHINE="$("$UNAME_BIN" -m)"
HOMEBREW_PREFIX=""
case "$HOST_SYSTEM:$HOST_MACHINE" in
    Darwin:arm64)
        HOMEBREW_PREFIX="/opt/homebrew"
        TRUSTED_PATH="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"
        ;;
    Darwin:*)
        HOMEBREW_PREFIX="/usr/local"
        TRUSTED_PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
        ;;
    Linux:*)
        TRUSTED_PATH="/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin"
        ;;
    *)
        printf 'ERROR: unsupported RootFS candidate host: %s %s\n' \
            "$HOST_SYSTEM" "$HOST_MACHINE" >&2
        exit 69
        ;;
esac
BASH_BIN="$(PATH="$TRUSTED_PATH" command -v bash)"
[[ "$BASH_BIN" == /* && -f "$BASH_BIN" && -x "$BASH_BIN" ]] || {
    printf 'ERROR: cannot locate bash through the trusted tool path\n' >&2
    exit 69
}
CANDIDATE_TMP_PARENT="$(CDPATH= cd "${TMPDIR:-/tmp}" && pwd -P)"
SANITIZED_HOME=""
sanitized_environment_is_valid() {
    local raw_home="${ISHEMBED_ROOTFS_CANDIDATE_CLEAN_HOME:-}"
    local home_name home_parent name
    local -a home_entries

    [[ "${ISHEMBED_ROOTFS_CANDIDATE_SANITIZED:-0}" == "1" ]] || return 1
    [[ "${PATH:-}" == "$TRUSTED_PATH" &&
       "${HOME:-}" == "$raw_home" &&
       "${TMPDIR:-}" == "$CANDIDATE_TMP_PARENT" &&
       "${LC_ALL:-}" == "C" &&
       "${LANG:-}" == "C" &&
       "${TZ:-}" == "UTC" &&
       "${COPYFILE_DISABLE:-}" == "1" &&
       "${GIT_CONFIG_NOSYSTEM:-}" == "1" &&
       "${PYTHONDONTWRITEBYTECODE:-}" == "1" &&
       "${PYTHONHASHSEED:-}" == "0" &&
       "${PYTHONNOUSERSITE:-}" == "1" &&
       "${SHLVL:-}" == "1" ]] || return 1

    home_name="${raw_home##*/}"
    home_parent="${raw_home%/*}"
    [[ "$home_name" =~ ^ishembed-rootfs-candidate-home\.[A-Za-z0-9]{6}$ &&
       "$home_parent" == "$CANDIDATE_TMP_PARENT" &&
       "$raw_home" == "$CANDIDATE_TMP_PARENT/$home_name" &&
       -d "$raw_home" && ! -L "$raw_home" ]] || return 1
    SANITIZED_HOME="$(CDPATH= cd "$raw_home" && pwd -P)" || return 1
    [[ "$SANITIZED_HOME" == "$CANDIDATE_TMP_PARENT/$home_name" ]] || return 1

    # The wrapper creates an empty HOME. Reject a caller-supplied lookalike
    # containing Git, Python, shell, or tool configuration.
    shopt -s dotglob nullglob
    home_entries=("$SANITIZED_HOME"/*)
    shopt -u dotglob nullglob
    (( ${#home_entries[@]} == 0 )) || return 1

    # A forged sentinel is harmless only when the complete exported
    # environment already matches the wrapper's deliberately small contract.
    while IFS= read -r name; do
        case "$name" in
            ALL_PROXY|COPYFILE_DISABLE|CURL_CA_BUNDLE|GIT_CONFIG_NOSYSTEM|HOME|\
            HTTPS_PROXY|HTTP_PROXY|\
            ISHEMBED_ROOTFS_CANDIDATE_CLEAN_HOME|ISHEMBED_ROOTFS_CANDIDATE_SANITIZED|\
            LANG|LC_ALL|NO_PROXY|PATH|PWD|PYTHONDONTWRITEBYTECODE|PYTHONHASHSEED|\
            PYTHONNOUSERSITE|SHLVL|SSL_CERT_DIR|SSL_CERT_FILE|TMPDIR|TZ|\
            all_proxy|http_proxy|https_proxy|no_proxy)
                ;;
            *)
                SANITIZED_HOME=""
                return 1
                ;;
        esac
    done < <(compgen -e)
    return 0
}

if ! sanitized_environment_is_valid; then
    CLEAN_HOME="$(
        "$MKTEMP_BIN" -d \
            "$CANDIDATE_TMP_PARENT/ishembed-rootfs-candidate-home.XXXXXX"
    )"
    "$CHMOD_BIN" 0700 "$CLEAN_HOME"

    # The wrapper process that actually created CLEAN_HOME owns deletion. The
    # sanitized child deliberately never removes HOME: a caller can reproduce
    # the exported environment contract, but cannot make this parent process
    # adopt and delete a lookalike directory it did not create.
    cleanup_wrapper_home() {
        local rc=$?
        trap - EXIT
        "$RM_BIN" -rf -- "$CLEAN_HOME"
        exit "$rc"
    }
    trap cleanup_wrapper_home EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP

    if /usr/bin/env -i \
        HOME="$CLEAN_HOME" \
        PATH="$TRUSTED_PATH" \
        TMPDIR="$CANDIDATE_TMP_PARENT" \
        LC_ALL=C \
        LANG=C \
        TZ=UTC \
        COPYFILE_DISABLE=1 \
        GIT_CONFIG_NOSYSTEM=1 \
        PYTHONDONTWRITEBYTECODE=1 \
        PYTHONHASHSEED=0 \
        PYTHONNOUSERSITE=1 \
        HTTP_PROXY="${HTTP_PROXY:-}" \
        HTTPS_PROXY="${HTTPS_PROXY:-}" \
        ALL_PROXY="${ALL_PROXY:-}" \
        NO_PROXY="${NO_PROXY:-}" \
        http_proxy="${http_proxy:-}" \
        https_proxy="${https_proxy:-}" \
        all_proxy="${all_proxy:-}" \
        no_proxy="${no_proxy:-}" \
        CURL_CA_BUNDLE="${CURL_CA_BUNDLE:-}" \
        SSL_CERT_FILE="${SSL_CERT_FILE:-}" \
        SSL_CERT_DIR="${SSL_CERT_DIR:-}" \
        ISHEMBED_ROOTFS_CANDIDATE_SANITIZED=1 \
        ISHEMBED_ROOTFS_CANDIDATE_CLEAN_HOME="$CLEAN_HOME" \
        "$BASH_BIN" "$0" "$@"; then
        exit 0
    else
        exit $?
    fi
fi

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
ROOTFS_BUILDER="$PKG_ROOT/scripts/build-rootfs.sh"
ROOTFS_PIN="$PKG_ROOT/scripts/alpine-rootfs-pin.sh"
BUILD_ENVIRONMENT_CAPTURE="$PKG_ROOT/scripts/capture-rootfs-build-environment.py"
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
# override silently change the reviewed pin, source tree, toolchain, archiver,
# or locks. The build-environment receipt records the trusted-PATH-selected
# tools below, so compiler-selection variables must not select different ones.
unset \
    ALPINE_VERSION \
    ALPINE_ARCH \
    ALPINE_SHA256 \
    AR \
    CC \
    CFLAGS \
    CPPFLAGS \
    CXX \
    CXXFLAGS \
    CC_AR \
    CC_LD \
    MAKEFLAGS \
    MFLAGS \
    MAKEOVERRIDES \
    GNUMAKEFLAGS \
    LDFLAGS \
    ZIG \
    TARGET_TRIPLE \
    ARCHFLAGS \
    CPATH \
    C_INCLUDE_PATH \
    CPLUS_INCLUDE_PATH \
    LIBRARY_PATH \
    OBJC_INCLUDE_PATH \
    SDKROOT \
    MACOSX_DEPLOYMENT_TARGET \
    DEVELOPER_DIR \
    TOOLCHAINS \
    PKG_CONFIG_PATH \
    PKG_CONFIG_LIBDIR \
    PKG_CONFIG \
    PKG_CONFIG_EXECUTABLE \
    PKG_CONFIG_SYSROOT_DIR \
    PKG_CONFIG_TOP_BUILD_DIR \
    PKG_CONFIG_DISABLE_UNINSTALLED \
    NINJA \
    SAMU \
    TAR_OPTIONS \
    LD_AUDIT \
    LD_LIBRARY_PATH \
    LD_PRELOAD \
    DYLD_FALLBACK_FRAMEWORK_PATH \
    DYLD_FALLBACK_LIBRARY_PATH \
    DYLD_FRAMEWORK_PATH \
    DYLD_IMAGE_SUFFIX \
    DYLD_INSERT_LIBRARIES \
    DYLD_LIBRARY_PATH \
    DYLD_ROOT_PATH \
    DYLD_SHARED_CACHE_DIR \
    DYLD_VERSIONED_FRAMEWORK_PATH \
    DYLD_VERSIONED_LIBRARY_PATH \
    SOURCE_DATE_EPOCH \
    ROOTFS_ARCHIVER \
    ISH_SRC \
    FAKEFSIFY_BIN \
    FAKEFSIFY_PROVENANCE_SHA256 \
    FAKEFSIFY_EXPECTED_BINARY_SHA256 \
    ROOTFS_INPUTS_FILE \
    BUILD_DIR \
    ROOTFS_REUSE_VALID \
    ROOTFS_INHERITED_LOCK_TOKEN \
    ROOTFS_INHERITED_LOCK_PID

REQUIRED_TOOLS=(
    bash cc curl git make meson ninja pkg-config python3 shasum tar zig
)
case "$HOST_SYSTEM" in
    Darwin) REQUIRED_TOOLS+=(otool) ;;
    Linux) REQUIRED_TOOLS+=(ldd) ;;
    *)
        printf 'ERROR: unsupported RootFS candidate host\n' >&2
        exit 69
        ;;
esac
for tool in "${REQUIRED_TOOLS[@]}"; do
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
CAPTURE_SCRIPT_SHA256="$(
    shasum -a 256 "$BUILD_ENVIRONMENT_CAPTURE" | awk '{print $1}'
)"
FAKEFSIFY_PROVENANCE_SHA256="$(
    "$ROOTFS_BUILDER" --print-identity |
        awk -F= '$1 == "FAKEFSIFY_INPUT_SHA256" { print $2 }'
)"
[[ "$FAKEFSIFY_PROVENANCE_SHA256" =~ ^[0-9a-f]{64}$ ]] || {
    printf 'ERROR: stable fakefsify source provenance is unavailable\n' >&2
    exit 70
}

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
if [[ "$HOST_SYSTEM" == "Darwin" ]]; then
    LIBARCHIVE_PKGCONFIG="$HOMEBREW_PREFIX/opt/libarchive/lib/pkgconfig"
    [[ -d "$LIBARCHIVE_PKGCONFIG" && ! -L "$LIBARCHIVE_PKGCONFIG" ]] || {
        printf 'ERROR: active Homebrew libarchive is unavailable: %s\n' \
            "$LIBARCHIVE_PKGCONFIG" >&2
        exit 69
    }
    PKG_CONFIG_PATH="$LIBARCHIVE_PKGCONFIG"
    export PKG_CONFIG_PATH
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
BUILD_ENVIRONMENT="$WORK_ROOT/ROOTFS_BUILD_ENVIRONMENT.json"
"$BUILD_ENVIRONMENT_CAPTURE" \
    --output "$BUILD_ENVIRONMENT" \
    --repository-revision "$HEAD_REVISION" \
    --ish-revision "$ISH_REVISION" \
    --rootfs-pin-sha256 "$PIN_SHA256" \
    --candidate-script-sha256 "$CANDIDATE_SCRIPT_SHA256" \
    --capture-script-sha256 "$CAPTURE_SCRIPT_SHA256" \
    --fakefsify "$FAKEFSIFY" \
    --fakefsify-provenance-sha256 "$FAKEFSIFY_PROVENANCE_SHA256"
BUILD_ENVIRONMENT_SHA256="$(
    shasum -a 256 "$BUILD_ENVIRONMENT" | awk '{print $1}'
)"

printf '==> Building first RootFS candidate\n'
FAKEFSIFY_BIN="$FAKEFSIFY" \
FAKEFSIFY_PROVENANCE_SHA256="$FAKEFSIFY_PROVENANCE_SHA256" \
FAKEFSIFY_EXPECTED_BINARY_SHA256="$FAKEFSIFY_SHA256" \
ROOTFS_INPUTS_FILE="$ROOTFS_PIN" \
BUILD_DIR="$WORK_ROOT/first" \
ROOTFS_REQUIRE_ABSENT=1 \
    "$ROOTFS_BUILDER"

mkdir -p "$WORK_ROOT/second/dl"
cp "$WORK_ROOT/first/dl/"alpine-minirootfs-*.tar.gz "$WORK_ROOT/second/dl/"

printf '==> Building independent second RootFS candidate\n'
FAKEFSIFY_BIN="$FAKEFSIFY" \
FAKEFSIFY_PROVENANCE_SHA256="$FAKEFSIFY_PROVENANCE_SHA256" \
FAKEFSIFY_EXPECTED_BINARY_SHA256="$FAKEFSIFY_SHA256" \
ROOTFS_INPUTS_FILE="$ROOTFS_PIN" \
BUILD_DIR="$WORK_ROOT/second" \
ROOTFS_REQUIRE_ABSENT=1 \
    "$ROOTFS_BUILDER"

for generation in first second; do
    FAKEFSIFY_BIN="$FAKEFSIFY" \
    FAKEFSIFY_PROVENANCE_SHA256="$FAKEFSIFY_PROVENANCE_SHA256" \
    FAKEFSIFY_EXPECTED_BINARY_SHA256="$FAKEFSIFY_SHA256" \
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
cp "$BUILD_ENVIRONMENT" "$CANDIDATE_STAGE/ROOTFS_BUILD_ENVIRONMENT.json"

python3 - \
    "$CANDIDATE_STAGE/ROOTFS_CANDIDATE.json" \
    "$HEAD_REVISION" \
    "$ISH_REVISION" \
    "$PIN_SHA256" \
    "$CANDIDATE_SCRIPT_SHA256" \
    "$CAPTURE_SCRIPT_SHA256" \
    "$FAKEFSIFY_PROVENANCE_SHA256" \
    "$FAKEFSIFY_SHA256" \
    "$BUILD_ENVIRONMENT_SHA256" \
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
    capture_script_sha256,
    fakefsify_provenance_sha256,
    fakefsify_sha256,
    build_environment_sha256,
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
        "stableHostToolProvenanceSHA256": fakefsify_provenance_sha256,
        "crossInvocationComparison": "not-performed",
    },
    "source": {
        "repository": "https://github.com/jacklv-coder/ish-arm64-pkg",
        "revision": revision,
        "ishRevision": ish_revision,
        "rootfsPinSHA256": pin_sha256,
        "candidateScriptSHA256": candidate_script_sha256,
        "captureScriptSHA256": capture_script_sha256,
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
        "ROOTFS_BUILD_ENVIRONMENT.json": {
            "sha256": build_environment_sha256,
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
