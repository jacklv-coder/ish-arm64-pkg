#!/usr/bin/env python3
"""Verify that the bundled PID 1 is a static ARM64 Linux ELF executable."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


ELFCLASS64 = 2
ELFDATA2LSB = 1
ET_EXEC = 2
EM_AARCH64 = 183
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2


def fail(message: str) -> None:
    raise SystemExit(f"invalid supervisor ELF: {message}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    data = args.elf.read_bytes()

    if len(data) < 64 or data[:4] != b"\x7fELF":
        fail("missing ELF header")
    if data[4] != ELFCLASS64:
        fail("expected ELF64")
    if data[5] != ELFDATA2LSB:
        fail("expected little-endian encoding")
    if data[6] != 1:
        fail("unsupported ELF version")

    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
    e_type, e_machine, e_version = header[1], header[2], header[3]
    e_entry, e_phoff = header[4], header[5]
    e_ehsize, e_phentsize, e_phnum = header[8], header[9], header[10]
    if e_type != ET_EXEC:
        fail(f"expected ET_EXEC, got {e_type}")
    if e_machine != EM_AARCH64:
        fail(f"expected EM_AARCH64, got {e_machine}")
    if e_version != 1 or e_ehsize != 64:
        fail("invalid ELF header version or size")
    if e_phentsize != 56 or e_phnum == 0:
        fail("invalid program-header table")
    if e_phoff < e_ehsize or e_phoff + e_phentsize * e_phnum > len(data):
        fail("program-header table exceeds file")

    has_executable_load = False
    entry_is_executable = False
    has_nonexec_stack = False
    for index in range(e_phnum):
        offset = e_phoff + index * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, p_align = struct.unpack_from(
            "<IIQQQQQQ", data, offset
        )
        if p_filesz > p_memsz:
            fail(f"program header {index} has filesz > memsz")
        if p_offset + p_filesz > len(data):
            fail(f"program header {index} exceeds file")
        if p_align not in (0, 1):
            if p_align & (p_align - 1):
                fail(f"program header {index} alignment is not a power of two")
            if p_vaddr % p_align != p_offset % p_align:
                fail(f"program header {index} has incongruent address/offset")
        if p_type == PT_INTERP:
            fail("PT_INTERP is forbidden")
        if p_type == PT_DYNAMIC:
            fail("PT_DYNAMIC/DT_NEEDED is forbidden")
        if p_type == PT_GNU_STACK:
            if p_flags & PF_X:
                fail("executable stack is forbidden")
            has_nonexec_stack = True
        if p_type == PT_LOAD and p_flags & PF_X:
            if p_flags & PF_W:
                fail("writable executable PT_LOAD is forbidden")
            has_executable_load = True
            if p_vaddr <= e_entry < p_vaddr + p_filesz:
                entry_is_executable = True
    if not has_executable_load:
        fail("missing executable PT_LOAD segment")
    if not entry_is_executable:
        fail("entry point is outside executable file-backed PT_LOAD")
    if not has_nonexec_stack:
        fail("missing non-executable PT_GNU_STACK")

    digest = hashlib.sha256(data).hexdigest()
    print(
        "verified ELF64 little-endian AArch64 ET_EXEC, static, "
        f"no PT_INTERP/DT_NEEDED, sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
