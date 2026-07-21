#!/usr/bin/env python3
"""Reject binary/container inputs that do not belong in Corresponding Source."""

from __future__ import annotations

import argparse
import os
import pathlib
import posixpath
import stat
import sys
import tarfile


MAX_SOURCE_FILE_BYTES = 16 * 1024 * 1024
SIGNATURE_WINDOW_BYTES = 64 * 1024
TRAILER_WINDOW_BYTES = 512

ROOTFS_DIRECTORY_NAMES = {
    "minirootfs",
    "root-fs",
    "root_fs",
    "rootfs",
}

ROOTFS_MARKER_NAMES = {
    "meta.db",
}

# Corresponding Source is assembled from expanded source trees.  Nested package,
# filesystem, and disk-image containers are unnecessary and make it possible to
# smuggle a renamed RootFS through the release's otherwise fixed asset list.
FORBIDDEN_CONTAINER_SUFFIXES = (
    ".7z",
    ".apk",
    ".cpio",
    ".cpio.gz",
    ".cpio.xz",
    ".cpio.zst",
    ".deb",
    ".dmg",
    ".ext2",
    ".ext3",
    ".ext4",
    ".img",
    ".ipa",
    ".iso",
    ".qcow",
    ".qcow2",
    ".rar",
    ".raw",
    ".rpm",
    ".sqfs",
    ".squashfs",
    ".tar",
    ".tar.bz2",
    ".tar.gz",
    ".tar.xz",
    ".tar.zst",
    ".tbz2",
    ".tgz",
    ".txz",
    ".tzst",
    ".vhd",
    ".vhdx",
    ".vmdk",
    ".zip",
)

ROOTFS_DOCUMENT_SUFFIXES = (
    ".c",
    ".h",
    ".ini",
    ".json",
    ".md",
    ".mk",
    ".plist",
    ".py",
    ".sh",
    ".swift",
    ".toml",
    ".txt",
    ".yaml",
    ".yml",
)

# Extension checks are useful for reviewability, but are not an ownership or
# provenance boundary: a container can be renamed to an innocuous suffix.  The
# signatures below therefore reject common archive, package, filesystem, disk,
# and RootFS database formats by content as well.
FORBIDDEN_MAGIC_PREFIXES = (
    (b"\x1f\x8b\x08", "gzip stream"),
    (b"BZh", "bzip2 stream"),
    (b"\xfd7zXZ\x00", "xz stream"),
    (b"\x28\xb5\x2f\xfd", "zstd stream"),
    (b"\x04\x22\x4d\x18", "LZ4 stream"),
    (b"PK\x03\x04", "ZIP archive"),
    (b"PK\x05\x06", "empty ZIP archive"),
    (b"PK\x07\x08", "spanned ZIP archive"),
    (b"7z\xbc\xaf\x27\x1c", "7-Zip archive"),
    (b"Rar!\x1a\x07", "RAR archive"),
    (b"070701", "new ASCII cpio archive"),
    (b"070702", "CRC cpio archive"),
    (b"070707", "old ASCII cpio archive"),
    (b"\x71\xc7", "binary cpio archive (big-endian magic)"),
    (b"\xc7\x71", "binary cpio archive (little-endian magic)"),
    (b"!<arch>\n", "ar package/archive"),
    (b"xar!", "XAR package/archive"),
    (b"\xed\xab\xee\xdb", "RPM package"),
    (b"hsqs", "SquashFS image"),
    (b"sqsh", "SquashFS image"),
    (b"qshs", "SquashFS image"),
    (b"shsq", "SquashFS image"),
    (b"QFI\xfb", "QCOW disk image"),
    (b"KDMV", "VMDK disk image"),
    (b"MSWIM\x00\x00\x00", "WIM disk image"),
    (b"ANDROID!", "Android boot image"),
    (b"\x3a\xff\x26\xed", "Android sparse image"),
    (b"SQLite format 3\x00", "SQLite/RootFS metadata database"),
)


class PolicyError(ValueError):
    pass


def normalize_relative(raw: str) -> pathlib.PurePosixPath:
    path = pathlib.PurePosixPath(raw)
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise PolicyError(f"unsafe source path: {raw}")
    return path


def check_path(path: pathlib.PurePosixPath, *, is_file: bool, size: int = 0) -> None:
    lowered_parts = tuple(part.lower() for part in path.parts)
    if any(part in ROOTFS_DIRECTORY_NAMES for part in lowered_parts[:-1]):
        raise PolicyError(f"RootFS directory is forbidden in Corresponding Source: {path}")

    if not is_file:
        if lowered_parts[-1] in ROOTFS_DIRECTORY_NAMES:
            raise PolicyError(f"RootFS directory is forbidden in Corresponding Source: {path}")
        return

    basename = lowered_parts[-1]
    if basename in ROOTFS_MARKER_NAMES:
        raise PolicyError(f"RootFS marker is forbidden in Corresponding Source: {path}")
    if basename.endswith(FORBIDDEN_CONTAINER_SUFFIXES):
        raise PolicyError(f"nested archive/package/filesystem image is forbidden: {path}")
    if ("rootfs" in basename or "minirootfs" in basename) and not basename.endswith(
        ROOTFS_DOCUMENT_SUFFIXES
    ):
        raise PolicyError(f"RootFS-like binary asset name is forbidden: {path}")
    if size > MAX_SOURCE_FILE_BYTES:
        raise PolicyError(
            f"source file exceeds {MAX_SOURCE_FILE_BYTES} bytes: {path} ({size} bytes)"
        )


