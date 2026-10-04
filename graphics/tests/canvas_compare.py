#!/usr/bin/env python3
"""ADR-0043 test 2's pixel comparison: canvas-test's pixels on the Mac against Android 15's, both raw RGBA bytes of
the same size. Equal bytes pass; otherwise a channel may differ by 1 in 255, and at most 0.1 % of the pixels may
differ by more in any channel [guess, the ADR's: another compiler for the same Skia]. canvas_compare_test.py checks
both limits.

Usage: canvas_compare.py <expected .rgba.gz> <actual .rgba> <width> <height>
"""
import gzip
import hashlib
import sys

TOLERANCE = 1
SHARE = 0.001


def compare(expected, actual, width, height):
    """(passed, text) for two RGBA byte strings of width x height pixels."""
    size = width * height * 4
    if len(expected) != size or len(actual) != size:
        return False, f"{len(expected)} and {len(actual)} bytes, not the {size} of {width} x {height} RGBA pixels"
    if expected == actual:
        return True, f"equal pixels, SHA-256 {hashlib.sha256(actual).hexdigest()}"
    differing = largest = 0
    for start in range(0, size, 4):
        delta = max(abs(a - b) for a, b in zip(expected[start:start + 4], actual[start:start + 4]))
        largest = max(largest, delta)
        if delta > TOLERANCE:
            differing += 1
    limit = int(width * height * SHARE)
    return differing <= limit, (f"{differing} of {width * height} pixels differ by more than {TOLERANCE} in 255 (at "
                                f"most {limit} may), the most by {largest}")


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    with gzip.open(sys.argv[1]) as handle:
        expected = handle.read()
    with open(sys.argv[2], "rb") as handle:
        actual = handle.read()
    passed, text = compare(expected, actual, int(sys.argv[3]), int(sys.argv[4]))
    print(("" if passed else "FAIL: ") + text)
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
