#!/usr/bin/env python3
"""Create a byte-stable gzip-compressed tar archive from one directory tree."""

from __future__ import annotations

import argparse
import gzip
import os
import pathlib
import stat
import tarfile
import tempfile


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--arcname", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--mtime", required=True, type=int)
    return parser.parse_args()


def normalized_member(info: tarfile.TarInfo, epoch: int) -> tarfile.TarInfo:
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = epoch
    info.pax_headers = {}
    return info


def main() -> None:
    arguments = parse_arguments()
    source = pathlib.Path(arguments.source)
    output = pathlib.Path(arguments.output)

    if arguments.mtime < 0 or arguments.mtime > 0xFFFFFFFF:
        raise SystemExit("--mtime must fit the unsigned 32-bit gzip timestamp")
    if arguments.arcname not in {".", "fs"}:
        raise SystemExit("--arcname must be either '.' or 'fs'")

    source_info = source.lstat()
    if stat.S_ISLNK(source_info.st_mode) or not stat.S_ISDIR(source_info.st_mode):
        raise SystemExit("--source must be a real directory")
    if output.exists() or output.is_symlink():
        raise SystemExit("--output must not already exist")

    output_parent = output.parent.resolve(strict=True)
    temporary_fd, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.",
        suffix=".tmp",
        dir=output_parent,
    )
    os.close(temporary_fd)
    temporary = pathlib.Path(temporary_name)

    try:
        with temporary.open("wb") as raw_stream:
            with gzip.GzipFile(
                filename="",
                mode="wb",
                compresslevel=9,
                fileobj=raw_stream,
                mtime=arguments.mtime,
            ) as gzip_stream:
                with tarfile.open(
                    fileobj=gzip_stream,
                    mode="w",
                    format=tarfile.PAX_FORMAT,
                    dereference=False,
                ) as archive:
                    archive.add(
                        source,
                        arcname=arguments.arcname,
                        recursive=True,
                        filter=lambda info: normalized_member(
                            info, arguments.mtime
                        ),
                    )
            raw_stream.flush()
            os.fsync(raw_stream.fileno())
        os.chmod(temporary, 0o644)
        os.link(temporary, output)
        temporary.unlink()
    finally:
        if temporary.exists() or temporary.is_symlink():
            temporary.unlink()


if __name__ == "__main__":
    main()
