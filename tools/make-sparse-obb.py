#!/usr/bin/env python3
"""Makes the main OBB of obb-large-test (a sparse file of 2 GiB and 128 MiB with eight-byte tags at the offsets the test
reads) in the folder given, named main.1.<package>.obb, and prints its path. The file is sparse where the filesystem has
sparse files (APFS, ext4); nothing else is written, so it costs a few KiB on the Mac. `adb push` sends every byte.

Usage: tools/make-sparse-obb.py <folder>
"""
import os
import sys

PACKAGE = "io.github.kisame76.minato.test.obblarge"
GIB2 = 1 << 31
SIZE = GIB2 + (128 << 20)
# (offset, tag): the first bytes, eight bytes spanning 2 GiB, a tag above 2 GiB and the last eight bytes.
TAGS = [(0, b"FIRST-08"), (GIB2 - 4, b"SPANNING"), (GIB2 + 4096, b"ABOVE2G!"), (SIZE - 8, b"THE-END!")]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, f"main.1.{PACKAGE}.obb")
    with open(path, "wb") as obb:
        obb.truncate(SIZE)
        for offset, tag in TAGS:
            obb.seek(offset)
            obb.write(tag)
    print(path)


if __name__ == "__main__":
    main()
