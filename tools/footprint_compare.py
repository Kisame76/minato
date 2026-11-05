#!/usr/bin/env python3
"""The footprint comparison of tools/window-contents-check.sh (ADR-0043 b5, step f): the peak physical footprint of
views-test by contents and by drawables, the changelog's memory number. A run whose footprint-log has no sample is a
device fetch or timing artifact (the sampler writes a line at once and every 10 s, so an empty log means the fetch
raced the run's end, not a leak), not a correctness regression: it is a skip with a note, since the memory is a
measurement, not a gate on the pixel round.
Runs on /usr/bin/python3 (3.9).
Usage: tools/footprint_compare.py <contents-footprint-log> <drawables-footprint-log>
"""
import sys


def peak(path):
    """The largest footprint sample in bytes in a footprint-log, or None when the file is missing or has no sample."""
    try:
        values = [int(line.split()[2]) for line in open(path) if line.split()[1:2] == ["footprint"]]
    except OSError:
        return None
    return max(values) if values else None


def main(argv):
    contents, drawables = peak(argv[1]), peak(argv[2])
    if contents is None or drawables is None:
        # A device fetch or timing artifact, not a leak: the memory is a measurement, not a gate on the pixel round.
        print("note  views-test footprint skipped (no sample in one run)")
        return 0
    print("note  views-test's footprint at its peak: {:.1f} MB by contents, {:.1f} MB by drawables, {:.1f} MB less "
          "by contents".format(contents / 1e6, drawables / 1e6, (drawables - contents) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
