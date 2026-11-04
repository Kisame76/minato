#!/usr/bin/env python3
"""Writes the Ren'Py memory game's backgrounds (tools/renpy-memory.sh): <count> distinct 1920 x 1080 PNGs bg0.png ...
into <folder>, each a different gradient with a seeded pattern, so that the repository holds no image and no two
scenes share a texture. Usage: images.py <folder> <count>"""
import os
import struct
import sys
import zlib

WIDTH, HEIGHT = 1920, 1080


def png(path, red, green, blue):
    rows = []
    for y in range(HEIGHT):
        row = bytearray([0])
        for x in range(0, WIDTH, 8):
            pixel = bytes(((red + x // 8 * 3) & 255, (green + y // 4) & 255, (blue + (x // 8 ^ y // 4)) & 255))
            row += pixel * 8
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
                  + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    folder, count = sys.argv[1], int(sys.argv[2])
    for index in range(count):
        png(os.path.join(folder, f"bg{index}.png"), index * 37 & 255, index * 91 & 255, index * 53 & 255)


if __name__ == "__main__":
    main()
