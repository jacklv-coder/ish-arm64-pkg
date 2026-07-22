#!/usr/bin/env bash
# Create the Corresponding Source archive shipped beside the XCFramework.
#
# The archive is assembled from immutable Git objects rather than the working
# tree. It expands the pinned third_party/ish gitlink and adds the musl source
# snapshot used by Zig when statically linking the bundled guest supervisor.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/package-source.sh --version vX.Y.Z --output FILE [options]

Options:
  --commit REV       Parent repository revision to archive (default: HEAD)
  --musl-source DIR  Exact musl source tree used by Zig (default: auto-detect)
  --expected-zig-version VERSION
                     Require this exact `zig version` value
  --expected-zig-sha256 SHA256
                     Require this exact Zig executable SHA-256
  --expected-musl-sha256 SHA256
                     Require this exact normalized musl source-tree hash
  --print-musl-sha256 DIR
                     Print DIR's normalized source-tree hash and exit
  --version VERSION  Release version recorded in the archive
  --output FILE      Destination .tar.gz; must not already exist
  -h, --help         Show this help

The result contains the parent repository, the exact third_party/ish gitlink
contents, and the musl source used for the statically linked guest supervisor.
Git metadata, build outputs, XCFramework binaries, and RootFS assets are not
included.
EOF
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

source_tree_sha256() {
    local source_root="$1"
    python3 - "$source_root" <<'PY'
import hashlib
import os
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1])
if not root.is_dir():
    raise SystemExit(f"source tree is not a directory: {root}")

paths = [root]

def collect(directory: pathlib.Path) -> None:
    entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
    for entry in entries:
        path = pathlib.Path(entry.path)
        paths.append(path)
        if entry.is_dir(follow_symlinks=False):
            collect(path)

collect(root)
digest = hashlib.sha256()

def add_field(value: bytes) -> None:
    digest.update(len(value).to_bytes(8, "big"))
    digest.update(value)

for path in paths:
    relative = "." if path == root else path.relative_to(root).as_posix()
    metadata = os.lstat(path)
    add_field(relative.encode("utf-8", "surrogateescape"))
    if stat.S_ISDIR(metadata.st_mode):
        add_field(b"directory")
    elif stat.S_ISLNK(metadata.st_mode):
        add_field(b"symlink")
        add_field(os.readlink(path).encode("utf-8", "surrogateescape"))
    elif stat.S_ISREG(metadata.st_mode):
        add_field(b"executable" if metadata.st_mode & stat.S_IXUSR else b"file")
        add_field(metadata.st_size.to_bytes(8, "big"))
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
    else:
        raise SystemExit(f"unsupported source file type: {path}")

print(digest.hexdigest())
PY
}

COMMIT="HEAD"
MUSL_SOURCE=""
EXPECTED_ZIG_VERSION=""
EXPECTED_ZIG_SHA256=""
EXPECTED_MUSL_SHA256=""
PRINT_MUSL_SHA256=""
VERSION=""
OUTPUT=""

