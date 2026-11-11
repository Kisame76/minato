#!/usr/bin/env python3
"""tools/window_pixels.py (ADR-0043 b5, step f), on files made for the test:
  - screen: a screenshot that shows the dump's buffer is ok, in PNGs of each of the five row filters, RGBA and RGB,
    whose pixels in few levels make Paeth's predictor meet its ties;
    one 10 off at a grid point DIFFERS and names the point; a translucent buffer shows its RGB over black; with a
    Display P3 profile the buffer's sRGB is compared as P3 (pure sRGB red as P3's 234 51 35, the known conversion
    within 1); a hidden window is not compared, one hidden with visible pixels DIFFERS; a dump of the wrong size is
    refused; of two windows, each is compared
    at its frame's points that no layer covers, a dialog the screen lacks DIFFERS, translucent pixels over another
    window and a window at half opacity are not compared;
  - posts: each window's last post equal to the reference's passes, another CRC or size DIFFERS, hidden posts match
    hidden ones, and a log without CRC lines is refused.
Runs on /usr/bin/python3 (3.9).
Usage: tools/tests/window_pixels_test.py
"""
import importlib.util
import io
import os
import struct
import sys
import tempfile
import zlib
from contextlib import redirect_stderr, redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location("window_pixels", os.path.join(ROOT, "tools", "window_pixels.py"))
window_pixels = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(window_pixels)

WIDTH, HEIGHT = 12, 18


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)


def filtered(rows, channels):
    """The rows with filter (index mod 5), encoded as a PNG encoder would."""
    out, previous = b"", bytes(len(rows[0]))
    for index, row in enumerate(rows):
        kind = index % 5
        line = bytearray()
        for x, value in enumerate(row):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 0:
                guess = 0
            elif kind == 1:
                guess = left
            elif kind == 2:
                guess = up
            elif kind == 3:
                guess = (left + up) // 2
            else:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                guess = left if pa <= pb and pa <= pc else up if pb <= pc else corner
            line.append((value - guess) & 0xff)
        out += bytes([kind]) + bytes(line)
        previous = row
    return out


def png(path, pixel, channels=4, p3=False):
    """A screenshot of WIDTH x HEIGHT whose pixels are pixel(x, y) (RGB), varied so that the filters matter."""
    rows = []
    for y in range(HEIGHT):
        row = bytearray()
        for x in range(WIDTH):
            row += bytes(pixel(x, y)) + (b"\xff" if channels == 4 else b"")
        rows.append(bytes(row))
    body = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 6 if channels == 4 else 2,
                                                                0, 0, 0))
    if p3:
        body += chunk(b"iCCP", b"Display P3\0\0" + zlib.compress(b"desc Display P3 profile"))
    body += chunk(b"IDAT", zlib.compress(filtered(rows, channels))) + chunk(b"IEND", b"")
    open(path, "wb").write(body)


def dump(folder, rgba, facts="12x18 RGBA_8888 post 3 shown", window=1, size=(WIDTH, HEIGHT)):
    os.makedirs(folder, exist_ok=True)
    open(f"{folder}/window-{window}.txt", "w").write(facts + "\n")
    open(f"{folder}/window-{window}.rgba", "wb").write(bytes(b for y in range(size[1]) for x in range(size[0])
                                                             for b in rgba(x, y)))


def run(*arguments):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        status = window_pixels.main(list(arguments))
    return status, out.getvalue() + err.getvalue()


