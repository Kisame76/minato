#!/usr/bin/env python3
"""Writes the guest's export table (ADR-0055 decision 3), which the import's compatibility check reads: per library of
the guest system, by its soname, the libraries it needs and the names it exports, so that the check can bind an app
library's undefined symbols as linker64 does. The binding rule is the reader's: in the app's isolated namespace
linker64 searches the global group, then breadth first the app's own libraries and the system libraries they need
directly, not those libraries' own dependencies (bionic's find_libraries and android_namespace_t::is_accessible).

  # minato export table 1
  @ libandroid.so
  needs libc.so libdl.so liblog.so libm.so libnativewindow.so
  AAsset_close@@LIBANDROID
  ASensorManager_getInstance@@LIBANDROID!

A block's header has " global" when the library has DF_1_GLOBAL (libsigchain.so): it joins the global group, which an
app namespace inherits. The main executables are in that group too but have no block: activity-host exports nothing,
app_process64 only main, _start and two weak templates. A block's needs line is its DT_NEEDED in order. A name is
written as llvm-nm writes it: name@@V for the default version V, name@V for a hidden one, and the bare name for an
unversioned or base-version symbol; only defined dynamic symbols that linker64 binds, global or weak
(is_symbol_global_and_defined), functions and data alike. A trailing ! marks a generated stub of Minato's NDK libraries
(runtime/android/stubgen.py), which logs once and answers "nothing". Blocks are sorted by soname, names by name and
then version. The same library given twice is one block; two different ones
with one soname are an error: the table is one soname space over /system and the APEXes, as today's copies are the
same files. So is what no line can say, which lld never writes: a hidden base version, a defined name of version
index 0 (local), a name with a space, "@" or "!", and a file that is no AArch64 shared library.
tools/embed-guest-system.sh writes it into the app's guest system as /system/etc/export-table.txt, from the ELF files
it converts; the import records the SHA-256 of the file.

Usage: tools/export-table.py <output> <ELF library> ...
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime", "android"))
import stubgen  # noqa: E402

HEADER = "# minato export table 1"
SHT_DYNAMIC, SHT_DYNSYM = 6, 11
SHT_GNU_VERDEF, SHT_GNU_VERSYM = 0x6FFFFFFD, 0x6FFFFFFF
DT_NULL, DT_NEEDED, DT_SONAME, DT_FLAGS_1, DF_1_GLOBAL = 0, 1, 14, 0x6FFFFFFB, 0x2
ET_DYN, EM_AARCH64 = 3, 183
STB_GLOBAL, STB_WEAK = 1, 2
VERSYM_HIDDEN, VER_NDX_LOCAL, VER_NDX_GLOBAL = 0x8000, 0, 1


class Elf:
    """The parts of a 64-bit little-endian ELF file the table needs, from its section headers."""

    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        self.path = path
        if self.data[:4] != b"\x7fELF" or self.data[4] != 2 or self.data[5] != 1:
            raise SystemExit(f"export-table: {path} is no 64-bit little-endian ELF file")
        if struct.unpack_from("<HH", self.data, 16) != (ET_DYN, EM_AARCH64):
            raise SystemExit(f"export-table: {path} is no AArch64 shared library")
        offset, = struct.unpack_from("<Q", self.data, 0x28)
        size, count = struct.unpack_from("<HH", self.data, 0x3A)
        if offset == 0 or count == 0:
            raise SystemExit(f"export-table: {path} has no section headers")
        self.sections = [struct.unpack_from("<IIQQQQIIQQ", self.data, offset + index * size) for index in range(count)]

    def section(self, kind):
        found = [section for section in self.sections if section[1] == kind]
        return found[0] if found else None

    def string(self, table, offset):
        start = self.sections[table][4] + offset
        return self.data[start:self.data.index(b"\0", start)].decode("utf-8")

    def dynamic(self):
        """DT_NEEDED in order, DT_SONAME, and whether DT_FLAGS_1 has DF_1_GLOBAL."""
        section = self.section(SHT_DYNAMIC)
        needed, soname, is_global = [], None, False
        if section is None:
            return needed, soname, is_global
        for at in range(section[4], section[4] + section[5], 16):
            tag, value = struct.unpack_from("<qQ", self.data, at)
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed.append(self.string(section[6], value))
            elif tag == DT_SONAME:
                soname = self.string(section[6], value)
            elif tag == DT_FLAGS_1:
                is_global = bool(value & DF_1_GLOBAL)
        return needed, soname, is_global

    def versions(self):
        """Version index -> name. The file's base version has index 1, VER_NDX_GLOBAL, which names no version."""
        section = self.section(SHT_GNU_VERDEF)
        versions = {}
        if section is None:
            return versions
        at = section[4]
        for _ in range(section[7]):  # sh_info: the number of definitions
            _, _, index, _, _, aux, following = struct.unpack_from("<HHHHIII", self.data, at)
            name_offset, = struct.unpack_from("<I", self.data, at + aux)
            versions[index] = self.string(section[6], name_offset)
            if following == 0:
                break
            at += following
        return versions

    def exports(self):
        """The defined dynamic symbols that bind, as (name, version or None, hidden)."""
        symbols = self.section(SHT_DYNSYM)
        if symbols is None:
            return []
        versym = self.section(SHT_GNU_VERSYM)
        versions = self.versions()
        exports = []
        for index in range(1, symbols[5] // 24):
            name_offset, info, _, section_index = struct.unpack_from("<IBBH", self.data, symbols[4] + index * 24)
            if section_index == 0 or info >> 4 not in (STB_GLOBAL, STB_WEAK):
                continue
            name = self.string(symbols[6], name_offset)
            version, hidden = None, False
            if versym is not None:
                value, = struct.unpack_from("<H", self.data, versym[4] + index * 2)
                hidden = bool(value & VERSYM_HIDDEN)
                number = value & ~VERSYM_HIDDEN
                if number == VER_NDX_LOCAL:
                    raise SystemExit(f"export-table: {self.path}: {name} is defined with version index 0 (local), "
                                     "which the table cannot say")
                if number == VER_NDX_GLOBAL and hidden:
                    raise SystemExit(f"export-table: {self.path}: {name} has a hidden base version, which the table "
                                     "cannot say")
                if number > VER_NDX_GLOBAL:
                    if number not in versions:
                        raise SystemExit(f"export-table: {self.path} names version {number}, which it does not define")
                    version = versions[number]
            if not name or "@" in name or "!" in name or any(letter.isspace() for letter in name):
                raise SystemExit(f"export-table: {self.path}: a name the table cannot hold: {name!r}")
            exports.append((name, version, hidden))
        return exports


def stubs(soname, exports):
    """The names of `soname` that are stubgen's generated stubs."""
    library = soname[:-len(".so")] if soname.endswith(".so") else soname
    if library not in stubgen.LIBRARIES:
        return set()
    private = set(stubgen.LIBRARIES[library]["private"])
    return set(stubgen.stubbed_names(library, sorted({name for name, _, _ in exports if name not in private})))


def block(path):
    elf = Elf(path)
    needed, soname, is_global = elf.dynamic()
    soname = soname or os.path.basename(path)
    exports = elf.exports()
    stubbed = stubs(soname, exports)
    lines = []
    for name, version, hidden in sorted(exports, key=lambda export: (export[0], export[1] or "", export[2])):
        text = name if version is None else f"{name}@{version}" if hidden else f"{name}@@{version}"
        lines.append(text + ("!" if name in stubbed else ""))
    return soname, [f"@ {soname}" + (" global" if is_global else ""), " ".join(["needs"] + needed)] + lines


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    blocks = {}
    for path in sys.argv[2:]:
        soname, lines = block(path)
        if soname in blocks and blocks[soname][1] != lines:
            raise SystemExit(f"export-table: {blocks[soname][0]} and {path} are different libraries named {soname}")
        blocks.setdefault(soname, (path, lines))
    text = "\n".join([HEADER] + [line for soname in sorted(blocks) for line in blocks[soname][1]]) + "\n"
    output = sys.argv[1]
    with open(output + ".tmp", "w", encoding="utf-8") as handle:
        handle.write(text)
    os.replace(output + ".tmp", output)


if __name__ == "__main__":
    main()
