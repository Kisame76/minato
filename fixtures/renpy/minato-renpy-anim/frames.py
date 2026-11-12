#!/usr/bin/env python3
"""Writes the Ren'Py anim game's frames into <folder> (its game/): <count> distinct 1920 x 1080 PNGs bg0.png ... and
anim.rpy, the title's animation of 20 * <count> frames at 0.04 s each, looping: frame i is bg<i % count>.png with its
hue turned by (i // count) * 18 degrees, so 30 PNGs give 600 different textures. Each PNG is picture-like (a smooth
two-axis gradient, hard-edged shapes and two bits of seeded noise), so that it packs about as well as real art: ~1.6x
with LZ4 and ~4x with LZFSE after a row delta filter, where flat blocks would pack 16-29x and hide what texture
parking really saves. The repository holds no image; the same <count> gives the same files.
Usage: frames.py <folder> <count>"""
import os
import random
import struct
import sys
import zlib

WIDTH, HEIGHT = 1920, 1080


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def frame(path, index):
    rng = random.Random(index)
    # Red: the x gradient, with the shapes' cells flipped; one row per band of 120 lines (5 band patterns).
    red_bands = []
    for band in range(5):
        row = bytearray(((x * 255 // WIDTH + index * 7) & 255) for x in range(WIDTH))
        for x in range(WIDTH):
            if ((x + index * 37) // 160 + band) % 5 == 0:
                row[x] ^= 0x60
        red_bands.append(bytes(row))
    # Blue: a diagonal gradient, read as a sliding window over x + y.
    diagonal = bytes((((t * 255) // (WIDTH + HEIGHT) + index * 5) & 255) for t in range(WIDTH + HEIGHT))
    shape_bands = []
    for band in range(5):
        row = bytearray(WIDTH)
        for x in range(WIDTH):
            if ((x + index * 37) // 160 + band) % 5 == 0:
                row[x] = 0x30
        shape_bands.append(int.from_bytes(bytes(row), "little"))
    mask = int.from_bytes(b"\x03" * WIDTH, "little")
    rows = []
    for y in range(HEIGHT):
        band = ((y + index * 11) // 120) % 5
        noise = int.from_bytes(rng.randbytes(WIDTH), "little") & mask
        red = (int.from_bytes(red_bands[band], "little") ^ noise).to_bytes(WIDTH, "little")
        green = (int.from_bytes(bytes([(y * 255 // HEIGHT + index * 3) & 255]) * WIDTH, "little")
                 ^ noise).to_bytes(WIDTH, "little")
        blue = (int.from_bytes(diagonal[y:y + WIDTH], "little") ^ shape_bands[band]).to_bytes(WIDTH, "little")
        row = bytearray(1 + WIDTH * 3)
        row[1::3] = red
        row[2::3] = green
        row[3::3] = blue
        rows.append(bytes(row))
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
                  + chunk(b"IDAT", zlib.compress(b"".join(rows), 1)) + chunk(b"IEND", b""))


def animation(path, count):
    # The header names anim_images.py, the generator's first name, so that 30 gives the anim game's anim.rpy byte for
    # byte.
    lines = ["# The title's frame-by-frame animation: %d different frames at 0.04 s each, looping. Frame i is"
             % (20 * count),
             "# bg<i %% %d>.png with its hue turned by (i // %d) * 18 degrees, so that %d PNGs (anim_images.py) give %d"
             % (count, count, count, 20 * count),
             "# different textures of picture-like content.",
             "image titleanim:"]
    for index in range(20 * count):
        lines.append('    im.MatrixColor("bg%d.png", im.matrix.hue(%d))' % (index % count, (index // count) * 18))
        lines.append("    0.04")
    lines.append("    repeat")
    with open(path, "w") as out:
        out.write("\n".join(lines) + "\n")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    folder, count = sys.argv[1], int(sys.argv[2])
    for index in range(count):
        frame(os.path.join(folder, f"bg{index}.png"), index)
    animation(os.path.join(folder, "anim.rpy"), count)


if __name__ == "__main__":
    main()
