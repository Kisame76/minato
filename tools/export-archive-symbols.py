#!/usr/bin/env python3
"""Copies a static archive of ELF64 objects and gives its hidden global symbols default visibility.

Android links libc.so with "libunwind-exported", LLVM's libunwind built without hidden visibility,
so that libc.so can export _Unwind_Backtrace and friends (version LIBC_R). The NDK ships libunwind.a
with hidden symbols only. This makes the same code exportable; which symbols libc.so really exports
is still up to its version script (ADR-0021).

Usage: tools/export-archive-symbols.py <llvm-ar> <input.a> <output.a>
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SHT_SYMTAB = 2
STB_GLOBAL = 1
STV_DEFAULT = 0
STV_HIDDEN = 2


def export_hidden_globals(path):
    """Patches the symbol table of one ELF64 little-endian object in place; returns the count."""
    with open(path, "r+b") as handle:
        data = bytearray(handle.read())
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise SystemExit(f"{path}: not an ELF64 little-endian object")
        section_offset, = struct.unpack_from("<Q", data, 0x28)
        section_size, section_count = struct.unpack_from("<HH", data, 0x3a)
        changed = 0
        for index in range(section_count):
            header = section_offset + index * section_size
            section_type, = struct.unpack_from("<I", data, header + 4)
            if section_type != SHT_SYMTAB:
                continue
            offset, size, _, _, _, entry_size = struct.unpack_from("<QQIIQQ", data, header + 24)
            for symbol in range(offset, offset + size, entry_size):
                info, other = data[symbol + 4], data[symbol + 5]
                if info >> 4 == STB_GLOBAL and other & 3 == STV_HIDDEN and struct.unpack_from("<H", data, symbol + 6)[0] != 0:
                    data[symbol + 5] = (other & ~3) | STV_DEFAULT
                    changed += 1
        handle.seek(0)
        handle.write(data)
    return changed


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    archiver, source, target = sys.argv[1:]
    work = tempfile.mkdtemp()
    try:
        members = subprocess.run([archiver, "t", os.path.abspath(source)], check=True, capture_output=True,
                                 text=True).stdout.split()
        if len(members) != len(set(members)):
            raise SystemExit(f"{source}: members with the same name are not supported")
        subprocess.run([archiver, "x", os.path.abspath(source)], check=True, cwd=work)
        changed = sum(export_hidden_globals(os.path.join(work, member)) for member in members)
        output = os.path.join(work, "out.a")
        subprocess.run([archiver, "rcs", output] + members, check=True, cwd=work)
        shutil.move(output, target)
        print(f"{os.path.basename(target)}: {changed} symbols made visible in {len(members)} objects")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
