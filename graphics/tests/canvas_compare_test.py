#!/usr/bin/env python3
"""graphics.canvas_compare: canvas_compare.py's verdict at both limits of ADR-0043 test 2's tolerance, on canvas-test's
480 x 480 pixels (230,400): a channel may differ by 1 in 255, and at most 0.1 % of the pixels, 230, by more."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from canvas_compare import compare  # noqa: E402

WIDTH = HEIGHT = 480
PIXELS = WIDTH * HEIGHT
failures = []


def picture():
    """Deterministic RGBA bytes with every channel value, away from 0 and 255 so that a change of 2 fits."""
    return bytearray((index * 7 + 3) % 250 + 3 for index in range(PIXELS * 4))


def changed(pixels, delta, step=1, channel=0):
    """A copy with `pixels` pixels, every `step`th from the first, changed by `delta` in one channel."""
    data = picture()
    for pixel in range(0, pixels * step, step):
        data[pixel * 4 + channel] += delta
    return bytes(data)


def expect(name, actual, passed):
    verdict, text = compare(bytes(picture()), actual, WIDTH, HEIGHT)
    if verdict != passed:
        failures.append(f"{name}: {'passed' if verdict else 'failed'} ({text}), expected to "
                        f"{'pass' if passed else 'fail'}")


expect("equal", bytes(picture()), True)
expect("every pixel off by 1", changed(PIXELS, 1), True)
expect("every pixel off by -1 in alpha", changed(PIXELS, -1, channel=3), True)
expect("one pixel off by 2", changed(1, 2), True)
expect("230 pixels off by 2", changed(230, 2, step=997), True)
expect("231 pixels off by 2", changed(231, 2, step=997), False)
expect("231 pixels off by -2 in blue", changed(231, -2, step=997, channel=2), False)
expect("231 pixels off by 2 in alpha", changed(231, 2, step=997, channel=3), False)
expect("one byte short", bytes(picture())[:-1], False)

if failures:
    print("\n".join("FAIL: " + failure for failure in failures))
    sys.exit(1)
print("canvas_compare passes 1 in 255 everywhere and 0.1 % of the pixels beyond it, and fails one pixel more")
