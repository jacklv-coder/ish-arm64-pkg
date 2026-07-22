#!/usr/bin/env bash
# Build iSH + embed glue as a single static archive packed into an
# xcframework that the Swift Package consumes.
#
# Slices: arm64 device + arm64 simulator (+ x86_64 simulator if requested).
#
# Default output: build/xcframework/libIshKernel.xcframework
# Override BUILD_DIR and OUT_DIR with explicit private directories for release
# transactions or other concurrent builds.
#
# Notes:
#  - We deliberately scrub CC, SDKROOT, RUSTC_WRAPPER, etc. from inside
#    each meson invocation: those env vars wreck Meson's host compiler
#    probes when --cross-file expects iOS but the host is macOS.
#  - We don't `env -i` because Meson needs PATH, HOME, PYTHON*. Instead
#    we use `unset` for the toxic ones.

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISH_SRC="$PKG_ROOT/third_party/ish"
EMBED_DIR="$PKG_ROOT"

# Release builds provide private output roots so another build cannot replace
# an artifact between compilation, verification, and packaging.  Keep the
# familiar repository-local defaults for maintainer development builds.
BUILD_DIR="${BUILD_DIR:-$PKG_ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/xcframework}"

# OUT_DIR becomes part of a Make target while building the supervisor. GNU/BSD
# Make cannot represent arbitrary whitespace or metacharacters there reliably,
# even if every shell recipe quotes the value. Keep both roots to a deliberately
# conservative alphabet before creating or deleting anything. This also blocks
# shell and Make syntax such as $, ;, #, %, :, quotes, globbing, and controls.
validate_build_path() {
    local name="$1"
    local value="$2"
    [[ -n "$value" && "$value" =~ [^/] ]] || {
        printf 'unsafe %s: %s\n' "$name" "$value" >&2
        exit 64
    }
    [[ "$value" != -* ]] || {
        printf 'unsafe %s: relative path must not begin with -: %s\n' \
            "$name" "$value" >&2
        exit 64
    }
    [[ "$value" =~ ^[-A-Za-z0-9._/+]+$ ]] || {
        printf 'unsafe %s: path contains whitespace or Make/shell metacharacters: %s\n' \
            "$name" "$value" >&2
        exit 64
    }
}

validate_build_path BUILD_DIR "$BUILD_DIR"
validate_build_path OUT_DIR "$OUT_DIR"
mkdir -p -- "$BUILD_DIR" "$OUT_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd -P)"
OUT_DIR="$(cd "$OUT_DIR" && pwd -P)"
validate_build_path BUILD_DIR "$BUILD_DIR"
validate_build_path OUT_DIR "$OUT_DIR"
[[ "$BUILD_DIR" != / && "$BUILD_DIR" != "$PKG_ROOT" ]] || {
    printf 'unsafe BUILD_DIR: %s\n' "$BUILD_DIR" >&2
    exit 64
}
[[ "$OUT_DIR" != / && "$OUT_DIR" != "$PKG_ROOT" ]] || {
    printf 'unsafe OUT_DIR: %s\n' "$OUT_DIR" >&2
    exit 64
}
SUPERVISOR_BUILD_DIR="$BUILD_DIR/supervisor-bundle"
GENERATED_DIR="$BUILD_DIR/generated"
SUPERVISOR_BIN="$SUPERVISOR_BUILD_DIR/ishsv"
SUPERVISOR_BLOB_C="$GENERATED_DIR/ishsv_blob.c"

mkdir -p -- "$GENERATED_DIR"

echo "==> Building bundled guest supervisor (aarch64-linux-musl)"
command -v zig >/dev/null
command -v python3 >/dev/null
echo "    Zig $(zig version)"
make -C "$PKG_ROOT/supervisor" OUT_DIR="$SUPERVISOR_BUILD_DIR" clean
make -C "$PKG_ROOT/supervisor" \
    OUT_DIR="$SUPERVISOR_BUILD_DIR" \
    TARGET_TRIPLE=aarch64-linux-musl
test -x "$SUPERVISOR_BIN"
python3 "$PKG_ROOT/scripts/verify-supervisor-elf.py" "$SUPERVISOR_BIN"
python3 "$PKG_ROOT/scripts/generate-supervisor-blob.py" \
    --input "$SUPERVISOR_BIN" \
    --output "$SUPERVISOR_BLOB_C"
grep -q '^ISH_BLOB_HIDDEN const uint8_t ish_embed_bundled_supervisor\[\]' "$SUPERVISOR_BLOB_C"
grep -q '^ISH_BLOB_HIDDEN const size_t ish_embed_bundled_supervisor_len' "$SUPERVISOR_BLOB_C"

scrub_env() {
    unset CC CXX CFLAGS CXXFLAGS LDFLAGS CPPFLAGS SDKROOT MACOSX_DEPLOYMENT_TARGET
    unset RUSTC RUSTC_WRAPPER RUSTFLAGS CARGO_TARGET_DIR
}

