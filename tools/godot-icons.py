#!/usr/bin/env python3
"""Writes godot-bench's launcher icons (ADR-0042 a) into the given folder, so its APKs carry icons of ours
instead of the export template's, which are Godot's logo: icon_192.png, and the adaptive icon's
foreground_432.png, background_432.png and monochrome_432.png. A plain design of ours: a light disc on a dark
ground. tools/build-godot.sh calls it for its copy of the project. Runs on /usr/bin/python3 (3.9).
Usage: tools/godot-icons.py <folder>
"""
import os
import struct
import sys
import zlib

GROUND = (20, 33, 61, 255)
DISC = (252, 163, 17, 255)
CLEAR = (0, 0, 0, 0)
WHITE = (255, 255, 255, 255)


def png(path, size, pixel):
    """An RGBA PNG of size x size, colored by pixel(x, y)."""
    raw = b"".join(b"\x00" + bytes(channel for x in range(size) for channel in pixel(x, y)) for y in range(size))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def disc(size, radius, inside, outside):
    center = (size - 1) / 2
    return lambda x, y: inside if (x - center) ** 2 + (y - center) ** 2 <= radius ** 2 else outside


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: tools/godot-icons.py <folder>")
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    png(os.path.join(folder, "icon_192.png"), 192, disc(192, 56, DISC, GROUND))
    # The adaptive icon's visible part is its middle 66 %, so the disc keeps inside it.
    png(os.path.join(folder, "foreground_432.png"), 432, disc(432, 100, DISC, CLEAR))
    png(os.path.join(folder, "background_432.png"), 432, lambda x, y: GROUND)
    png(os.path.join(folder, "monochrome_432.png"), 432, disc(432, 100, WHITE, CLEAR))


if __name__ == "__main__":
    main()
