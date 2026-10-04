#!/usr/bin/env python3
"""The images of canvas-test (ADR-0043 test 2), synthetic and deterministic, so that no license applies: they are made
once and committed in fixtures/apps/canvas-test, and this script makes them again.
  - assets/picture.png: a 64 x 64 opaque picture with gradients, hard edges and stripes, written here;
  - assets/picture.jpg: the same through macOS's sips, at quality 90;
  - res/drawable-nodpi/frame.9.png: a 32 x 32 frame with a transparent middle and the 1-pixel border of a nine-patch,
    whose middle 8 pixels stretch both ways; aapt2 turns the border into the npTc chunk;
  - with --webp <adb>, assets/picture-lossy.webp and assets/picture-lossless.webp from Android 15's own encoder:
    graphics/tests/canvas-webp's program, dexed against framework.jar's classes, runs through app_process on the
    emulator that adb reaches (sips cannot write WebP).

Usage: canvas_images.py [--webp <adb>]   (from the repository's root)
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

APP = "fixtures/apps/canvas-test"
SIZE = 64
FRAME = 32


def png(path, width, height, rows, alpha):
    """A PNG of 8-bit RGB or RGBA rows, without the filters a deterministic writer does not need."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0)
    raw = b"".join(b"\0" + bytes(row) for row in rows)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9))
                     + chunk(b"IEND", b""))


def picture():
    """Gradients in red and green, a white square, a dark disk and blue stripes, so that every decoder works."""
    rows = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            r, g, b = x * 4, y * 4, 128
            if 8 <= x < 24 and 8 <= y < 24:
                r, g, b = 255, 255, 255
            elif (x - 44) ** 2 + (y - 20) ** 2 < 100:
                r, g, b = 32, 16, 64
            elif y >= 40 and (x + y) % 8 < 3:
                r, g, b = 0, 64, 255
            row += [r, g, b]
        rows.append(row)
    return rows


def frame():
    """A nine-patch: a 4-pixel border in dark blue around a transparent middle, with its stretch marks."""
    size = FRAME + 2
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            if x in (0, size - 1) or y in (0, size - 1):
                # The marks: the top row and the left column name the stretchable 8 pixels in the middle.
                marked = (y == 0 and 13 <= x < 21) or (x == 0 and 13 <= y < 21)
                row += [0, 0, 0, 255] if marked else [0, 0, 0, 0]
            elif min(x - 1, y - 1, FRAME - x, FRAME - y) < 4:
                row += [16, 48, 160, 255]
            else:
                row += [0, 0, 0, 0]
        rows.append(row)
    return size, rows


def webp(adb):
    """The two WebP files from Android 15's encoder, through app_process on the emulator."""
    jdk = next(os.path.join("build/tools/jdk-17", name, "Contents/Home") for name in os.listdir("build/tools/jdk-17"))
    source = "graphics/tests/canvas-webp"
    with tempfile.TemporaryDirectory() as work:
        java = [os.path.join(folder, name) for folder, _, names in os.walk(source) for name in names]
        subprocess.run([f"{jdk}/bin/javac", "-source", "8", "-target", "8", "-Xlint:-options", "-bootclasspath",
                        "build/art/java/all-classes.jar", "-classpath",
                        "build/framework/classes.jar:build/framework/stubs.jar", "-d", f"{work}/classes"] + java,
                       check=True)
        classes = [os.path.join(folder, name) for folder, _, names in os.walk(f"{work}/classes") for name in names]
        os.makedirs(f"{work}/dex")
        subprocess.run([f"{jdk}/bin/java", "-cp", "build/sources/r8/r8.jar", "com.android.tools.r8.D8", "--release",
                        "--min-api", "29", "--lib", "build/art/java/all-classes.jar", "--lib",
                        "build/framework/classes.jar", "--output", f"{work}/dex"] + classes, check=True)
        device = "/data/local/tmp/canvas-webp"
        subprocess.run([adb, "shell", f"rm -rf {device} && mkdir -p {device}"], check=True)
        subprocess.run([adb, "push", f"{work}/dex/classes.dex", f"{device}/webp.dex"], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([adb, "push", f"{APP}/assets/picture.png", f"{device}/picture.png"], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([adb, "shell", f"CLASSPATH={device}/webp.dex app_process {device} "
                        f"io.github.kisame76.minato.test.canvas.WebpEncode {device}/picture.png "
                        f"{device}/picture-lossy.webp {device}/picture-lossless.webp"], check=True)
        for name in ("picture-lossy.webp", "picture-lossless.webp"):
            subprocess.run([adb, "pull", f"{device}/{name}", f"{APP}/assets/{name}"], check=True,
                           stdout=subprocess.DEVNULL)
        subprocess.run([adb, "shell", f"rm -rf {device}"], check=True)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--webp":
        webp(sys.argv[2])
        return
    if len(sys.argv) != 1:
        raise SystemExit(__doc__)
    os.makedirs(f"{APP}/assets", exist_ok=True)
    os.makedirs(f"{APP}/res/drawable-nodpi", exist_ok=True)
    png(f"{APP}/assets/picture.png", SIZE, SIZE, picture(), alpha=False)
    subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "90", f"{APP}/assets/picture.png",
                    "--out", f"{APP}/assets/picture.jpg"], check=True, stdout=subprocess.DEVNULL)
    size, rows = frame()
    png(f"{APP}/res/drawable-nodpi/frame.9.png", size, size, rows, alpha=True)


if __name__ == "__main__":
    main()