def check_symlink(path: pathlib.PurePosixPath, target: str) -> None:
    if not target or target.startswith("/"):
        raise PolicyError(f"absolute/empty source symlink is forbidden: {path} -> {target}")
    resolved = posixpath.normpath(posixpath.join(path.parent.as_posix(), target))
    if resolved == ".." or resolved.startswith("../"):
        raise PolicyError(f"source symlink escapes the archive root: {path} -> {target}")


def forbidden_content_kind(head: bytes, tail: bytes, size: int) -> str | None:
    for magic, kind in FORBIDDEN_MAGIC_PREFIXES:
        if head.startswith(magic):
            return kind
    if len(head) >= 262 and head[257:262] == b"ustar":
        return "tar archive"
    if len(head) >= 32774 and head[32769:32774] == b"CD001":
        return "ISO filesystem image"
    if len(head) >= 1082 and size >= 65536:
        ext_log_block_size = int.from_bytes(head[1048:1052], "little")
        if head[1080:1082] == b"\x53\xef" and ext_log_block_size <= 6:
            return "ext filesystem image"
    if len(head) >= 36 and head[32:36] == b"NXSB":
        return "APFS container image"
    if len(head) >= 1026 and head[1024:1026] in {b"H+", b"HX"}:
        return "HFS filesystem image"
    if tail.startswith(b"koly"):
        return "DMG disk image"
    if tail.startswith(b"conectix"):
        return "VHD disk image"
    return None


def check_content(
    path: pathlib.PurePosixPath, *, size: int, head: bytes, tail: bytes
) -> None:
    kind = forbidden_content_kind(head, tail, size)
    if kind is not None:
        raise PolicyError(f"nested {kind} is forbidden by content: {path}")


def read_signature_windows(source, size: int) -> tuple[bytes, bytes]:
    head = source.read(min(size, SIGNATURE_WINDOW_BYTES))
    if size <= TRAILER_WINDOW_BYTES:
        return head, head
    source.seek(size - TRAILER_WINDOW_BYTES)
    return head, source.read(TRAILER_WINDOW_BYTES)


def validate_tree(root: pathlib.Path) -> None:
    if not root.is_dir():
        raise PolicyError(f"source-policy root is not a directory: {root}")
    for candidate in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = normalize_relative(candidate.relative_to(root).as_posix())
        metadata = os.lstat(candidate)
        if stat.S_ISDIR(metadata.st_mode):
            check_path(relative, is_file=False)
        elif stat.S_ISLNK(metadata.st_mode):
            check_path(relative, is_file=False)
            check_symlink(relative, os.readlink(candidate))
        elif stat.S_ISREG(metadata.st_mode):
            check_path(relative, is_file=True, size=metadata.st_size)
            with candidate.open("rb") as source:
                head, tail = read_signature_windows(source, metadata.st_size)
            check_content(relative, size=metadata.st_size, head=head, tail=tail)
        else:
            raise PolicyError(f"unsupported source file type: {relative}")


def validate_archive(archive_path: pathlib.Path, prefix: str) -> None:
    expected_prefix = normalize_relative(prefix).as_posix()
    with tarfile.open(archive_path, "r:gz") as archive:
        for member in archive.getmembers():
            name = member.name.rstrip("/")
            if name == expected_prefix:
                continue
            if not name.startswith(expected_prefix + "/"):
                raise PolicyError(f"archive member escapes the release prefix: {name}")
            relative = normalize_relative(name[len(expected_prefix) + 1 :])
            if member.isdir():
                check_path(relative, is_file=False)
            elif member.issym():
                check_path(relative, is_file=False)
                check_symlink(relative, member.linkname)
            elif member.isfile():
                check_path(relative, is_file=True, size=member.size)
                extracted = archive.extractfile(member)
                if extracted is None:
                    raise PolicyError(f"could not inspect archive member: {name}")
                with extracted:
                    head, tail = read_signature_windows(extracted, member.size)
                check_content(relative, size=member.size, head=head, tail=tail)
            else:
                raise PolicyError(f"unsupported archive member type: {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--tree", type=pathlib.Path)
    mode.add_argument("--archive", type=pathlib.Path)
    parser.add_argument("--prefix")
    args = parser.parse_args()

    try:
        if args.tree is not None:
            if args.prefix is not None:
                parser.error("--prefix is valid only with --archive")
            validate_tree(args.tree)
        else:
            if not args.prefix:
                parser.error("--archive requires --prefix")
            validate_archive(args.archive, args.prefix)
    except (OSError, tarfile.TarError, PolicyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
