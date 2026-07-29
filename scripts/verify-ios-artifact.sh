#!/usr/bin/env bash

# Verify the exact XCFramework bytes that CI or the isolated release worktree
# is about to consume. Keeping this in one script prevents release.sh from
# validating a weaker artifact than pull-request CI.

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PKG_ROOT/build}"
XCF="${XCF:-$BUILD_DIR/xcframework/libIshKernel.xcframework}"

usage() {
    printf 'usage: BUILD_DIR=<build-root> XCF=<xcframework> %s\n' "$0"
}

if (( $# != 0 )); then
    usage >&2
    exit 64
fi

for command_name in python3 shasum grep cmp ar lipo nm otool xcrun vtool mktemp awk rm; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'verify-ios-artifact: required command not found: %s\n' \
            "$command_name" >&2
        exit 69
    }
done

[[ -d "$BUILD_DIR" && ! -L "$BUILD_DIR" ]] || {
    printf 'verify-ios-artifact: unsafe build directory: %s\n' "$BUILD_DIR" >&2
    exit 65
}
BUILD_DIR="$(cd "$BUILD_DIR" && pwd -P)"
[[ -d "$XCF" && ! -L "$XCF" && -f "$XCF/Info.plist" ]] || {
    printf 'verify-ios-artifact: XCFramework is missing or unsafe: %s\n' "$XCF" >&2
    exit 65
}
XCF="$(cd "$XCF" && pwd -P)"

VERIFY_TMP="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-ios-artifact.XXXXXX")"
cleanup() {
    local rc=$?
    trap - EXIT
    rm -rf -- "$VERIFY_TMP"
    exit "$rc"
}
trap cleanup EXIT
# Convert an asynchronous interruption into an explicit failure; the EXIT trap
# still owns cleanup and preserves this non-zero status for release.sh/CI.
trap 'exit 130' INT TERM HUP

SUPERVISOR_BIN="$BUILD_DIR/supervisor-bundle/ishsv"
SUPERVISOR_BLOB="$BUILD_DIR/generated/ishsv_blob.c"
[[ -f "$SUPERVISOR_BIN" && ! -L "$SUPERVISOR_BIN" ]] || {
    printf 'verify-ios-artifact: bundled supervisor is missing or unsafe\n' >&2
    exit 65
}
[[ -f "$SUPERVISOR_BLOB" && ! -L "$SUPERVISOR_BLOB" ]] || {
    printf 'verify-ios-artifact: generated supervisor blob is missing or unsafe\n' >&2
    exit 65
}

python3 "$PKG_ROOT/scripts/verify-supervisor-elf.py" "$SUPERVISOR_BIN"
python3 - "$SUPERVISOR_BIN" "$SUPERVISOR_BLOB" <<'PY'
import hashlib
import pathlib
import re
import sys

binary_path = pathlib.Path(sys.argv[1])
source_path = pathlib.Path(sys.argv[2])
binary = binary_path.read_bytes()
source = source_path.read_text(encoding="utf-8")
digest = hashlib.sha256(binary).hexdigest()
guest_path = f"/sbin/.ishsv-ishembed-sha256-{digest}"

array_pattern = re.compile(
    r"ISH_BLOB_HIDDEN\s+const\s+uint8_t\s+"
    r"ish_embed_bundled_supervisor\[\]\s*=\s*\{(?P<body>.*?)\};",
    re.DOTALL,
)
arrays = list(array_pattern.finditer(source))
if len(arrays) != 1:
    raise SystemExit("generated supervisor source must contain exactly one blob array")
body = arrays[0].group("body")
tokens = re.findall(r"0x[0-9a-fA-F]{2}", body)
residue = re.sub(r"0x[0-9a-fA-F]{2}", "", body)
if re.fullmatch(r"[\s,]*", residue) is None:
    raise SystemExit("generated supervisor array contains unexpected tokens")
generated = bytes(int(token, 16) for token in tokens)
if generated != binary:
    raise SystemExit("generated supervisor array does not match the verified ELF bytes")

expected_lines = (
    "ISH_BLOB_HIDDEN const size_t ish_embed_bundled_supervisor_len = "
    "sizeof(ish_embed_bundled_supervisor);",
    f'ISH_BLOB_HIDDEN const char ish_embed_bundled_supervisor_sha256[] = "{digest}";',
    f'ISH_BLOB_HIDDEN const char ish_embed_bundled_supervisor_guest_path[] = "{guest_path}";',
)
for expected in expected_lines:
    if source.count(expected) != 1:
        raise SystemExit(f"generated supervisor metadata mismatch: {expected}")
