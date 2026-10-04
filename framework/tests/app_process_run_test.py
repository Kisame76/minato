#!/usr/bin/env python3
"""framework/tests/app_process_run.py's convert() (ADR-0039 step c1): a container is made again unless it was made
from the very ELF file given, by its real path, size and time, so a folder pointed at an older build gets that
build's containers. Run with a stand-in for apk-convert in a scratch directory."""
import os
import stat
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app_process_run  # noqa: E402

failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


with tempfile.TemporaryDirectory() as scratch:
    calls = os.path.join(scratch, "calls")
    # apk-convert's stand-in: "convert <elf> <container> ..." copies the ELF and notes the call.
    fake = os.path.join(scratch, "apk-convert")
    with open(fake, "w") as handle:
        handle.write(f'#!/bin/sh\ncp "$2" "$3" && echo "$2" >> "{calls}"\n')
    os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)

    def calls_made():
        return open(calls).read().splitlines() if os.path.exists(calls) else []

    def elf(folder, text, mtime):
        path = os.path.join(scratch, folder, "libx.so")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as handle:
            handle.write(text)
        os.utime(path, (mtime, mtime))
        return path

    container = os.path.join(scratch, "lib64", "libx.so.dylib")
    newer = elf("newer", "the newer build", 2_000_000_000)
    older = elf("older", "the older build", 1_000_000_000)

    app_process_run.convert(fake, newer, container)
    check(open(container).read() == "the newer build" and len(calls_made()) == 1, "the first start converts")
    app_process_run.convert(fake, newer, container)
    check(len(calls_made()) == 1, "the same file again keeps the container")

    # The stale copy: the folder points at an older build, whose file is older than the container.
    app_process_run.convert(fake, older, container)
    check(open(container).read() == "the older build", "an older build's file replaces the newer container")

    # The same path with other contents of another size, at the same time.
    with open(older, "w") as handle:
        handle.write("the older build, rebuilt")
    os.utime(older, (1_000_000_000, 1_000_000_000))
    app_process_run.convert(fake, older, container)
    check(open(container).read() == "the older build, rebuilt", "a file of another size is converted again")

    # A container without its stamp, as a start before the stamps left it.
    os.remove(os.path.join(scratch, "lib64", ".from", "libx.so.dylib"))
    count = len(calls_made())
    app_process_run.convert(fake, older, container)
    check(len(calls_made()) == count + 1, "a container without its stamp is converted again")

if failures:
    sys.exit(f"app_process_run_test: {failures} failed")
print("app_process_run_test: convert() fine")
