#!/usr/bin/env python3
"""tools/renpy_footprint.py (java-start-check.sh's renpy-memory-test summary):
  - A run with footprint samples: the CSV line of min, max, end footprint and the memory-warning count (warning lines
    counted, not treated as footprints), exit 0.
  - A run with no footprint sample (the file is missing, empty, or has only warning lines): a device fetch or timing
    artifact, not a leak, so a note and exit 0, not a failure; the game-ended check stays the real gate.
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


def run(path):
    out = io.StringIO()
    with redirect_stdout(out):
        code = renpy_footprint.main(["renpy_footprint.py", path])
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
    check("csv,100,200,150,1,1" in text, "the CSV should be min 100, max 200, end 150, 1 warning: " + text.strip())

    # No footprint line at all (only a warning): a note, not a failure.
    only_warning = write(directory, "only-warning", ["1730800000.000 warning 209715200"])
    code, text = run(only_warning)
    check(code == 0, "a sampleless log must be a note, not a failure (exit 0), got {}".format(code))
    check("no footprint sample" in text, "a sampleless log should print the note: " + text.strip())

    # A missing file (the device fetch raced the run's end): the same note.
    code, text = run(os.path.join(directory, "absent"))
    check(code == 0, "a missing log must be a note, not a failure (exit 0), got {}".format(code))
    check("no footprint sample" in text, "a missing log should print the note: " + text.strip())

if failures:
    print("renpy_footprint_test: {} FAILED".format(failures))
    sys.exit(1)
print("renpy_footprint_test: ok")
