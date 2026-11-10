#!/usr/bin/env python3
"""Makes the main OBB of obb-zip64-test: a Zip64 archive of stored (uncompressed) entries whose first entry, "big.bin", is
4 GiB and 64 KiB of zeros, sparse, and whose second entry, "after.txt", lies behind it, beyond 2^32, so that its local
header offset needs the Zip64 extra field as the archive's end needs the Zip64 end-of-central-directory record. Nothing
but the headers is written (the CRC of the zeros is computed, not stored): a few KiB on a filesystem with sparse files.
`adb push` sends every byte. Prints the path of main.1.<package>.obb in the folder given.

Usage: tools/make-sparse-zip64-obb.py <folder>
"""
import os
import struct
import sys
import zlib

PACKAGE = "io.github.kisame76.minato.test.obbzip64"
BIG = (1 << 32) + 65536
AFTER = b"after the big entry\n"
TAIL = b"THE-TAIL"  # the last eight bytes of big.bin


def crc_of_zeros_and_tail(length, tail):
    crc = 0
    chunk = bytes(16 << 20)
    remaining = length - len(tail)
    while remaining > 0:
        step = min(remaining, len(chunk))
        crc = zlib.crc32(chunk[:step], crc)
        remaining -= step
    return zlib.crc32(tail, crc)


def local_header(name, crc, size, zip64):
    extra = struct.pack("<HHQQ", 1, 16, size, size) if zip64 else b""
    sizes = (0xFFFFFFFF, 0xFFFFFFFF) if zip64 else (size, size)
    return struct.pack("<IHHHHHIIIHH", 0x04034B50, 45 if zip64 else 10, 0, 0, 0x21, 0x0021, crc, sizes[0], sizes[1],
                       len(name), len(extra)) + name + extra


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    path = os.path.join(sys.argv[1], f"main.1.{PACKAGE}.obb")
    big_name, after_name = b"big.bin", b"after.txt"
    crc_big = crc_of_zeros_and_tail(BIG, TAIL)
    crc_after = zlib.crc32(AFTER)
    with open(path, "wb") as obb:
        big_header = local_header(big_name, crc_big, BIG, True)
        obb.write(big_header)
        big_data = obb.tell()
        obb.truncate(big_data + BIG)
        obb.seek(big_data + BIG - len(TAIL))
        obb.write(TAIL)
        after_offset = obb.tell()
        obb.write(local_header(after_name, crc_after, len(AFTER), False))
        obb.write(AFTER)
        directory = obb.tell()
        # Central directory: big.bin with a Zip64 extra (sizes, offset), after.txt with its offset in a Zip64 extra.
        big_extra = struct.pack("<HHQQQ", 1, 24, BIG, BIG, 0)
        after_extra = struct.pack("<HHQ", 1, 8, after_offset)
        entries = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 45, 45, 0, 0, 0x21, 0x0021, crc_big, 0xFFFFFFFF, 0xFFFFFFFF,
                              len(big_name), len(big_extra), 0, 0, 0, 0, 0xFFFFFFFF) \
            + big_name + big_extra
        entries += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 45, 10, 0, 0, 0x21, 0x0021, crc_after, len(AFTER),
                               len(AFTER), len(after_name), len(after_extra), 0, 0, 0, 0, 0xFFFFFFFF) \
            + after_name + after_extra
        obb.write(entries)
        end64 = obb.tell()
        obb.write(struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, 2, 2, len(entries), directory))
        obb.write(struct.pack("<IIQI", 0x07064B50, 0, end64, 1))
        obb.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0))
    print(path)


if __name__ == "__main__":
    main()
