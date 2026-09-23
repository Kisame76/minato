#!/usr/bin/env python3
"""Packs the time zones of the Mac into bionic's tzdata file (ADR-0021, M2 step 5c).

Bionic reads time zones from one file, /system/usr/share/zoneinfo/tzdata or the tzdata APEX
(libc/tzcode/bionic.cpp): a header, an index of zone names and the TZif data of every zone. The Mac
has the same IANA tz database as TZif files in /usr/share/zoneinfo; the data is in the public domain.

  tools/make-tzdata.py <output> [<zoneinfo directory>]

Layout, big-endian as bionic reads it:
  char version[12]      "tzdata" + IANA version, NUL-terminated, e.g. "tzdata2026c"
  int32 index_offset    24, the index follows the header
  int32 data_offset     the TZif data follows the index
  int32 final_offset    the end of the data
  index entries         char name[40] (NUL-padded), int32 start (from data_offset), int32 length, int32 0
"""
import os
import struct
import sys

NAME_LENGTH = 40
# Files in the zoneinfo directory that are not zones, and zones Android's tzdata does not have.
SKIPPED = {"posixrules", "localtime", "Factory"}


def zones(root):
    """(name, TZif bytes) for every zone below root, sorted by name."""
    found = []
    for directory, _, files in os.walk(root, followlinks=True):
        for file in files:
            path = os.path.join(directory, file)
            name = os.path.relpath(path, root)
            if name in SKIPPED or len(name) > NAME_LENGTH:
                continue
            with open(path, "rb") as stream:
                data = stream.read()
            if data.startswith(b"TZif"):
                found.append((name, data))
    return sorted(found)


def pack(version, entries):
    header_size = 24
    index_size = len(entries) * (NAME_LENGTH + 12)
    data_offset = header_size + index_size
    index = bytearray()
    data = bytearray()
    for name, tzif in entries:
        index += name.encode().ljust(NAME_LENGTH, b"\0")
        index += struct.pack(">iii", len(data), len(tzif), 0)
        data += tzif
    tag = ("tzdata" + version).encode()
    if len(tag) != 11:
        raise SystemExit(f"unexpected tz version {version!r}")
    header = tag + b"\0" + struct.pack(">iii", header_size, data_offset, data_offset + len(data))
    return header + bytes(index) + bytes(data)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    output = sys.argv[1]
    root = os.path.realpath(sys.argv[2] if len(sys.argv) == 3 else "/usr/share/zoneinfo")
    with open(os.path.join(root, "+VERSION")) as stream:
        version = stream.read().strip()
    entries = zones(root)
    if not any(name == "Etc/UTC" for name, _ in entries):
        raise SystemExit(f"{root} has no Etc/UTC")
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "wb") as stream:
        stream.write(pack(version, entries))
    print(f"{output}: tzdata{version}, {len(entries)} zones")


if __name__ == "__main__":
    main()