build_slice() {
    local slice_name="$1"
    local sdk="$2"
    local arch="$3"
    local min_ver="$4"
    local outdir="$BUILD_DIR/ios-$slice_name"

    echo "==> Building slice: $slice_name (sdk=$sdk arch=$arch)"

    SDKROOT_R="$(xcrun --sdk "$sdk" --show-sdk-path)"
    CC_R="$(xcrun --sdk "$sdk" --find clang)"
    AR_R="$(xcrun --sdk "$sdk" --find ar)"
    STRIP_R="$(xcrun --sdk "$sdk" --find strip)"

    local cpu_family
    case "$arch" in
        arm64)  cpu_family=aarch64 ;;
        x86_64) cpu_family=x86_64 ;;
    esac

    # Pick the right clang target triple. For simulator slices we *must*
    # use -target arm64-apple-ios-simulator (or the x86_64 equivalent),
    # otherwise clang produces device-targeted code and the linker rejects
    # libsqlite3.tbd whose target is `arm64-ios-simulator`.
    local clang_target
    case "$sdk" in
        iphoneos)        clang_target="${arch}-apple-ios${min_ver}" ;;
        iphonesimulator) clang_target="${arch}-apple-ios${min_ver}-simulator" ;;
        *)               clang_target="${arch}-apple-ios${min_ver}" ;;
    esac

    rm -rf "$outdir"
    mkdir -p "$outdir/ish" "$outdir/embed"

    cat > "$outdir/cross.ini" <<EOF
[binaries]
c = '$CC_R'
ar = '$AR_R'
strip = '$STRIP_R'
[host_machine]
system = 'darwin'
cpu_family = '$cpu_family'
cpu = '$arch'
endian = 'little'
[built-in options]
c_args = ['-target', '$clang_target', '-isysroot', '$SDKROOT_R', '-fno-stack-protector', '-DISH_DISABLE_SKIP_BRK=1']
c_link_args = ['-target', '$clang_target', '-isysroot', '$SDKROOT_R']
default_library = 'static'
[properties]
needs_exe_wrapper = true
EOF

    # The pinned iSH fork contains an opt-out arm64 BRK recovery experiment.
    # Production runtime slices must keep fatal guest SIGTRAP semantics; a
    # trapped assertion must not forge x0/PC state and continue execution.

    (
        scrub_env
        meson setup "$outdir/ish" "$ISH_SRC" \
            --cross-file="$outdir/cross.ini" \
            -Dengine=asbestos -Dkernel=ish -Dguest_arch=arm64 \
            -Dlog_handler=stderr \
            -Dwerror=false -Dwarning_level=0
        ninja -C "$outdir/ish" libish.a libish_emu.a libfakefs.a
    )

    (
        scrub_env
        meson setup "$outdir/embed" "$EMBED_DIR" \
            --cross-file="$outdir/cross.ini" \
            -Dish_src="$ISH_SRC" -Dguest_arch=arm64 \
            -Dsupervisor_blob_src="$SUPERVISOR_BLOB_C" \
            -Dwarning_level=0
        ninja -C "$outdir/embed"
    )

    libtool -static -o "$outdir/libIshKernel.a" \
        "$outdir/ish/libish.a" \
        "$outdir/ish/libish_emu.a" \
        "$outdir/ish/libfakefs.a" \
        "$outdir/embed/libishffi.a" \
        "$outdir/embed/libishembed.a"
    echo "    => $outdir/libIshKernel.a"
}

build_slice "device"        iphoneos          arm64  18.0
build_slice "simulator"     iphonesimulator   arm64  18.0
# build_slice "simulator-x86" iphonesimulator x86_64 18.0  # opt-in

rm -rf "$OUT_DIR/libIshKernel.xcframework"
xcodebuild -create-xcframework \
    -library "$BUILD_DIR/ios-device/libIshKernel.a"    -headers "$EMBED_DIR/include" \
    -library "$BUILD_DIR/ios-simulator/libIshKernel.a" -headers "$EMBED_DIR/include" \
    -output  "$OUT_DIR/libIshKernel.xcframework"

LICENSE_DIR="$OUT_DIR/libIshKernel.xcframework/Licenses"
mkdir -p \
    "$LICENSE_DIR/docs" \
    "$LICENSE_DIR/third_party/ish" \
    "$LICENSE_DIR/third_party/licenses"

# Preserve repository-relative paths below Licenses/.  NOTICE.md and the
# release guides therefore keep working both in the source tree and in the
# distributed XCFramework instead of pointing at renamed, missing files.
cp "$PKG_ROOT/LICENSE" "$LICENSE_DIR/LICENSE"
cp "$PKG_ROOT/NOTICE.md" "$LICENSE_DIR/NOTICE.md"
cp "$PKG_ROOT/docs/releasing.md" "$LICENSE_DIR/docs/releasing.md"
cp "$PKG_ROOT/docs/releasing.en.md" "$LICENSE_DIR/docs/releasing.en.md"
cp "$ISH_SRC/LICENSE.md" "$LICENSE_DIR/third_party/ish/LICENSE.md"
cp "$ISH_SRC/LICENSE.IOS" "$LICENSE_DIR/third_party/ish/LICENSE.IOS"
cp "$PKG_ROOT/third_party/licenses/musl-COPYRIGHT" \
    "$LICENSE_DIR/third_party/licenses/musl-COPYRIGHT"
for notice in \
    LICENSE \
    NOTICE.md \
    docs/releasing.md \
    docs/releasing.en.md \
    third_party/ish/LICENSE.md \
    third_party/ish/LICENSE.IOS \
    third_party/licenses/musl-COPYRIGHT; do
    test -s "$LICENSE_DIR/$notice"
done

echo
echo "Done: $OUT_DIR/libIshKernel.xcframework"