PY
supervisor_sha="$(shasum -a 256 "$SUPERVISOR_BIN" | awk '{print $1}')"

python3 - "$XCF" > "$VERIFY_TMP/xcframework-paths.tsv" <<'PY'
import plistlib
import os
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1])

def require_safe_path(relative_parts, expected_kind):
    current = root
    for index, part in enumerate(relative_parts):
        current = current / part
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            raise SystemExit(f"XCFramework path is missing: {current}") from None
        if stat.S_ISLNK(metadata.st_mode):
            raise SystemExit(f"XCFramework path must not contain symlinks: {current}")
        final = index == len(relative_parts) - 1
        if not final and not stat.S_ISDIR(metadata.st_mode):
            raise SystemExit(f"XCFramework parent is not a directory: {current}")
        if final:
            if expected_kind == "file" and not stat.S_ISREG(metadata.st_mode):
                raise SystemExit(f"XCFramework entry is not a regular file: {current}")
            if expected_kind == "directory" and not stat.S_ISDIR(metadata.st_mode):
                raise SystemExit(f"XCFramework entry is not a directory: {current}")
    resolved = current.resolve(strict=True)
    if root != resolved and root not in resolved.parents:
        raise SystemExit(f"XCFramework entry escapes its root: {current}")

require_safe_path(("Info.plist",), "file")
with (root / "Info.plist").open("rb") as stream:
    payload = plistlib.load(stream)
libraries = payload.get("AvailableLibraries")
if not isinstance(libraries, list) or len(libraries) != 2:
    raise SystemExit("XCFramework must contain exactly two libraries")
expected = {
    "ios-arm64": ("ios", None),
    "ios-arm64-simulator": ("ios", "simulator"),
}
seen = {}
for library in libraries:
    if not isinstance(library, dict):
        raise SystemExit("XCFramework library record is not a dictionary")
    identifier = library.get("LibraryIdentifier")
    if identifier not in expected or identifier in seen:
        raise SystemExit("unexpected or duplicate XCFramework identifier")
    seen[identifier] = library
    if library.get("SupportedArchitectures") != ["arm64"]:
        raise SystemExit(f"{identifier} is not arm64-only")
    platform, variant = expected[identifier]
    if library.get("SupportedPlatform") != platform:
        raise SystemExit(f"{identifier} has the wrong platform")
    if library.get("SupportedPlatformVariant") != variant:
        raise SystemExit(f"{identifier} has the wrong platform variant")
    for key in ("LibraryPath", "BinaryPath"):
        if library.get(key) != "libIshKernel.a":
            raise SystemExit(f"{identifier} has an unexpected {key}")
    if library.get("HeadersPath") != "Headers":
        raise SystemExit(f"{identifier} has an unexpected HeadersPath")
    require_safe_path((identifier,), "directory")
    require_safe_path((identifier, library["LibraryPath"]), "file")
    require_safe_path((identifier, library["HeadersPath"]), "directory")
if set(seen) != set(expected):
    raise SystemExit("XCFramework is missing a required slice")
for identifier in ("ios-arm64", "ios-arm64-simulator"):
    library = seen[identifier]
    print(identifier, library["LibraryPath"], library["HeadersPath"], sep="\t")
PY

exec 7< "$VERIFY_TMP/xcframework-paths.tsv"
IFS=$'\t' read -r device_identifier device_library_path device_headers_path <&7
IFS=$'\t' read -r simulator_identifier simulator_library_path simulator_headers_path <&7
if IFS= read -r unexpected_record <&7; then
    printf 'verify-ios-artifact: unexpected XCFramework path record: %s\n' \
        "$unexpected_record" >&2
    exit 65
fi
exec 7<&-
[[ "$device_identifier" == ios-arm64 && \
   "$simulator_identifier" == ios-arm64-simulator && \
   "$device_library_path" == libIshKernel.a && \
   "$simulator_library_path" == libIshKernel.a && \
   "$device_headers_path" == Headers && \
   "$simulator_headers_path" == Headers ]] || {
    printf 'verify-ios-artifact: internal XCFramework path parsing mismatch\n' >&2
    exit 65
}

