#!/usr/bin/env python3
"""The renpy-memory-test footprint summary of tools/java-start-check.sh: the minimum, maximum and final physical
footprint of the run in MB, its memory-warning count and the onLowMemory calls the guest got, as a CSV line. The
call count is the caller's (java-start-check counts the session's events): the bridge passes one warning a minute on,
so it can be lower than the warning count; without it the field is empty. A run that produced no footprint sample is a
device fetch or timing artifact (the sampler writes a line at once and every 10 s, so an empty log means the fetch
raced the run's end), not a leak, so it is a note, not a failure; the real gate on the run is "game ended before N
min" in java-start-check.sh, not this summary.
With --lived and the run's minutes, it says whether the process lived the whole run: the sampler writes a line at
once and every 10 s while the process runs and nothing once iOS has killed it, and a jetsam kill writes no event of
Minato's, so the run lived when its lines span the minutes less 30 s. Else it prints "the game ended before N min
(killed: last footprint X MB after S s)", the last line's footprint (a warning's included), and exits 1. A log without
a footprint line is the same note as above.
Runs on /usr/bin/python3 (3.9).
Usage: tools/renpy_footprint.py <footprint-log> [<onLowMemory calls>]
       tools/renpy_footprint.py --lived <footprint-log> <minutes>
"""
import sys


def samples(path):
    """(footprints in MB, memory-warning count) from a footprint-log; the list is empty when it has no footprint."""
    footprints, warnings = [], 0
    try:
        for line in open(path, encoding="utf-8"):
            parts = line.split()
            if len(parts) != 3:
                continue
            _, kind, value = parts
            if kind == "footprint" and int(value) >= 0:
                footprints.append(int(value) / 1048576)
            elif kind == "warning":
                warnings += 1
    except OSError:
        pass
    return footprints, warnings


def lines(path):
    """(seconds since 1970, bytes) of every footprint and warning line, in the file's order."""
    found = []
    try:
        for line in open(path, encoding="utf-8"):
            parts = line.split()
            if len(parts) == 3 and parts[1] in ("footprint", "warning") and int(parts[2]) >= 0:
                found.append((float(parts[0]), int(parts[2])))
    except (OSError, ValueError):
        pass
    return found


def lived(path, minutes):
    """0 when the log's lines span `minutes` less 30 s, else 1 and the kill's line; a note without a footprint."""
    if not samples(path)[0]:
        print("note  renpy-memory-test: no footprint sample (skipped)")
        return 0
    found = lines(path)
    span = found[-1][0] - found[0][0]
    if span >= minutes * 60 - 30:
        return 0
    print("the game ended before {} min (killed: last footprint {:.0f} MB after {:.0f} s)".format(
        minutes, found[-1][1] / 1048576, int(span)))
    return 1


def main(argv):
    if len(argv) == 4 and argv[1] == "--lived":
        return lived(argv[2], int(argv[3]))
    footprints, warnings = samples(argv[1])
    calls = argv[2] if len(argv) > 2 else ""
    if not footprints:
        # A device fetch or timing artifact, not a leak: a note, not a failure. The game-ended check is the real gate.
        print("note  renpy-memory-test: no footprint sample (skipped)")
        return 0
    print("csv,footprint_min_mb,footprint_max_mb,footprint_end_mb,memory_warnings,on_low_memory\n"
          "csv,{:.0f},{:.0f},{:.0f},{},{}".format(min(footprints), max(footprints), footprints[-1], warnings, calls))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
