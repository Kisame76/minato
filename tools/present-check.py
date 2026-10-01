#!/usr/bin/env python3
"""ADR-0041 test 3 on the iPhone: whether present-test's frames reached the screen at the times they asked for.

  present-check.py <present.csv> <drawables.csv>

present.csv is present-test's (internalDataPath, fetched from Minato's data container): a row per frame in the order
of its presents, with the time each asked for (asked_ns, the guest's CLOCK_MONOTONIC). drawables.csv is the App's
recording of the same session (tools/record-session.sh): row k is the drawable of frame k, with the time Core
Animation presented it (presented_ns, CLOCK_UPTIME_RAW, the same clock by F10; 0 for one that never reached the
screen). Decision 11's tolerance: a frame is presented no earlier than 1 ms before its time and no later than one
refresh period of its rate after it, for 99 % of the 600 frames at 60 and at 120 Hz.

Per rate, and per block of 1, 2 and 3 periods on, it prints the frames in tolerance, early, late and never presented,
and how far the presents lay from the times asked for (p50 and p99 by nearest rank, min and max). For Vulkan it also
compares the past timings present-test got (actual_ns) with the recording. Exit status: 0 when both rates pass, 1
when one fails, 2 when the files cannot be matched."""
import csv
import math
import pathlib
import sys

MS = 1_000_000
RATES = (60, 120)
FRAMES_PER_RATE = 600
EARLIEST_NS = -MS  # no earlier than 1 ms before the time asked for


class Refused(Exception):
    pass


def read(path, columns):
    with open(path, newline="") as handle:
        reader = csv.DictReader(handle)
        missing = [column for column in columns if column not in (reader.fieldnames or [])]
        if missing:
            raise Refused(f"{path}: no column {', '.join(missing)}")
        return list(reader)


def nearest_rank(values, percent):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(percent / 100 * len(ordered)) - 1)]


def ms(nanoseconds):
    return f"{nanoseconds / MS:+.3f}"


def spread(deltas):
    if not deltas:
        return "none presented"
    return (f"presented - asked p50 {ms(nearest_rank(deltas, 50))} ms, p99 {ms(nearest_rank(deltas, 99))} ms, "
            f"min {ms(min(deltas))}, max {ms(max(deltas))}")


def judge(frames):
    """(in tolerance, early, late, not presented, deltas) over (rate, asked_ns, presented_ns) of `frames`."""
    inside = early = late = never = 0
    deltas = []
    for rate, asked, presented in frames:
        if presented == 0:
            never += 1
            continue
        delta = presented - asked
        deltas.append(delta)
        if delta < EARLIEST_NS:
            early += 1
        elif delta > 1e9 / rate:
            late += 1
        else:
            inside += 1
    return inside, early, late, never, deltas


def main():
    if len(sys.argv) != 3:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        presents = read(sys.argv[1], ("api", "rate", "interval", "frame", "asked_ns", "actual_ns"))
        drawables = read(sys.argv[2], ("frame", "presented_ns"))
        if len(drawables) != len(presents):
            raise Refused(f"{len(presents)} frames but {len(drawables)} drawables: the recording does not match the "
                          f"presents one to one")
        for index, (row, drawable) in enumerate(zip(presents, drawables)):
            if int(row["frame"]) != index:
                raise Refused(f"present.csv: frame {row['frame']} in row {index}")
            if int(drawable["frame"]) != index:
                raise Refused(f"drawables.csv: frame {drawable['frame']} in row {index}")
        for rate in RATES:
            count = sum(1 for row in presents if int(row["rate"]) == rate)
            if count != FRAMES_PER_RATE:
                raise Refused(f"rate {rate}: {count} frames, not {FRAMES_PER_RATE}")
    except (OSError, ValueError, Refused) as error:
        print(f"present-check: refused: {error}")
        return 2

    api = presents[0]["api"]
    print(f"present-check: {api}, {len(presents)} frames, {len(drawables)} drawables")
    passed = True
    for rate in RATES:
        rows = [(row, drawable) for row, drawable in zip(presents, drawables) if int(row["rate"]) == rate]
        inside, early, late, never, deltas = judge(
            (rate, int(row["asked_ns"]), int(drawable["presented_ns"])) for row, drawable in rows)
        share = 100 * inside / len(rows)
        print(f"rate {rate}: {len(rows)} frames, {inside} in tolerance ({share:.1f} %), early {early}, late {late}, "
              f"not presented {never}; {spread(deltas)}")
        for interval in (1, 2, 3):
            block = [(row, drawable) for row, drawable in rows if int(row["interval"]) == interval]
            b_inside, b_early, b_late, b_never, b_deltas = judge(
                (rate, int(row["asked_ns"]), int(drawable["presented_ns"])) for row, drawable in block)
            print(f"  {interval} period{'s' if interval > 1 else ''} on: {len(block)} frames, {b_inside} in "
                  f"tolerance, early {b_early}, late {b_late}, not presented {b_never}; {spread(b_deltas)}")
        passed = passed and inside * 100 >= 99 * len(rows)
    if api == "vulkan":
        # The past timings present-test got, against the recording, where both have the frame.
        pairs = [int(row["actual_ns"]) - int(drawable["presented_ns"]) for row, drawable in zip(presents, drawables)
                 if int(row["actual_ns"]) != 0 and int(drawable["presented_ns"]) != 0]
        if pairs:
            print(f"vulkan's past timings: {len(pairs)} of {len(presents)} frames; actual - recorded p50 "
                  f"{ms(nearest_rank(pairs, 50))} ms, min {ms(min(pairs))}, max {ms(max(pairs))}")
        else:
            print(f"vulkan's past timings: none of {len(presents)} frames")
    print(f"present-check: {'pass' if passed else 'fail'}: decision 11's tolerance for 99 % of each rate's frames")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