license_dir="$XCF/Licenses"
for required_file in \
    "$license_dir/LICENSE" \
    "$license_dir/NOTICE.md" \
    "$license_dir/docs/releasing.md" \
    "$license_dir/docs/releasing.en.md" \
    "$license_dir/third_party/ish/LICENSE.md" \
    "$license_dir/third_party/ish/LICENSE.IOS" \
    "$license_dir/third_party/licenses/musl-COPYRIGHT"; do
    [[ -f "$required_file" && ! -L "$required_file" ]] || {
        printf 'verify-ios-artifact: license file is missing or unsafe: %s\n' \
            "$required_file" >&2
        exit 65
    }
done
cmp "$PKG_ROOT/LICENSE" "$license_dir/LICENSE"
cmp "$PKG_ROOT/NOTICE.md" "$license_dir/NOTICE.md"
cmp "$PKG_ROOT/docs/releasing.md" "$license_dir/docs/releasing.md"
cmp "$PKG_ROOT/docs/releasing.en.md" "$license_dir/docs/releasing.en.md"
cmp "$PKG_ROOT/third_party/ish/LICENSE.md" \
    "$license_dir/third_party/ish/LICENSE.md"
cmp "$PKG_ROOT/third_party/ish/LICENSE.IOS" \
    "$license_dir/third_party/ish/LICENSE.IOS"
cmp "$PKG_ROOT/third_party/licenses/musl-COPYRIGHT" \
    "$license_dir/third_party/licenses/musl-COPYRIGHT"

device_lib="$XCF/$device_identifier/$device_library_path"
simulator_lib="$XCF/$simulator_identifier/$simulator_library_path"
for library in "$device_lib" "$simulator_lib"; do
    [[ -f "$library" && ! -L "$library" ]] || {
        printf 'verify-ios-artifact: library is missing or unsafe: %s\n' \
            "$library" >&2
        exit 65
    }
done
test "$(lipo -archs "$device_lib")" = arm64
test "$(lipo -archs "$simulator_lib")" = arm64

verify_archive_build_versions() {
    local library="$1"
    local expected_platform="$2"
    local output="$3"
    otool -l "$library" > "$output"
    ar -t "$library" > "$output.members"
    python3 - "$output" "$output.members" "$library" "$expected_platform" <<'PY'
import pathlib
import re
import sys

output_path = pathlib.Path(sys.argv[1])
members_path = pathlib.Path(sys.argv[2])
archive = sys.argv[3]
expected_platform = sys.argv[4]
lines = output_path.read_text(encoding="utf-8", errors="strict").splitlines()
if not lines or lines[0] != f"Archive : {archive}":
    raise SystemExit(f"unexpected otool archive header for {archive}")

prefix = archive + "("
headers = [
    index for index, line in enumerate(lines)
    if line.startswith(prefix) and line.endswith("):")
]
if not headers:
    raise SystemExit(f"archive contains no Mach-O members: {archive}")
otool_members = [lines[index][len(prefix):-2] for index in headers]
archive_members = [
    member for member in members_path.read_text(encoding="utf-8").splitlines()
    if member not in {"__.SYMDEF", "__.SYMDEF SORTED", "/", "//", "/SYM64/"}
]
if archive_members != otool_members:
    raise SystemExit(
        f"otool did not parse every non-index archive member in order: {archive}"
    )

def parse_version(raw):
    match = re.fullmatch(r"([0-9]+)(?:[.]([0-9]+))?(?:[.]([0-9]+))?", raw)
    if match is None:
        raise SystemExit(f"invalid Mach-O minimum version in {archive}: {raw}")
    return tuple(int(part or 0) for part in match.groups())

for position, start in enumerate(headers):
    end = headers[position + 1] if position + 1 < len(headers) else len(lines)
    member = otool_members[position]
    block = lines[start + 1:end]
    build_commands = [
        index for index, line in enumerate(block)
        if line.strip() == "cmd LC_BUILD_VERSION"
    ]
    if len(build_commands) != 1:
        raise SystemExit(
            f"{archive}({member}) must contain exactly one LC_BUILD_VERSION"
        )
    command_start = build_commands[0]
    command_end = len(block)
    for index in range(command_start + 1, len(block)):
        if block[index].startswith("Load command "):
            command_end = index
            break
    fields = {}
    for line in block[command_start + 1:command_end]:
        parts = line.split()
        if len(parts) == 2 and parts[0] in {"platform", "minos"}:
            fields[parts[0]] = parts[1]
    if fields.get("platform") != expected_platform:
        raise SystemExit(
            f"{archive}({member}) has platform {fields.get('platform')}, "
            f"expected {expected_platform}"
        )
    minimum = parse_version(fields.get("minos", ""))
    if minimum != (18, 0, 0):
        raise SystemExit(
            f"{archive}({member}) has minimum iOS {fields['minos']}, expected 18.0"
        )
PY
}

