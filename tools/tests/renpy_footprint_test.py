#!/usr/bin/env python3
"""tools/renpy_footprint.py (java-start-check.sh's renpy-memory-test summary):
  - A run with footprint samples: the CSV line of min, max, end footprint, the memory-warning count (warning lines
    counted, not treated as footprints) and the onLowMemory calls the guest got, exit 0. That last count is
    java-start-check's, from the session's events: the bridge passes one warning a minute on, so two warnings at once
    give one call (Simulator round of 2026-11-11: 3 warnings, 2 calls, and the column said 3). Without it the field
    is empty, never the warning count.
  - A run with no footprint sample (the file is missing, empty, or has only warning lines): a device fetch or timing
    artifact, not a leak, so a note and exit 0, not a failure; the game-ended check stays the real gate.
  - --lived <log> <minutes>: whether the process lived the whole run. The sampler writes a footprint line at once and
    every 10 s while the process runs, and nothing once iOS has killed it (jetsam writes no event of Minato's), so the
    run lived when its lines span the minutes less 30 s; else exit 1 and "the game ended before N min (killed: last
    footprint X MB after S s)", with the last line's footprint, a warning's included. iPhone, 2026-11-10: the build
    without texture parking was killed ~15 s into a 5 min run, and the round still said "played for 5 min".
Runs on /usr/bin/python3 (3.9).
Usage: tools/tests/renpy_footprint_test.py
"""
import importlib.util
import io
import os
import sys
import tempfile
from contextlib import redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location("renpy_footprint", os.path.join(ROOT, "tools", "renpy_footprint.py"))
renpy_footprint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(renpy_footprint)


def run(path, *options):
    out = io.StringIO()
    with redirect_stdout(out):
        # renpy_footprint.py <log> [<onLowMemory calls>], or renpy_footprint.py --lived <log> <minutes>.
        if options and options[0] == "--lived":
            argv = ["renpy_footprint.py", options[0], path, *options[1:]]
        else:
            argv = ["renpy_footprint.py", path, *options]
        code = renpy_footprint.main(argv)
    return code, out.getvalue()


def write(directory, name, lines):
    path = os.path.join(directory, name)
    with open(path, "w") as handle:
        handle.write("".join(line + "\n" for line in lines))
    return path


failures = 0


def check(condition, message):
    global failures
    if not condition:
        print("FAIL  " + message)
        failures += 1


with tempfile.TemporaryDirectory() as directory:
    # Samples present: min/max/end in MB (1048576 bytes = 1 MB here) and the warning count; warning lines not footprints.
    log = write(directory, "footprint-log", ["1730800000.000 footprint 104857600",    # 100 MB
                                             "1730800010.000 footprint 209715200",    # 200 MB
                                             "1730800020.000 warning 209715200",
                                             "1730800030.000 footprint 157286400"])   # 150 MB (end)
    code, text = run(log)
    check(code == 0, "a run with samples should pass (exit 0), got {}".format(code))
    check("csv,100,200,150,1,\n" in text, "the CSV should be min 100, max 200, end 150, 1 warning, no call count: "
          + text.strip())

    # Two warnings at once and the guest's one onLowMemory: the columns differ.
    pair = write(directory, "pair", ["1730800000.000 footprint 104857600",
                                     "1730800010.000 warning 104857600",
                                     "1730800010.100 warning 104857600"])
    code, text = run(pair, "1")
    check(code == 0, "a run with a call count should pass (exit 0), got {}".format(code))
    check("csv,100,100,100,2,1\n" in text, "two warnings and one onLowMemory should read 2,1: " + text.strip())

    # No footprint line at all (only a warning): a note, not a failure.
    only_warning = write(directory, "only-warning", ["1730800000.000 warning 209715200"])
    code, text = run(only_warning)
    check(code == 0, "a sampleless log must be a note, not a failure (exit 0), got {}".format(code))
    check("no footprint sample" in text, "a sampleless log should print the note: " + text.strip())

    # A missing file (the device fetch raced the run's end): the same note.
    code, text = run(os.path.join(directory, "absent"))
    check(code == 0, "a missing log must be a note, not a failure (exit 0), got {}".format(code))
    check("no footprint sample" in text, "a missing log should print the note: " + text.strip())

    # --lived: the iPhone's red run of 2026-11-10 (5 min asked): 24 MB at the start, 3.5 GB at +10 s, five memory
    # warnings at 5.0-5.4 GB at +14 s, then nothing: jetsam killed the process ~15 s in.
    MB = 1048576
    killed = write(directory, "killed", ["1730800000.000 footprint {}".format(24 * MB),
                                         "1730800010.000 footprint {}".format(3584 * MB)]
                   + ["1730800014.{:03d} warning {}".format(i, (5120 + 70 * i) * MB) for i in range(5)])
    code, text = run(killed, "--lived", "5")
    check(code == 1, "a run killed 15 s into 5 min must fail (exit 1), got {}".format(code))
    check("the game ended before 5 min (killed: last footprint 5400 MB after 14 s)" in text,
          "the kill should name the last footprint and when: " + text.strip())
    # A run that lived: a line every 10 s for the 5 minutes.
    lived = write(directory, "lived", ["{}.000 footprint {}".format(1730800000 + 10 * i, 4800 * MB)
                                       for i in range(30)])
    code, text = run(lived, "--lived", "5")
    check(code == 0, "a run with lines over the 5 minutes must pass (exit 0), got {}: {}".format(code, text.strip()))
    # Its log fetched a little early (the last line at +280 s): still lived, within the 30 s.
    early = write(directory, "early", ["{}.000 footprint {}".format(1730800000 + 10 * i, 4800 * MB)
                                       for i in range(29)])
    code, text = run(early, "--lived", "5")
    check(code == 0, "the last line 20 s before the end is no kill: " + text.strip())
    # No line at all: a note, as for the summary; the kill needs a line to tell.
    code, text = run(os.path.join(directory, "absent"), "--lived", "5")
    check(code == 0 and "no footprint sample" in text, "a missing log under --lived must be a note: " + text.strip())

if failures:
    print("renpy_footprint_test: {} FAILED".format(failures))
    sys.exit(1)
print("renpy_footprint_test: ok")
