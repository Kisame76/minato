#!/usr/bin/env python3
"""ADR-0058 d)'s pixel check of texture-view-test (tools/texture-view-check.sh): the screen where the app's two
TextureViews are, in the last post that graphics-run's fake buffers wrote (MINATO_WINDOW_DUMP, graphics/tests/
window_host.m), against Android 15's screen after the app's "done" (tools/android-reference.sh --app
texture-view-test).

The views' bounds come from the reference's log ("texture-view-test: plain bounds <l> <t> <r> <b>", the same for
"mirrored"), in the window, which fills the screen on both. fixtures/expected/texture-view-test.rgba.gz holds the
screen's RGBA bytes in those rectangles, the plain view's first, row by row. Each rectangle passes by
graphics/tests/canvas_compare.py's rule, its RGB bytes only: the fourth byte of an RGBX_8888 buffer is no alpha.

With --crop, it makes the reference instead, as tools/android-reference.sh calls it: from the raw output of
`screencap` (its 16-byte header, then RGBA_8888 rows) and the app's log.

Usage: texture_view_compare.py <expected .rgba.gz> <expected .logcat> <dump folder>
       texture_view_compare.py --crop <screencap output> <app .logcat> <out .rgba.gz>
"""
import gzip
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from canvas_compare import compare  # noqa: E402

POST = re.compile(r"^(\d+) window (\d+) (\d+)x(\d+) format (\d+) dirty (-?\d+) (-?\d+) (-?\d+) (-?\d+)$")
VIEW_BOUNDS = re.compile(r"texture-view-test: (plain|mirrored) bounds (\d+) (\d+) (\d+) (\d+)$")
NAMES = ("plain", "mirrored")
RGBA_8888 = 1


def opaque(pixels):
    """The RGBA bytes with every fourth byte 255: what an opaque window shows."""
    data = bytearray(pixels)
    data[3::4] = b"\xff" * (len(data) // 4)
    return bytes(data)


def views(log_lines):
    """{name: (left, top, right, bottom)} from the log, or a failure text."""
    found = {}
    for line in log_lines:
        match = VIEW_BOUNDS.search(line.strip())
        if match is None:
            continue
        if match.group(1) in found:
            return f"the log names the {match.group(1)} view's bounds twice"
        found[match.group(1)] = tuple(int(value) for value in match.groups()[1:])
    missing = [name for name in NAMES if name not in found]
    if missing:
        return f"no bounds of the {' and '.join(missing)} view in the log"
    return found


def rectangle(rows, width, height, bounds):
    """The RGBA bytes of `bounds` in rows of `width` pixels, `height` rows, or None when they leave the rows."""
    left, top, right, bottom = bounds
    if not (0 <= left < right <= width and 0 <= top < bottom <= height):
        return None
    return b"".join(rows[(line * width + left) * 4:(line * width + right) * 4] for line in range(top, bottom))


def crop(screencap, log_lines):
    """The reference's bytes from `screencap`'s output and the app's log."""
    width, height, pixel_format = struct.unpack("<3I", screencap[:12])
    if pixel_format != RGBA_8888 or len(screencap) != 16 + width * height * 4:
        raise SystemExit(f"screencap: {width} x {height} in format {pixel_format}, {len(screencap)} bytes")
    bounds = views(log_lines)
    if isinstance(bounds, str):
        raise SystemExit(bounds)
    parts = []
    for name in NAMES:
        part = rectangle(screencap[16:], width, height, bounds[name])
        if part is None:
            raise SystemExit(f"the {name} view's bounds {bounds[name]} leave the screen of {width} x {height}")
        parts.append(part)
    return b"".join(parts)


def check(expected, expected_log, dump):
    """The failures, empty when the last post passes."""
    bounds = views(expected_log)
    if isinstance(bounds, str):
        return [bounds]
    posts = []
    with open(os.path.join(dump, "posts.txt")) as handle:
        for line in handle:
            found = POST.match(line.strip())
            if not found:
                return [f"posts.txt: {line.strip()!r}"]
            posts.append(tuple(int(value) for value in found.groups()))
    if not posts:
        return ["no post"]
    number, _, width, height = posts[-1][:4]
    with open(os.path.join(dump, f"post-{number}.rgba"), "rb") as handle:
        window = handle.read()
    if len(window) != width * height * 4:
        return [f"post {number}: {len(window)} bytes, not {width * height * 4}"]
    failures = []
    offset = 0
    for name in NAMES:
        left, top, right, bottom = bounds[name]
        size = (right - left) * (bottom - top) * 4
        theirs = opaque(expected[offset:offset + size])
        offset += size
        ours = rectangle(window, width, height, bounds[name])
        if ours is None:
            failures.append(f"the {name} view's bounds {bounds[name]} leave post {number}'s {width} x {height}")
            continue
        passed, text = compare(theirs, opaque(ours), right - left, bottom - top)
        print(f"post {number} of {len(posts)}, the {name} view at {left} {top} {right} {bottom}: {text}")
        if not passed:
            failures.append(f"the {name} view: {text}")
    if offset != len(expected):
        failures.append(f"the reference has {len(expected)} bytes, the views' bounds {offset}")
    return failures


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--crop":
        with open(sys.argv[2], "rb") as handle:
            screencap = handle.read()
        with open(sys.argv[3]) as handle:
            log_lines = handle.read().splitlines()
        rows = crop(screencap, log_lines)
        # Without a name or a time in its header, as gzip -n writes canvas-test's: the same pixels, the same file.
        with open(sys.argv[4], "wb") as raw, gzip.GzipFile("", "wb", 9, raw, mtime=0) as handle:
            handle.write(rows)
        return
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    with gzip.open(sys.argv[1]) as handle:
        expected = handle.read()
    with open(sys.argv[2]) as handle:
        expected_log = handle.read().splitlines()
    failures = check(expected, expected_log, sys.argv[3])
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
