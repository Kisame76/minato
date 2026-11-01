#!/usr/bin/env python3
"""ADR-0043 step c3's pixel check of views-test (tools/views-check.sh): the window that graphics-run's fake buffers
wrote post by post (MINATO_WINDOW_DUMP, graphics/tests/window_host.m) against Android 15's screen after the app's
"done" (tools/android-reference.sh --app views-test; its twin views-test-default has the same lines under its own tag).

The reference is the screen's rows between the AVD's system bars, since SystemUI draws over the rest:
fixtures/expected/views-test.rgba.gz holds those rows as RGBA bytes, fixtures/expected/views-test.rows the screen's
size and the bars' insets ("width <w> height <h> top <t> bottom <b>"), which the app logged under views-test-device.
The check:
  - the last post's window equals the reference in those rows, by graphics/tests/canvas_compare.py's rule, its RGB
    bytes only: the fourth byte of an RGBX_8888 buffer is no alpha, and SurfaceFlinger shows an opaque window;
  - so does each view whose bounds the reference's log names (label, block, bitmap, TextView and Button), on its own
    by the same rule, so that a small view's difference cannot hide in the window's 0.1 %: the bitmap's 96 x 96
    pixels drawn at the wrong density differ in 566 of the window's 3.4 million;
  - the first post takes the whole window, as a new surface's first draw does (and again at REDRAW_NEEDED); a later
    one takes exactly the union of the label's and the Button's bounds from the reference's log
    ("label bounds", "button bounds"): AOSP's software path draws one dirty rectangle per traversal, the union of the
    frame's invalidates, and the label's text change and the Button's drawable state, which follows the window's
    focus, come from the same focus message before the next frame (a split into two traversals fails here, loudly);
    the last post takes only the Button's bounds, the redraw the app asks for once the click's ripple has gone;
  - every post is RGBX_8888 (format 2), the buffers of an opaque window, whose decor has an opaque background.

With --crop, it makes the reference instead, as tools/android-reference.sh calls it: from the raw output of
`screencap` (its 16-byte header, then RGBA_8888 rows) and the app's views-test-device lines, the two expected files.

Usage: views_compare.py <expected .rgba.gz> <rows file> <expected .logcat> <dump folder>
       views_compare.py --crop <screencap output> <device .logcat> <out .rgba.gz> <out rows file>
"""
import gzip
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "graphics", "tests"))
from canvas_compare import compare  # noqa: E402

POST = re.compile(r"^(\d+) window (\d+) (\d+)x(\d+) format (\d+) dirty (-?\d+) (-?\d+) (-?\d+) (-?\d+)$")
ROWS = re.compile(r"^width (\d+) height (\d+) top (\d+) bottom (\d+)$")
INSETS = re.compile(r"views-test(?:-default|-fonts)?-device: insets top (\d+) bottom (\d+)$")
VIEW_BOUNDS = re.compile(r"views-test(?:-default|-fonts)?: (\w+) bounds (\d+) (\d+) (\d+) (\d+)$")
RGBA_8888 = 1
RGBX_8888 = 2