def main():
    failures = []

    def check(name, got, want_status, want_text=""):
        status, text = got
        if status != want_status or want_text not in text:
            failures.append(f"{name}: status {status}, wanted {want_status} with {want_text!r}; printed {text!r}")

    def gradient(x, y):
        # Pseudo-random values in a few levels, so that Paeth's predictor meets its ties.
        value = (x * 7919 + y * 104729 + x * y * 31) % 9973
        return (value % 4 * 64, value // 4 % 4 * 64, value // 16 % 4 * 64)

    with tempfile.TemporaryDirectory() as work:
        dump(f"{work}/d", lambda x, y: gradient(x, y) + (255,))
        png(f"{work}/same.png", gradient)
        check("the same pixels, RGBA", run("screen", f"{work}/d", f"{work}/same.png"), 0, "largest error 0: ok")
        png(f"{work}/same-rgb.png", gradient, channels=3)
        check("the same pixels, RGB", run("screen", f"{work}/d", f"{work}/same-rgb.png"), 0, "largest error 0: ok")
        png(f"{work}/off.png", lambda x, y: tuple(min(c + 10, 255) for c in gradient(x, y)) if (x, y) == (6, 9)
            else gradient(x, y))
        check("10 off at the center", run("screen", f"{work}/d", f"{work}/off.png"), 1, "at 3/6, 3/6")
        check("10 off within a tolerance of 10", run("screen", f"{work}/d", f"{work}/off.png", "--tolerance", "10"),
              0, ": ok")
        # Premultiplied half-transparent gray over game mode's black: its RGB.
        dump(f"{work}/t", lambda x, y: (60, 60, 60, 128))
        png(f"{work}/t.png", lambda x, y: (60, 60, 60))
        check("translucent over black", run("screen", f"{work}/t", f"{work}/t.png"), 0, ": ok")
        dump(f"{work}/red", lambda x, y: (255, 0, 0, 255), "12x18 RGBX_8888 on black post 1 shown")
        png(f"{work}/red-p3.png", lambda x, y: (234, 51, 35), p3=True)
        check("sRGB red as Display P3", run("screen", f"{work}/red", f"{work}/red-p3.png", "--tolerance", "1"), 0,
              "P3; 9 points")
        png(f"{work}/red-srgb.png", lambda x, y: (255, 0, 0), p3=True)
        check("sRGB red unconverted on a P3 screen", run("screen", f"{work}/red", f"{work}/red-srgb.png"), 1,
              "DIFFERS")
        dump(f"{work}/h", lambda x, y: (0, 0, 0, 0), "12x18 RGBA_8888 post 2 hidden")
        check("hidden", run("screen", f"{work}/h", f"{work}/same.png"), 0, "not compared")
        dump(f"{work}/ht", lambda x, y: (0, 0, 0, 0), "12x18 RGBA_8888 post 2 hidden transparent")
        check("hidden as transparent", run("screen", f"{work}/ht", f"{work}/same.png"), 0, "not compared")
        # A buffer with visible pixels that the hide rule hid is a black screen, not a window to skip.
        dump(f"{work}/hv", lambda x, y: (60, 60, 60, 255), "12x18 RGBA_8888 post 2 hidden visible")
        check("a visible buffer hidden", run("screen", f"{work}/hv", f"{work}/same.png"), 1,
              "a visible buffer was hidden as transparent, not shown: DIFFERS")
        dump(f"{work}/w", lambda x, y: (0, 0, 0, 255), "13x18 RGBA_8888 post 2 shown")
        check("a dump of the wrong size", run("screen", f"{work}/w", f"{work}/same.png"), 2, "bytes for 13x18")

        # Two windows in a root of 4 x 6 points (the screenshot's 12 x 18 pixels at 3x): window 1 covers it, and
        # window 2 is a 6 x 6 pixel dialog at 1, 2 points over it, whose top left point a layer covers.
        def scene(x, y):
            return (200, 10, 10) if 3 <= x < 9 and 6 <= y < 12 and not (x < 6 and y < 9) else gradient(x, y)

        png(f"{work}/scene.png", scene)
        dump(f"{work}/s", lambda x, y: gradient(x, y) + (255,),
             "12x18 RGBA_8888 post 4 shown\nframe 0 0 4 6 in 4 6 alpha 1\nabove 1 2 2 2", 1)
        dump(f"{work}/s", lambda x, y: (200, 10, 10, 255), "6x6 RGBX_8888 on black post 2 shown\n"
             "frame 1 2 2 2 in 4 6 alpha 1\nabove 1 2 1 1\nunder 1", 2, (6, 6))
        check("two windows, each where it shows", run("screen", f"{work}/s", f"{work}/scene.png"), 0,
              "window 2: 6x6 RGBX_8888 on black post 2 shown; screen 12x18; 8 of 9 points, largest error 0: ok")
        check("the lower window only beside the upper one", run("screen", f"{work}/s", f"{work}/scene.png"), 0,
              "window 1: 12x18 RGBA_8888 post 4 shown; screen 12x18; 8 of 9 points, largest error 0: ok")
        png(f"{work}/no-dialog.png", gradient)
        check("a dialog the screen does not show", run("screen", f"{work}/s", f"{work}/no-dialog.png"), 1,
              "window 2: 6x6 RGBX_8888 on black post 2 shown; screen 12x18; 8 of 9 points, largest error 200: DIFFERS")
        # A dialog's dim over window 1 (a black layer at 0.4, written as a tint): window 1 compared where only the
        # dim covers it, as its color times 0.6; the dialog at 1, 2 points over both.
        def dimmed(x, y):
            return (200, 10, 10) if 3 <= x < 9 and 6 <= y < 12 else tuple(round(c * 0.6) for c in gradient(x, y))

        png(f"{work}/dimmed.png", dimmed)
        dump(f"{work}/m", lambda x, y: gradient(x, y) + (255,),
             "12x18 RGBA_8888 post 4 shown\nframe 0 0 4 6 in 4 6 alpha 1\ntint 0 0 4 6 0 0 0 0.4\nabove 1 2 2 2", 1)
        dump(f"{work}/m", lambda x, y: (200, 10, 10, 255), "6x6 RGBX_8888 on black post 2 shown\n"
             "frame 1 2 2 2 in 4 6 alpha 1\nunder 1", 2, (6, 6))
        check("a window under a dialog's dim", run("screen", f"{work}/m", f"{work}/dimmed.png"), 0,
              "window 1: 12x18 RGBA_8888 post 4 shown; screen 12x18; 8 of 9 points, largest error 0: ok")
        check("the dialog over its dim", run("screen", f"{work}/m", f"{work}/dimmed.png"), 0,
              "window 2: 6x6 RGBX_8888 on black post 2 shown; screen 12x18; 9 points, largest error 0: ok")
        check("a dim the screen does not show", run("screen", f"{work}/m", f"{work}/scene.png"), 1,
              "window 1: 12x18 RGBA_8888 post 4 shown; screen 12x18; 8 of 9 points, largest error")
        # On a Display P3 screen the color is converted first, then dimmed, as Core Animation blends in the screen's
        # space: sRGB red is (234, 51, 35) there, (140, 31, 21) under a dim of 0.4.
        dump(f"{work}/mp", lambda x, y: (255, 0, 0, 255),
             "12x18 RGBX_8888 on black post 1 shown\nframe 0 0 4 6 in 4 6 alpha 1\ntint 0 0 4 6 0 0 0 0.4", 1)
        png(f"{work}/mp.png", lambda x, y: (140, 31, 21), p3=True)
        check("a dimmed window on a P3 screen", run("screen", f"{work}/mp", f"{work}/mp.png", "--tolerance", "1"), 0,
              "P3; 9 points")
        # A tint of full alpha hides what is under it, as a layer drawn over it.
        dump(f"{work}/f", lambda x, y: gradient(x, y) + (255,),
             "12x18 RGBA_8888 post 4 shown\nframe 0 0 4 6 in 4 6 alpha 1\ntint 0 0 4 6 0 0 0 1", 1)
        check("a tint of full alpha", run("screen", f"{work}/f", f"{work}/same.png"), 0, "0 of 9 points: not compared")

        # A translucent pixel over another window is not compared, nor a window drawn at half opacity.
        dump(f"{work}/u", lambda x, y: (60, 60, 60, 128), "6x6 RGBA_8888 post 1 shown\n"
             "frame 1 2 2 2 in 4 6 alpha 1\nunder 1", 2, (6, 6))
        dump(f"{work}/u", lambda x, y: gradient(x, y) + (255,), "12x18 RGBA_8888 post 4 shown\n"
             "frame 0 0 4 6 in 4 6 alpha 1\nabove 1 2 2 2", 1)
        check("translucent over a window", run("screen", f"{work}/u", f"{work}/same.png"), 0,
              "window 2: 6x6 RGBA_8888 post 1 shown; 0 of 9 points: not compared")
        dump(f"{work}/a", lambda x, y: gradient(x, y) + (255,), "12x18 RGBA_8888 post 4 shown\n"
             "frame 0 0 4 6 in 4 6 alpha 0.5", 1)
        check("half opacity", run("screen", f"{work}/a", f"{work}/same.png"), 0, "drawn at opacity 0.5, not compared")

        log, reference = f"{work}/log", f"{work}/reference"
        open(log, "w").write("I/x: a\nminato window 1: post 1 12x18 RGBA_8888 CRC-32 0000000a\n"
                             "minato window 1: post 2 12x18 RGBX_8888 on black CRC-32 0000000b\n"
                             "minato window 2: post 1 hidden\n")
        open(reference, "w").write("minato window 1: post 5 12x18 RGBX_8888 on black CRC-32 0000000b\n"
                                   "minato window 2: post 3 hidden\n")
        check("equal last posts", run("posts", log, reference), 0, "window 2: last post 1 hidden; reference 3 hidden")
        open(reference, "w").write("minato window 1: post 2 12x18 RGBX_8888 CRC-32 0000000c\n"
                                   "minato window 2: post 3 hidden\n")
        check("another CRC", run("posts", log, reference), 1, "window 1: last post 2 12x18 RGBX_8888 0000000b")
        open(f"{work}/none", "w").write("I/x: no lines\n")
        check("no CRC lines", run("posts", f"{work}/none", reference), 2, "has no CRC lines")

    for failure in failures:
        print("FAIL", failure)
    print(f"window_pixels_test: {'all passed' if not failures else f'{len(failures)} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