# Mach-O LC_BUILD_VERSION platform constants: 2 = iOS, 7 = iOS Simulator.
verify_archive_build_versions \
    "$device_lib" 2 "$VERIFY_TMP/device-archive-load-commands.txt"
verify_archive_build_versions \
    "$simulator_lib" 7 "$VERIFY_TMP/simulator-archive-load-commands.txt"

for library in "$device_lib" "$simulator_lib"; do
    symbols="$(nm -gU "$library" | awk '{print $NF}')"
    symbol_details="$(nm -m "$library")"
    for symbol in \
        _ish_embed_session_retain \
        _ish_embed_session_release \
        _ish_embed_setup_vm_root \
        _ish_embed_rename_noreplace \
        _ish_embed_session_write_timeout \
        _ish_embed_session_close_stdin_timeout \
        _ish_embed_shutdown \
        _ish_embed_bundled_supervisor \
        _ish_embed_bundled_supervisor_len \
        _ish_embed_bundled_supervisor_guest_path \
        _ish_embed_bundled_supervisor_sha256 \
        _ish_ffi_task_join \
        _task_start_joinable \
        _ish_embed_soft_halt_enabled; do
        grep -qx "$symbol" <<< "$symbols"
    done
    ! grep -qx _ish_ffi_setup_vm_root <<< "$symbols"
    grep -Eq '\(__TEXT,__text\) private external _ish_embed_sha256_matches_hex$' \
        <<< "$symbol_details"
    grep -Eq '\(__TEXT,__text\) private external _ish_embed_supervisor_metadata_valid$' \
        <<< "$symbol_details"
    ! grep -Eq '\(__TEXT,__text\) external _ish_embed_sha256_matches_hex$' \
        <<< "$symbol_details"
    ! grep -Eq '\(__TEXT,__text\) external _ish_embed_supervisor_metadata_valid$' \
        <<< "$symbol_details"
done

device_sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
device_clang="$(xcrun --sdk iphoneos --find clang)"
"$device_clang" \
    -target arm64-apple-ios18.0 \
    -isysroot "$device_sdk" \
    -Wl,-fatal_warnings \
    -I"$PKG_ROOT/include" -I"$PKG_ROOT/protocol" \
    "$PKG_ROOT/c-tests/smoke.c" "$device_lib" -lsqlite3 \
    -o "$VERIFY_TMP/ishembed-device-link"

"$device_clang" \
    -target arm64-apple-ios18.0 \
    -isysroot "$device_sdk" \
    -Wl,-fatal_warnings \
    -I"$PKG_ROOT/include" -I"$PKG_ROOT/protocol" \
    -I"$PKG_ROOT/Sources/CIshEmbed/include" \
    "$PKG_ROOT/c-tests/swift_bridge_smoke.c" \
    "$PKG_ROOT/Sources/CIshEmbed/CIshEmbed.c" \
    "$device_lib" -lsqlite3 \
    -o "$VERIFY_TMP/ishembed-device-bridge-link"

simulator_sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
simulator_clang="$(xcrun --sdk iphonesimulator --find clang)"
"$simulator_clang" \
    -target arm64-apple-ios18.0-simulator \
    -isysroot "$simulator_sdk" \
    -Wl,-fatal_warnings \
    -I"$PKG_ROOT/include" -I"$PKG_ROOT/protocol" \
    "$PKG_ROOT/c-tests/smoke.c" "$simulator_lib" -lsqlite3 \
    -o "$VERIFY_TMP/ishembed-simulator-link"

"$simulator_clang" \
    -target arm64-apple-ios18.0-simulator \
    -isysroot "$simulator_sdk" \
    -Wl,-fatal_warnings \
    -I"$PKG_ROOT/include" -I"$PKG_ROOT/protocol" \
    -I"$PKG_ROOT/Sources/CIshEmbed/include" \
    "$PKG_ROOT/c-tests/swift_bridge_smoke.c" \
    "$PKG_ROOT/Sources/CIshEmbed/CIshEmbed.c" \
    "$simulator_lib" -lsqlite3 \
    -o "$VERIFY_TMP/ishembed-simulator-bridge-link"

