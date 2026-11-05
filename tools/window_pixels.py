"""Step f of ADR-0043 b5 (tools/window-contents-check.sh): compares what the iPhone shows with what graphics/host's
windows posted, with the standard library only (Python 3.9).

  window_pixels.py screen <dump folder> <screenshot.png> [--tolerance <n>]
    The dump is SessionCheck's window-dump (minato_graphics_window_dump): window-<id>.rgba and window-<id>.txt for
    each window, whose lines after the first give its layer's frame in its root's points, the opacity it is drawn
    with, what is drawn over it, the tints over it (a layer of one color drawn at less than full alpha, such as a
    dialog's dim) and the windows under it. At 9 points of a 3 x 3 grid of each window's frame (at 1/6, 1/2 and 5/6
    of each side) that nothing but tints covers, the screenshot must show its buffer, RGBA over game mode's black,
    within the tolerance per channel (3), after the buffer's sRGB is converted to the screenshot's Display P3 when
    its ICC profile says so, and then each tint over the point blended in as Core Animation blends it, the color
    times 1 - alpha plus the tint's times alpha (a black dim: the color times 1 - dim). A window the hide rule hid or
    drawn at less than full opacity is not compared, nor a translucent pixel over another window. Exit 0 when every
    window compared matches, 1 when one differs, 2 when the files cannot be read.
  window_pixels.py posts <log> <reference log>
    The CRC lines window.m writes (MINATO_WINDOW_CRC): each window's last post in <log> must have the CRC-32 of the
    reference's last post of the same window and size. Prints the first window that differs. Exit 0, 1 or 2."""
import os
import re
import struct
import sys
import zlib

POST = re.compile(r"minato window (\d+): post (\d+) (?:(\d+)x(\d+) (\S+)(?: on black)? CRC-32 ([0-9a-f]{8})|hidden)")


class Refused(Exception):
    pass