while (($# > 0)); do
    case "$1" in
        --commit)
            (($# >= 2)) || fail "--commit requires a revision"
            COMMIT="$2"
            shift 2
            ;;
        --musl-source)
            (($# >= 2)) || fail "--musl-source requires a directory"
            MUSL_SOURCE="$2"
            shift 2
            ;;
        --expected-zig-version)
            (($# >= 2)) || fail "--expected-zig-version requires a value"
            EXPECTED_ZIG_VERSION="$2"
            shift 2
            ;;
        --expected-zig-sha256)
            (($# >= 2)) || fail "--expected-zig-sha256 requires a SHA-256"
            EXPECTED_ZIG_SHA256="$2"
            shift 2
            ;;
        --expected-musl-sha256)
            (($# >= 2)) || fail "--expected-musl-sha256 requires a SHA-256"
            EXPECTED_MUSL_SHA256="$2"
            shift 2
            ;;
        --print-musl-sha256)
            (($# >= 2)) || fail "--print-musl-sha256 requires a directory"
            PRINT_MUSL_SHA256="$2"
            shift 2
            ;;
        --version)
            (($# >= 2)) || fail "--version requires a value"
            VERSION="$2"
            shift 2
            ;;
        --output)
            (($# >= 2)) || fail "--output requires a path"
            OUTPUT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            fail "unknown option: $1"
            ;;
        *)
            fail "unexpected argument: $1"
            ;;
    esac
done

if [[ -n "$PRINT_MUSL_SHA256" ]]; then
    [[ "$COMMIT" == HEAD && -z "$MUSL_SOURCE" && -z "$EXPECTED_ZIG_VERSION" && \
       -z "$EXPECTED_ZIG_SHA256" && \
       -z "$EXPECTED_MUSL_SHA256" && -z "$VERSION" && -z "$OUTPUT" ]] \
        || fail "--print-musl-sha256 cannot be combined with packaging options"
    require_command python3
    PRINT_MUSL_SHA256="$(cd "$PRINT_MUSL_SHA256" 2>/dev/null && pwd)" \
        || fail "musl source directory does not exist: $PRINT_MUSL_SHA256"
    source_tree_sha256 "$PRINT_MUSL_SHA256"
    exit 0
fi

[[ "$VERSION" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$ ]] \
    || fail "--version must be strict SemVer with a v prefix"
if [[ "$VERSION" == *-* ]]; then
    PRERELEASE="${VERSION#*-}"
    IFS='.' read -r -a PRERELEASE_IDENTIFIERS <<< "$PRERELEASE"
    for identifier in "${PRERELEASE_IDENTIFIERS[@]}"; do
        [[ ! "$identifier" =~ ^0[0-9]+$ ]] \
            || fail "numeric prerelease identifiers must not contain leading zeroes: $identifier"
    done
fi
[[ -n "$OUTPUT" ]] || { usage >&2; exit 2; }
[[ -z "$EXPECTED_ZIG_SHA256" || "$EXPECTED_ZIG_SHA256" =~ ^[0-9a-f]{64}$ ]] \
    || fail "--expected-zig-sha256 must be 64 lowercase hexadecimal characters"
[[ -z "$EXPECTED_MUSL_SHA256" || "$EXPECTED_MUSL_SHA256" =~ ^[0-9a-f]{64}$ ]] \
    || fail "--expected-musl-sha256 must be 64 lowercase hexadecimal characters"

for command_name in git tar python3 zig shasum mktemp cmp; do
    require_command "$command_name"
done

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISH_REPO="$PKG_ROOT/third_party/ish"
SOURCE_POLICY_VALIDATOR="$PKG_ROOT/scripts/validate-source-policy.py"
[[ -x "$SOURCE_POLICY_VALIDATOR" ]] \
    || fail "Corresponding Source policy validator is missing or not executable"
PARENT_COMMIT="$(git -C "$PKG_ROOT" rev-parse --verify "$COMMIT^{commit}")" \
    || fail "could not resolve parent commit: $COMMIT"
PARENT_EPOCH="$(git -C "$PKG_ROOT" show -s --format=%ct "$PARENT_COMMIT")"

ISH_ENTRY="$(git -C "$PKG_ROOT" ls-tree "$PARENT_COMMIT" -- third_party/ish)"
read -r ISH_MODE ISH_TYPE ISH_COMMIT ISH_PATH <<< "$ISH_ENTRY"
[[ "$ISH_MODE" == 160000 && "$ISH_TYPE" == commit && \
   "$ISH_PATH" == third_party/ish && "$ISH_COMMIT" =~ ^[0-9a-f]{40}$ ]] \
    || fail "third_party/ish is not a pinned gitlink at $PARENT_COMMIT"
[[ -e "$ISH_REPO/.git" ]] \
    || fail "third_party/ish is not initialized; run git submodule update --init third_party/ish"
git -C "$ISH_REPO" cat-file -e "$ISH_COMMIT^{commit}" 2>/dev/null \
    || fail "pinned iSH commit is missing locally: $ISH_COMMIT"

ZIG_EXECUTABLE="$(command -v zig)"
ZIG_EXECUTABLE="$(cd "$(dirname "$ZIG_EXECUTABLE")" && pwd)/$(basename "$ZIG_EXECUTABLE")"
ZIG_SHA256="$(shasum -a 256 "$ZIG_EXECUTABLE" | awk '{print $1}')"
ZIG_VERSION="$(zig version)"
[[ -z "$EXPECTED_ZIG_VERSION" || "$ZIG_VERSION" == "$EXPECTED_ZIG_VERSION" ]] \
    || fail "Zig version changed: expected $EXPECTED_ZIG_VERSION, found $ZIG_VERSION"
[[ -z "$EXPECTED_ZIG_SHA256" || "$ZIG_SHA256" == "$EXPECTED_ZIG_SHA256" ]] \
    || fail "Zig executable changed: expected $EXPECTED_ZIG_SHA256, found $ZIG_SHA256"
if [[ -z "$MUSL_SOURCE" ]]; then
    ZIG_ENV_OUTPUT="$(zig env)"
    ZIG_LIB_DIR="$(printf '%s\n' "$ZIG_ENV_OUTPUT" | python3 -c '
import json
import re
import sys

raw = sys.stdin.read()
try:
    value = json.loads(raw)["lib_dir"]
except (json.JSONDecodeError, KeyError, TypeError):
    match = re.search(r"(?:[.]lib_dir|\"lib_dir\")\s*[:=]\s*\"([^\"]+)\"", raw)
    if match is None:
        raise SystemExit("could not locate lib_dir in zig env output")
    value = match.group(1)
print(value)
')" || fail "could not determine Zig library directory"
    MUSL_SOURCE="$ZIG_LIB_DIR/libc/musl"
fi
MUSL_SOURCE="$(cd "$MUSL_SOURCE" 2>/dev/null && pwd)" \
    || fail "musl source directory does not exist: $MUSL_SOURCE"
[[ -f "$MUSL_SOURCE/COPYRIGHT" && -d "$MUSL_SOURCE/src" && \
   -d "$MUSL_SOURCE/arch/aarch64" ]] \
    || fail "--musl-source is not a complete Zig musl source tree"
MUSL_SOURCE_SHA256="$(source_tree_sha256 "$MUSL_SOURCE")"
[[ -z "$EXPECTED_MUSL_SHA256" || \
   "$MUSL_SOURCE_SHA256" == "$EXPECTED_MUSL_SHA256" ]] \
    || fail "musl source changed: expected $EXPECTED_MUSL_SHA256, found $MUSL_SOURCE_SHA256"

OUTPUT_DIR="$(dirname "$OUTPUT")"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
OUTPUT="$OUTPUT_DIR/$(basename "$OUTPUT")"
[[ "$OUTPUT" == *.tar.gz ]] || fail "--output must end in .tar.gz"
[[ ! -e "$OUTPUT" ]] || fail "output already exists: $OUTPUT"

STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-source.XXXXXX")"
STAGING_DIR="$(cd "$STAGING_DIR" && pwd -P)"
SOURCE_ROOT="$STAGING_DIR/IshEmbed-${VERSION#v}-source"

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    rm -rf "$STAGING_DIR"
    exit "$rc"
}
trap cleanup EXIT INT TERM HUP

mkdir -p "$SOURCE_ROOT"
git -C "$PKG_ROOT" archive --format=tar "$PARENT_COMMIT" \
    | tar -xf - -C "$SOURCE_ROOT"
mkdir -p "$SOURCE_ROOT/third_party/ish"
git -C "$ISH_REPO" archive --format=tar "$ISH_COMMIT" \
    | tar -xf - -C "$SOURCE_ROOT/third_party/ish"

[[ -f "$SOURCE_ROOT/third_party/licenses/musl-COPYRIGHT" ]] \
    || fail "selected parent commit does not contain the tracked musl notice"
cmp -s "$SOURCE_ROOT/third_party/licenses/musl-COPYRIGHT" \
    "$MUSL_SOURCE/COPYRIGHT" \
    || fail "tracked musl notice differs from the selected toolchain; review and update third_party/licenses/musl-COPYRIGHT"

mkdir -p "$SOURCE_ROOT/corresponding-source"
cp -R "$MUSL_SOURCE" "$SOURCE_ROOT/corresponding-source/musl"
PACKAGED_MUSL_SHA256="$(source_tree_sha256 "$SOURCE_ROOT/corresponding-source/musl")"
[[ "$PACKAGED_MUSL_SHA256" == "$MUSL_SOURCE_SHA256" ]] \
    || fail "packaged musl source differs from the build toolchain snapshot"

NESTED_GITLINKS="$(git -C "$ISH_REPO" ls-tree -r "$ISH_COMMIT" | \
    awk '$1 == "160000" { print "- " $4 " @ " $3 " (未被本发布 XCFramework 构建使用 / not used by this release build)" }')"
python3 - "$SOURCE_ROOT/corresponding-source/SOURCE-MANIFEST.txt" \
    "$VERSION" "$PARENT_COMMIT" "$ISH_COMMIT" "$ZIG_VERSION" "$ZIG_SHA256" \
    "$MUSL_SOURCE_SHA256" \
    "$NESTED_GITLINKS" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
version, parent, ish, zig, zig_sha256, musl_sha256, nested = sys.argv[2:]
if not nested:
    nested = "- none"
path.write_text(
    f"""IshEmbed Corresponding Source Manifest / 对应源码清单
Format-Version: 1
Release-Version: {version}
Parent-Commit: {parent}
iSH-Path: third_party/ish
iSH-Commit: {ish}
Guest-Supervisor-Toolchain: Zig {zig}, target aarch64-linux-musl
Guest-Supervisor-Zig-SHA256: {zig_sha256}
musl-Source-Path: corresponding-source/musl
musl-Source-SHA256: {musl_sha256}

Included / 已包含
- Parent repository tracked source and build/release scripts at Parent-Commit.
  Parent-Commit 对应的父仓库受 Git 跟踪源码与构建、发布脚本。
- third_party/ish tracked source expanded from the exact iSH-Commit gitlink.
  从 iSH-Commit gitlink 精确展开的 third_party/ish 受跟踪源码。
- The musl source snapshot exposed by the recorded Zig toolchain and used to
  statically link the bundled guest supervisor.
  由所记录 Zig 工具链提供、用于静态链接内嵌 guest supervisor 的 musl 源码快照。

Excluded / 未包含
- Git metadata and all untracked or ignored build outputs.
  Git 元数据，以及所有未跟踪或已忽略的构建产物。
- libIshKernel.xcframework, generated supervisor binaries, and generated C blobs;
  these are regenerated by scripts/build-ios.sh from the included source.
  XCFramework、生成的 supervisor 二进制和 C blob；它们由已包含源码重新生成。
- Every RootFS image or archive. RootFS has independent provenance, content,
  hashing, export, and licensing gates and is not part of this source artifact.
  所有 RootFS 镜像或归档；RootFS 具有独立的来源、内容、哈希、出口和许可门禁。
- Nested archives/packages, filesystem or disk images, RootFS markers, and any
  individual regular file larger than 16 MiB, enforced before and after packing.
  嵌套归档/软件包、文件系统或磁盘镜像、RootFS 标记及超过 16 MiB 的任意普通文件；
  在打包前和临时归档生成后分别执行门禁。
- General-purpose build tools and Apple platform SDK/system libraries.
  通用构建工具以及 Apple 平台 SDK/系统库。

Pinned nested iSH gitlinks / iSH 内固定但本发布构建未使用的子模块
{nested}

The release build selects -Dkernel=ish and the libish/libish_emu/libfakefs
targets; the nested gitlinks above are not compiled or linked into the released
XCFramework. They remain recorded here for repository traceability.
本发布使用 -Dkernel=ish，并构建 libish、libish_emu、libfakefs；上述嵌套 gitlink
不会编译或链接进 XCFramework，仅为仓库追溯目的记录。

本清单用于记录构建输入与归档边界，不构成法律意见。
This manifest records build inputs and archive boundaries; it is not legal advice.
""",
    encoding="utf-8",
)
PY

# Reject RootFS trees, nested archives/images, and every regular file over the
# size ceiling before spending time compressing them. The completed tarball is checked
# again below before its final name is published.
python3 "$SOURCE_POLICY_VALIDATOR" --tree "$SOURCE_ROOT"

SOURCE_SHA256="$(python3 - "$SOURCE_ROOT" "$OUTPUT" "$PARENT_EPOCH" \
    "$(basename "$SOURCE_ROOT")" "$PARENT_COMMIT" "$ISH_COMMIT" \
    "$ZIG_SHA256" "$MUSL_SOURCE_SHA256" "$SOURCE_POLICY_VALIDATOR" <<'PY'
import gzip
import hashlib
import os
import pathlib
import subprocess
import stat
import sys
import tarfile
import tempfile

root = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
epoch = int(sys.argv[3])
prefix, parent, ish, zig_sha256, musl_sha256, source_policy_validator = sys.argv[4:]

paths = [root]
paths.extend(sorted(root.rglob("*"), key=lambda item: item.relative_to(root.parent).as_posix()))
temp_path = None
try:
    with tempfile.NamedTemporaryFile(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent, delete=False
    ) as raw:
        temp_path = pathlib.Path(raw.name)
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in paths:
                    relative = path.relative_to(root.parent).as_posix()
                    metadata = os.lstat(path)
                    info = tarfile.TarInfo(relative)
                    info.uid = 0
                    info.gid = 0
                    info.uname = "root"
                    info.gname = "root"
                    info.mtime = epoch
                    info.pax_headers = {}
                    if stat.S_ISDIR(metadata.st_mode):
                        info.type = tarfile.DIRTYPE
                        info.mode = 0o755
                        archive.addfile(info)
                    elif stat.S_ISLNK(metadata.st_mode):
                        info.type = tarfile.SYMTYPE
                        info.mode = 0o777
                        info.linkname = os.readlink(path)
                        archive.addfile(info)
                    elif stat.S_ISREG(metadata.st_mode):
                        info.type = tarfile.REGTYPE
                        info.mode = 0o755 if metadata.st_mode & stat.S_IXUSR else 0o644
                        info.size = metadata.st_size
                        with path.open("rb") as source:
                            archive.addfile(info, source)
                    else:
                        raise SystemExit(f"unsupported source file type: {path}")

    required = {
        f"{prefix}/Package.swift",
        f"{prefix}/LICENSE",
        f"{prefix}/NOTICE.md",
        f"{prefix}/scripts/build-ios.sh",
        f"{prefix}/scripts/release.sh",
        f"{prefix}/scripts/release-version-policy.sh",
        f"{prefix}/scripts/verify-ios-artifact.sh",
        f"{prefix}/scripts/validate-source-policy.py",
        f"{prefix}/third_party/ish/LICENSE.md",
        f"{prefix}/third_party/ish/LICENSE.IOS",
        f"{prefix}/third_party/licenses/musl-COPYRIGHT",
        f"{prefix}/corresponding-source/SOURCE-MANIFEST.txt",
        f"{prefix}/corresponding-source/musl/COPYRIGHT",
    }
    with tarfile.open(temp_path, "r:gz") as archive:
        members = archive.getmembers()
        names = {member.name.rstrip("/") for member in members}
        missing = sorted(required - names)
        if missing:
            raise SystemExit("source archive is missing: " + ", ".join(missing))
        for member in members:
            name = member.name.rstrip("/")
            if not (name == prefix or name.startswith(prefix + "/")):
                raise SystemExit(f"archive member escapes the release prefix: {name}")
            relative = name[len(prefix):].lstrip("/")
            components = relative.split("/") if relative else []
            if ".git" in components or "build" in components:
                raise SystemExit(f"forbidden metadata/build path in source archive: {name}")
            if any(component.endswith(".xcframework") for component in components):
                raise SystemExit(f"XCFramework found in source archive: {name}")
            basename = components[-1].lower() if components else ""
            if basename in {"fs.tar.gz", "rootfs.tar.gz", "rootfs.tar", "rootfs.img"}:
                raise SystemExit(f"RootFS asset found in source archive: {name}")
            if basename in {"ishsv", "ishsv_blob.c"} or basename.endswith((".a", ".o")):
                raise SystemExit(f"generated native artifact found in source archive: {name}")
        manifest_member = archive.getmember(
            f"{prefix}/corresponding-source/SOURCE-MANIFEST.txt"
        )
        manifest_file = archive.extractfile(manifest_member)
        if manifest_file is None:
            raise SystemExit("could not read source manifest")
        manifest = manifest_file.read().decode("utf-8")
        expected_manifest = (
            f"Parent-Commit: {parent}",
            f"iSH-Commit: {ish}",
            f"Guest-Supervisor-Zig-SHA256: {zig_sha256}",
            f"musl-Source-SHA256: {musl_sha256}",
        )
        for expected in expected_manifest:
            if expected not in manifest:
                raise SystemExit(f"source manifest does not contain {expected}")

    subprocess.run(
        [
            sys.executable,
            source_policy_validator,
            "--archive",
            str(temp_path),
            "--prefix",
            prefix,
        ],
        check=True,
    )

    digest = hashlib.sha256()
    with temp_path.open("rb") as archive_file:
        while chunk := archive_file.read(1024 * 1024):
            digest.update(chunk)
    try:
        os.link(temp_path, output)
    except FileExistsError:
        raise SystemExit(f"output already exists: {output}")
    print(digest.hexdigest())
finally:
    if temp_path is not None:
        temp_path.unlink(missing_ok=True)
PY
)"
printf 'Corresponding Source archive created.\n'
printf '  Archive: %s\n' "$OUTPUT"
printf '  Parent:  %s\n' "$PARENT_COMMIT"
printf '  iSH:     %s\n' "$ISH_COMMIT"
printf '  Zig:     %s\n' "$ZIG_VERSION"
printf '  Zig SHA: %s\n' "$ZIG_SHA256"
printf '  musl:    %s\n' "$MUSL_SOURCE_SHA256"
printf '  SHA-256: %s\n' "$SOURCE_SHA256"