for linked_binary in \
    "$VERIFY_TMP/ishembed-device-link" \
    "$VERIFY_TMP/ishembed-simulator-link" \
    "$VERIFY_TMP/ishembed-device-bridge-link" \
    "$VERIFY_TMP/ishembed-simulator-bridge-link"; do
    linked_exports="$(nm -gU "$linked_binary" | awk '{print $NF}')"
    ! grep -qx _ish_embed_sha256_matches_hex <<< "$linked_exports"
    ! grep -qx _ish_embed_supervisor_metadata_valid <<< "$linked_exports"
done

for bridge_binary in \
    "$VERIFY_TMP/ishembed-device-bridge-link" \
    "$VERIFY_TMP/ishembed-simulator-bridge-link"; do
    bridge_symbols="$(nm -m "$bridge_binary")"
    grep -Eq '\(__TEXT,__text\) external _ish_embed_rename_noreplace$' \
        <<< "$bridge_symbols"
    ! grep -Eq '\(__TEXT,__text\) weak external _ish_embed_rename_noreplace$' \
        <<< "$bridge_symbols"
    grep -Eq '\(__TEXT,__text\) external _ish_embed_session_write_timeout$' \
        <<< "$bridge_symbols"
    grep -Eq '\(__TEXT,__text\) external _ish_embed_session_close_stdin_timeout$' \
        <<< "$bridge_symbols"
    ! grep -Eq '\(__TEXT,__text\) weak external _ish_embed_session_write_timeout$' \
        <<< "$bridge_symbols"
    ! grep -Eq '\(__TEXT,__text\) weak external _ish_embed_session_close_stdin_timeout$' \
        <<< "$bridge_symbols"
done

verify_final_link_build_version() {
    local binary="$1"
    local expected_platform="$2"
    local output="$3"
    vtool -show-build "$binary" > "$output"
    python3 - "$output" "$binary" "$expected_platform" <<'PY'
import pathlib
import sys

output = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
binary = sys.argv[2]
expected_platform = sys.argv[3]
if sum(line.strip() == "cmd LC_BUILD_VERSION" for line in output) != 1:
    raise SystemExit(f"{binary} must contain exactly one LC_BUILD_VERSION")
platforms = [line.split()[1] for line in output if line.strip().startswith("platform ")]
minimums = [line.split()[1] for line in output if line.strip().startswith("minos ")]
if platforms != [expected_platform]:
    raise SystemExit(f"{binary} has unexpected platform metadata: {platforms}")
if minimums != ["18.0"]:
    raise SystemExit(f"{binary} has unexpected minimum OS metadata: {minimums}")
PY
}

