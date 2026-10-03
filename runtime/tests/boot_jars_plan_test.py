#!/usr/bin/env python3
"""tools/art-plan.py writes boot_jars.json, which runtime/art/boot_jars.py builds the boot jars from, only when its
content changes. CMake plans again whenever it configures ART's build, and the boot jars depend on the file, so a
rewrite with the same content built them all again. Plans twice into <work>: the second plan must leave the file
alone, and a plan over a changed file must write the planned content again. Prints "skipped: ..." without ART's
sources.

Usage: boot_jars_plan_test.py <art-plan.py> <sources> <work>
"""
import os
import shutil
import subprocess
import sys

OLD = 1000000000  # a modification time no plan writes


def plan(planner, sources, work):
    result = subprocess.run([sys.executable, planner, sources, os.path.join(work, "plan.cmake")],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"FAIL: art-plan failed:\n{result.stdout}{result.stderr}")


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    planner, sources, work = sys.argv[1:]
    if not os.path.isdir(os.path.join(sources, "art")):
        print(f"skipped: no ART sources in {sources}")
        return
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    description = os.path.join(work, "boot_jars.json")
    plan(planner, sources, work)
    with open(description) as file:
        planned = file.read()

    failures = []
    os.utime(description, (OLD, OLD))
    plan(planner, sources, work)
    if os.stat(description).st_mtime != OLD:
        failures.append("a second plan with the same inputs wrote boot_jars.json again, so the boot jars build again")
    with open(description, "w") as file:
        file.write("{}\n")
    os.utime(description, (OLD, OLD))
    plan(planner, sources, work)
    with open(description) as file:
        if file.read() != planned:
            failures.append("a plan over a changed boot_jars.json did not write the planned content")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        sys.exit(1)
    print("art-plan writes boot_jars.json only when its content changes")


if __name__ == "__main__":
    main()
