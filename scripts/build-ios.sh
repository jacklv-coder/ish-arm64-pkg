#!/usr/bin/env bash
# Build iSH + embed glue as a single static archive packed into an
# xcframework that the Swift Package consumes.
#
# Slices: arm64 device + arm64 simulator (+ x86_64 simulator if requested).
#
# Output: build/xcframework/libIshKernel.xcframework
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
BUILD_DIR="$PKG_ROOT/build"
OUT_DIR="$BUILD_DIR/xcframework"

mkdir -p "$BUILD_DIR" "$OUT_DIR"

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
c_args = ['-target', '$clang_target', '-isysroot', '$SDKROOT_R', '-fno-stack-protector']
c_link_args = ['-target', '$clang_target', '-isysroot', '$SDKROOT_R']
default_library = 'static'
[properties]
needs_exe_wrapper = true
EOF

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

build_slice "device"        iphoneos          arm64  14.0
build_slice "simulator"     iphonesimulator   arm64  14.0
# build_slice "simulator-x86" iphonesimulator x86_64 14.0  # opt-in

rm -rf "$OUT_DIR/libIshKernel.xcframework"
xcodebuild -create-xcframework \
    -library "$BUILD_DIR/ios-device/libIshKernel.a"    -headers "$EMBED_DIR/include" \
    -library "$BUILD_DIR/ios-simulator/libIshKernel.a" -headers "$EMBED_DIR/include" \
    -output  "$OUT_DIR/libIshKernel.xcframework"

echo
echo "Done: $OUT_DIR/libIshKernel.xcframework"