verify_linked_supervisor_blob() {
    local binary="$1"
    python3 - "$binary" "$SUPERVISOR_BIN" "$supervisor_sha" <<'PY'
import pathlib
import struct
import sys

binary_path = pathlib.Path(sys.argv[1])
supervisor_path = pathlib.Path(sys.argv[2])
expected_sha = sys.argv[3]
data = binary_path.read_bytes()
supervisor = supervisor_path.read_bytes()
expected_path = f"/sbin/.ishsv-ishembed-sha256-{expected_sha}"

def require_range(offset, size, label):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise SystemExit(f"invalid {label} range in {binary_path}")

if len(data) < 32:
    raise SystemExit(f"linked image is too short: {binary_path}")
header = struct.unpack_from("<IiiIIIII", data, 0)
magic, cpu_type, _cpu_subtype, file_type, command_count, command_bytes, _flags, _reserved = header
if magic != 0xFEEDFACF or cpu_type != 0x0100000C or file_type != 2:
    raise SystemExit(f"linked image is not an arm64 Mach-O executable: {binary_path}")
require_range(32, command_bytes, "load-command")

sections = []
symtab = None
offset = 32
for _ in range(command_count):
    require_range(offset, 8, "load-command header")
    command, command_size = struct.unpack_from("<II", data, offset)
    if command_size < 8:
        raise SystemExit(f"invalid load-command size in {binary_path}")
    require_range(offset, command_size, "load-command")
    if command == 0x19:  # LC_SEGMENT_64
        if command_size < 72:
            raise SystemExit(f"short LC_SEGMENT_64 in {binary_path}")
        segment = struct.unpack_from("<II16sQQQQiiII", data, offset)
        section_count = segment[9]
        if 72 + section_count * 80 > command_size:
            raise SystemExit(f"truncated section table in {binary_path}")
        section_offset = offset + 72
        for _section_index in range(section_count):
            section = struct.unpack_from("<16s16sQQIIIIIIII", data, section_offset)
            sections.append((section[2], section[3], section[4]))
            section_offset += 80
    elif command == 0x2:  # LC_SYMTAB
        if command_size < 24 or symtab is not None:
            raise SystemExit(f"invalid LC_SYMTAB in {binary_path}")
        symtab = struct.unpack_from("<IIIIII", data, offset)[2:]
    offset += command_size
if offset != 32 + command_bytes or symtab is None:
    raise SystemExit(f"invalid Mach-O load commands in {binary_path}")

symbol_offset, symbol_count, string_offset, string_size = symtab
require_range(symbol_offset, symbol_count * 16, "symbol-table")
require_range(string_offset, string_size, "string-table")
wanted = {
    b"_ish_embed_bundled_supervisor",
    b"_ish_embed_bundled_supervisor_len",
    b"_ish_embed_bundled_supervisor_sha256",
    b"_ish_embed_bundled_supervisor_guest_path",
}
symbols = {}
for index in range(symbol_count):
    entry = symbol_offset + index * 16
    string_index, symbol_type, section_index, _description, value = struct.unpack_from(
        "<IBBHQ", data, entry
    )
    if string_index >= string_size:
        raise SystemExit(f"invalid symbol string offset in {binary_path}")
    name_start = string_offset + string_index
    name_end = data.find(b"\0", name_start, string_offset + string_size)
    if name_end < 0:
        raise SystemExit(f"unterminated symbol name in {binary_path}")
    name = data[name_start:name_end]
    if name not in wanted:
        continue
    # Ignore matching DWARF/STABS records. The runtime object is the concrete
    # N_SECT symbol; debug entries have N_STAB set and no backing section.
    if symbol_type & 0xE0 or symbol_type & 0x0E != 0x0E:
        continue
    if name in symbols or section_index == 0 or section_index > len(sections):
        raise SystemExit(f"invalid or duplicate supervisor symbol in {binary_path}: {name!r}")
    symbols[name] = (section_index, value)
if set(symbols) != wanted:
    missing = sorted(name.decode("ascii") for name in wanted - set(symbols))
    raise SystemExit(f"linked image is missing supervisor symbols: {missing}")

def symbol_bytes(name, size):
    section_index, value = symbols[name]
    address, section_size, file_offset = sections[section_index - 1]
    relative = value - address
    if relative < 0 or relative + size > section_size:
        raise SystemExit(f"supervisor symbol escapes its section in {binary_path}: {name!r}")
    require_range(file_offset + relative, size, "supervisor-symbol")
    return data[file_offset + relative:file_offset + relative + size]

if symbol_bytes(b"_ish_embed_bundled_supervisor", len(supervisor)) != supervisor:
    raise SystemExit(f"linked supervisor blob differs from verified ELF: {binary_path}")
encoded_length = symbol_bytes(b"_ish_embed_bundled_supervisor_len", 8)
if int.from_bytes(encoded_length, "little") != len(supervisor):
    raise SystemExit(f"linked supervisor length mismatch: {binary_path}")
if symbol_bytes(b"_ish_embed_bundled_supervisor_sha256", 65) != expected_sha.encode() + b"\0":
    raise SystemExit(f"linked supervisor SHA-256 metadata mismatch: {binary_path}")
expected_path_bytes = expected_path.encode() + b"\0"
if symbol_bytes(b"_ish_embed_bundled_supervisor_guest_path", len(expected_path_bytes)) != expected_path_bytes:
    raise SystemExit(f"linked supervisor guest path mismatch: {binary_path}")
PY
}

verify_final_link_build_version \
    "$VERIFY_TMP/ishembed-device-link" IOS "$VERIFY_TMP/device-link-build.txt"
verify_final_link_build_version \
    "$VERIFY_TMP/ishembed-simulator-link" IOSSIMULATOR \
    "$VERIFY_TMP/simulator-link-build.txt"
verify_linked_supervisor_blob "$VERIFY_TMP/ishembed-device-link"
verify_linked_supervisor_blob "$VERIFY_TMP/ishembed-simulator-link"

printf 'iOS artifact verification passed: %s\n' "$XCF"