def read_png(path):
    """(width, height, rows of RGBA bytes, whether the ICC profile is Display P3)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise Refused(f"{path} is no PNG")
    at, idat, p3 = 8, b"", False
    width = height = depth = color = 0
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"iCCP":
            name = body.split(b"\0", 1)[0]
            p3 = b"P3" in name or b"Display P3" in zlib.decompress(body[len(name) + 2:])[:256]
    if depth != 8 or color not in (2, 6):
        raise Refused(f"{path}: depth {depth}, color type {color}; only 8-bit RGB or RGBA")
    channels = 4 if color == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xff
            elif kind == 2:
                line[x] = (line[x] + up) & 0xff
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 0xff
            elif kind == 4:
                guess = left + up - corner
                near = min((abs(guess - left), 0, left), (abs(guess - up), 1, up), (abs(guess - corner), 2, corner))
                line[x] = (line[x] + near[2]) & 0xff
        if channels == 3:
            rows.append(bytes(b for i in range(width) for b in line[3 * i:3 * i + 3] + b"\xff"))
        else:
            rows.append(bytes(line))
        previous = line
    return width, height, rows, p3


def srgb_to_p3(rgb):
    """An sRGB color as Display P3 values: the same white point and transfer curve, other primaries."""
    def linear(value):
        value /= 255
        return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4

    def encoded(value):
        value = min(max(value, 0.0), 1.0)
        value = value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055
        return round(value * 255)

    r, g, b = (linear(c) for c in rgb)
    return (encoded(0.8225 * r + 0.1774 * g),
            encoded(0.0332 * r + 0.9669 * g),
            encoded(0.0171 * r + 0.0724 * g + 0.9108 * b))


def placement(lines):
    """A dump's frame in its root's points, the root's size, the opacity it is drawn with, the rectangles drawn over
    it, the tints over it in their drawing order (a rectangle, an sRGB color and its alpha) and the windows under it;
    without a frame line the window covers the screen. A tint of full alpha covers as a layer drawn over it."""
    frame, root, alpha, above, tints, under = (0.0, 0.0, 1.0, 1.0), (1.0, 1.0), 1.0, [], [], []
    for line in lines:
        words = line.split()
        if words[:1] == ["frame"] and len(words) == 10:
            frame, root, alpha = tuple(map(float, words[1:5])), (float(words[6]), float(words[7])), float(words[9])
        elif words[:1] == ["above"] and len(words) == 5:
            above.append(tuple(map(float, words[1:5])))
        elif words[:1] == ["tint"] and len(words) == 9:
            rectangle, color, amount = tuple(map(float, words[1:5])), tuple(map(int, words[5:8])), float(words[8])
            if amount >= 1:
                above.append(rectangle)
            elif amount > 0:
                tints.append((rectangle, color, amount))
        elif words[:1] == ["under"]:
            under.append(words[1])
    return frame, root, alpha, above, tints, under


def compare_window(folder, window, image, tolerance):
    """Prints window <window>'s verdict; True when it differs."""
    width, height, rows, p3 = image
    lines = open(f"{folder}/window-{window}.txt").read().splitlines()
    facts = lines[0].split()
    if facts[-1] == "hidden":
        print(f"window {window}: hidden by the hide rule, not compared")
        return False
    size = re.fullmatch(r"(\d+)x(\d+)", facts[0])
    if size is None:
        raise Refused(f"{folder}/window-{window}.txt: {lines[0]}")
    buffer_width, buffer_height = int(size.group(1)), int(size.group(2))
    pixels = open(f"{folder}/window-{window}.rgba", "rb").read()
    if len(pixels) != buffer_width * buffer_height * 4:
        raise Refused(f"{folder}/window-{window}.rgba: {len(pixels)} bytes for {buffer_width}x{buffer_height}")
    (fx0, fy0, fw, fh), (root_width, root_height), alpha, above, tints, under = placement(lines[1:])
    if alpha != 1:
        print(f"window {window}: {lines[0]}; drawn at opacity {alpha:g}, not compared")
        return False
    largest, worst, compared = 0, "", 0
    for fy in (1, 3, 5):
        for fx in (1, 3, 5):
            # The point in the root's points, where nothing may be drawn over the window.
            px, py = fx0 + fw * fx / 6, fy0 + fh * fy / 6
            if any(x <= px < x + w and y <= py < y + h for x, y, w, h in above):
                continue
            bx, by = buffer_width * fx // 6, buffer_height * fy // 6
            r, g, b, a = pixels[(by * buffer_width + bx) * 4:(by * buffer_width + bx) * 4 + 4]
            if a != 255 and under:
                continue  # translucent over another window
            sx, sy = int(px * width / root_width), int(py * height / root_height)
            want = srgb_to_p3((r, g, b)) if p3 else (r, g, b)  # premultiplied over black: the RGB as it is
            for (x, y, w, h), color, amount in tints:
                if x <= px < x + w and y <= py < y + h:
                    tint = srgb_to_p3(color) if p3 else color
                    want = tuple(c * (1 - amount) + t * amount for c, t in zip(want, tint))
            want = tuple(round(c) for c in want)
            have = tuple(rows[sy][4 * sx:4 * sx + 3])
            error = max(abs(w - h) for w, h in zip(want, have))
            compared += 1
            if error > largest:
                largest, worst = error, f" (at {fx}/6, {fy}/6: want {want}, screen {have})"
    points = "9 points" if compared == 9 else f"{compared} of 9 points"
    if compared == 0:
        print(f"window {window}: {lines[0]}; {points}: not compared")
        return False
    verdict = "ok" if largest <= tolerance else "DIFFERS"
    print(f"window {window}: {lines[0]}; screen {width}x{height}{' P3' if p3 else ''}; {points}, largest error "
          f"{largest}: {verdict}{worst if largest > tolerance else ''}")
    return largest > tolerance


def screen(folder, png, tolerance):
    image = read_png(png)
    windows = sorted((int(name[7:-4]) for name in os.listdir(folder)
                      if name.startswith("window-") and name.endswith(".txt")))
    if not windows:
        raise Refused(f"{folder} has no window dumps")
    differs = [window for window in windows if compare_window(folder, window, image, tolerance)]
    return 1 if differs else 0


def last_posts(path):
    """Each window's last post: (post, size, format, CRC) or (post, 'hidden')."""
    posts = {}
    for line in open(path, errors="replace"):
        match = POST.search(line)
        if match:
            window, post, w, h, fmt, crc = match.groups()
            posts[window] = (int(post), f"{w}x{h}", fmt, crc) if crc else (int(post), "hidden")
    return posts


def posts(log, reference):
    mine, theirs = last_posts(log), last_posts(reference)
    if not mine:
        raise Refused(f"{log} has no CRC lines")
    status = 0
    for window in sorted(mine, key=int):
        have, want = mine[window], theirs.get(window)
        same = want is not None and have[1:] == want[1:]
        theirs_text = "none" if want is None else f"{want[0]} {' '.join(want[1:])}"
        print(f"window {window}: last post {have[0]} {' '.join(have[1:])}; reference {theirs_text}: "
              f"{'equal' if same else 'DIFFERS'}")
        status = status or (0 if same else 1)
    return status


def main(arguments):
    try:
        if len(arguments) >= 3 and arguments[0] == "screen":
            tolerance = int(arguments[4]) if len(arguments) == 5 and arguments[3] == "--tolerance" else 3
            return screen(arguments[1], arguments[2], tolerance)
        if len(arguments) == 3 and arguments[0] == "posts":
            return posts(arguments[1], arguments[2])
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    except (OSError, Refused, zlib.error) as error:
        print(f"window_pixels.py: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
