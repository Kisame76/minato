#!/usr/bin/env python3
"""tools/footprint_compare.py (ADR-0043 b5, step f): the peak physical footprint of views-test by contents and by
drawables, and the skip of a run that produced no sample.
  - Two runs with samples: the note names each run's peak footprint (warning lines ignored), exit 0.
  - A run with no sample (the file is missing, empty, or has only warning lines): a device fetch or timing artifact,
    not a leak, so a skip note and exit 0, never a failure of the pixel round.
Runs on /usr/bin/python3 (3.9).
Usage: tools/tests/footprint_compare_test.py
"""
import importlib.util
import io
import os
import sys
import tempfile
from contextlib import redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location("footprint_compare", os.path.join(ROOT, "tools", "footprint_compare.py"))
footprint_compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(footprint_compare)


def run(contents_path, drawables_path):
    """main() with its stdout captured; returns (exit code, stdout)."""
    out = io.StringIO()
    with redirect_stdout(out):
        code = footprint_compare.main(["footprint_compare.py", contents_path, drawables_path])
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
    # Two runs with samples: the peak of each, warning lines left out of the maximum.
    contents = write(directory, "contents", ["1730800000.000 footprint 100000000",
                                              "1730800010.000 footprint 120000000",
                                              "1730800020.000 warning 900000000"])
    drawables = write(directory, "drawables", ["1730800000.000 footprint 150000000",
                                               "1730800010.000 footprint 170000000"])
    code, text = run(contents, drawables)
    check(code == 0, "two runs with samples should pass (exit 0), got {}".format(code))
    check("120.0 MB by contents" in text, "the peak by contents should be 120 MB (warning ignored): " + text.strip())
    check("170.0 MB by drawables" in text, "the peak by drawables should be 170 MB: " + text.strip())

    # A missing drawables log (the device fetch raced the run's end): a skip note, not a failure.
    code, text = run(contents, os.path.join(directory, "absent"))
    check(code == 0, "a missing sample must be a skip, not a failure (exit 0), got {}".format(code))
    check("skipped (no sample in one run)" in text, "a missing sample should print the skip note: " + text.strip())

    # A drawables log with only a warning line (no footprint sample): the same skip.
    only_warning = write(directory, "only-warning", ["1730800000.000 warning 900000000"])
    code, text = run(contents, only_warning)
    check(code == 0, "a sampleless log must be a skip, not a failure (exit 0), got {}".format(code))
    check("skipped (no sample in one run)" in text, "a sampleless log should print the skip note: " + text.strip())

if failures:
    print("footprint_compare_test: {} FAILED".format(failures))
    sys.exit(1)
print("footprint_compare_test: ok")