def opaque(pixels):
    """The RGBA bytes with every fourth byte 255: what an opaque window shows."""
    data = bytearray(pixels)
    data[3::4] = b"\xff" * (len(data) // 4)
    return bytes(data)


def rectangle(rows, width, left, right, lines):
    """The RGBA bytes of the columns `left` to `right` of `lines`, rows of `rows`, each `width` pixels long."""
    return b"".join(rows[(line * width + left) * 4:(line * width + right) * 4] for line in lines)


def crop(screencap, device_lines):
    """(rows file text, RGBA bytes of the rows between the bars) from `screencap`'s output and the device lines."""
    width, height, pixel_format = struct.unpack("<3I", screencap[:12])
    if pixel_format != RGBA_8888 or len(screencap) != 16 + width * height * 4:
        raise SystemExit(f"screencap: {width} x {height} in format {pixel_format}, {len(screencap)} bytes")
    insets = [INSETS.search(line.strip()) for line in device_lines]
    insets = [match for match in insets if match]
    if len(insets) != 1:
        raise SystemExit(f"{len(insets)} lines of the insets in the device's log, not 1")
    top, bottom = int(insets[0].group(1)), int(insets[0].group(2))
    rows = screencap[16 + top * width * 4:16 + (height - bottom) * width * 4]
    return f"width {width} height {height} top {top} bottom {bottom}\n", rows


def check(expected, rows_text, expected_log, dump):
    """The failures, empty when the posts pass."""
    match = ROWS.match(rows_text.strip())
    if not match:
        return [f"rows file: {rows_text.strip()!r}"]
    width, height, top, bottom = (int(value) for value in match.groups())
    views = {}
    for line in expected_log:
        found = VIEW_BOUNDS.search(line.strip())
        if found is None:
            continue
        if found.group(1) in views:
            return [f"the reference's log names the {found.group(1)}'s bounds twice"]
        views[found.group(1)] = tuple(int(value) for value in found.groups()[1:])
    for view in ("label", "button"):
        if view not in views:
            return [f"no line of the {view}'s bounds in the reference's log"]
    posts = []
    with open(os.path.join(dump, "posts.txt")) as handle:
        for line in handle:
            found = POST.match(line.strip())
            if not found:
                return [f"posts.txt: {line.strip()!r}"]
            posts.append(tuple(int(value) for value in found.groups()))
    if not posts:
        return ["no post"]
    failures = []
    for number, window, post_width, post_height, post_format, *dirty in posts:
        if (post_width, post_height) != (width, height):
            failures.append(f"post {number}: {post_width} x {post_height}, not the screen's {width} x {height}")
        if post_format != RGBX_8888:
            failures.append(f"post {number}: format {post_format}, not RGBX_8888 ({RGBX_8888})")
    if tuple(posts[0][5:]) != (0, 0, width, height):
        failures.append(f"post 1 drew {posts[0][5:]}, not the whole window")
    label, button = views["label"], views["button"]
    union = (min(label[0], button[0]), min(label[1], button[1]), max(label[2], button[2]), max(label[3], button[3]))
    if not any(tuple(post[5:]) == union for post in posts[1:]):
        failures.append(f"no post drew the union of the label's and the Button's bounds {union}")
    if tuple(posts[-1][5:]) != views["button"]:
        failures.append(f"post {posts[-1][0]} drew {posts[-1][5:]}, not the Button's bounds {views['button']}")
    with open(os.path.join(dump, f"post-{posts[-1][0]}.rgba"), "rb") as handle:
        window = handle.read()
    if len(window) != width * height * 4:
        return failures + [f"post {posts[-1][0]}: {len(window)} bytes, not {width * height * 4}"]
    theirs = opaque(expected)
    ours = opaque(window[top * width * 4:(height - bottom) * width * 4])
    passed, text = compare(theirs, ours, width, height - top - bottom)
    print(f"post {posts[-1][0]} of {len(posts)}, rows {top} to {height - bottom}: {text}")
    if not passed:
        failures.append(text)
    for name, (left, view_top, right, view_bottom) in views.items():
        if not (0 <= left < right <= width and top <= view_top < view_bottom <= height - bottom):
            failures.append(f"the {name}'s bounds {left} {view_top} {right} {view_bottom} leave rows {top} to "
                            f"{height - bottom}")
            continue
        lines = range(view_top - top, view_bottom - top)
        passed, text = compare(rectangle(theirs, width, left, right, lines), rectangle(ours, width, left, right, lines),
                               right - left, view_bottom - view_top)
        print(f"  the {name}, {left} {view_top} {right} {view_bottom}: {text}")
        if not passed:
            failures.append(f"the {name}: {text}")
    return failures


def main():
    if len(sys.argv) == 6 and sys.argv[1] == "--crop":
        with open(sys.argv[2], "rb") as handle:
            screencap = handle.read()
        with open(sys.argv[3]) as handle:
            device_lines = handle.read().splitlines()
        rows_text, rows = crop(screencap, device_lines)
        # Without a name or a time in its header, as gzip -n writes canvas-test's: the same pixels, the same file.
        with open(sys.argv[4], "wb") as raw, gzip.GzipFile("", "wb", 9, raw, mtime=0) as handle:
            handle.write(rows)
        with open(sys.argv[5], "w") as handle:
            handle.write(rows_text)
        return
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    with gzip.open(sys.argv[1]) as handle:
        expected = handle.read()
    with open(sys.argv[2]) as handle:
        rows_text = handle.read()
    with open(sys.argv[3]) as handle:
        expected_log = handle.read().splitlines()
    failures = check(expected, rows_text, expected_log, sys.argv[4])
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
