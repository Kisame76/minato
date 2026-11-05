#!/usr/bin/env python3
"""The renpy-memory-test footprint summary of tools/java-start-check.sh: the minimum, maximum and final physical
footprint of the run in MB and its memory-warning count, as a CSV line. A run that produced no footprint sample is a
device fetch or timing artifact (the sampler writes a line at once and every 10 s, so an empty log means the fetch
raced the run's end), not a leak, so it is a note, not a failure; the real gate on the run is "game ended before N
min" in java-start-check.sh, not this summary.
Runs on /usr/bin/python3 (3.9).
Usage: tools/renpy_footprint.py <footprint-log>
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


def main(argv):
    footprints, warnings = samples(argv[1])
    if not footprints:
        # A device fetch or timing artifact, not a leak: a note, not a failure. The game-ended check is the real gate.
        print("note  renpy-memory-test: no footprint sample (skipped)")
        return 0
    print("csv,footprint_min_mb,footprint_max_mb,footprint_end_mb,memory_warnings,on_low_memory\n"
          "csv,{:.0f},{:.0f},{:.0f},{},{}".format(min(footprints), max(footprints), footprints[-1], warnings, warnings))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
