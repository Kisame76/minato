#!/usr/bin/env python3
"""Ren'Py's memory on the Mac (tools/renpy-memory.sh): Minato's Ren'Py memory game (fixtures/renpy/minato-renpy-memory),
which shows many distinct full-screen backgrounds one after the other as a visual novel does, through the Java path as
framework/tests/renpy_run.py starts the test game, with the process's footprint sampled every <seconds> s with the
system's footprint tool: the total and the dirty bytes of each category (malloc zones, the guest's mappings, the
IOSurface and Metal memory ANGLE holds, ...). The samples go to <csv> as seconds,total,<category>... and a summary of
the first, the middle and the last to stdout. The run ends 2 s after the game's line "minato-renpy-memory: done".

Usage: renpy_memory.py <scenes> <seconds> <csv> <app> <package> <art> <bionic-lib> <bionic-system> <hwui-lib>
                       <icu-data> <apk-convert> <graphics-run> <work> <app_process64> <framework.jar>
                       <framework-res folder> <app.apk>
"""
import csv
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

from app_process_run import missing, start

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PERMISSIONS = os.path.join(ROOT, "build", "sources", "frameworks_base_etc", "data", "etc")
LAST = r"^([VDIWEF])/python *: minato-renpy-memory: done$"


def process_of(work):
    """The pid of the run's apk-run, found by its data mount below `work`."""
    found = subprocess.run(["pgrep", "-n", "-f", "--", f"--mount /data={work}/run/data"], capture_output=True,
                           text=True)
    return int(found.stdout.split()[0]) if found.stdout.split() else None


def footprint(pid):
    """The process's footprint in bytes and the dirty bytes of each category, None when it is gone."""
    with tempfile.NamedTemporaryFile(suffix=".json") as out:
        done = subprocess.run(["footprint", "-p", str(pid), "-f", "bytes", "-j", out.name], capture_output=True)
        if done.returncode != 0:
            return None
        try:
            process = json.load(open(out.name, encoding="utf-8"))["processes"][0]
        except (ValueError, KeyError, IndexError):
            return None
    return process["footprint"], {name: values["dirty"] for name, values in process["categories"].items()}


def main():
    args = sys.argv[1:]
    if len(args) != 17:
        raise SystemExit(__doc__)
    (scenes, seconds, table, app, package, art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert,
     graphics_run, work, app_process, framework_jar, folder, apk) = args
    skipped = missing(art, bionic_system, hwui_lib, app_process, framework_jar, apk, app)
    if skipped:
        print(skipped)
        return
    mounts = [("/system/framework/framework-res", folder)]
    if os.path.isfile(os.path.join(PERMISSIONS, "platform.xml")):
        mounts.append(("/system/etc/permissions", PERMISSIONS))
    samples, stop = [], threading.Event()
    begin = time.time()

    def sample():
        while not stop.wait(float(seconds)):
            pid = process_of(work)
            taken = footprint(pid) if pid else None
            if taken:
                samples.append((round(time.time() - begin), *taken))

    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()
    result = start(art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, graphics_run, work, app_process,
                   framework_jar, apk, package, None, mounts=mounts, configuration="2796x1290@3,en-US", last=LAST,
                   environment=(f"MINATO_SCENES={scenes}",), timeout=3600)
    stop.set()
    sampler.join()
    categories = sorted({name for _, _, row in samples for name in row})
    os.makedirs(os.path.dirname(os.path.abspath(table)), exist_ok=True)
    with open(table, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["seconds", "total", *categories])
        for moment, total, row in samples:
            writer.writerow([moment, total, *[row.get(name, 0) for name in categories]])
    if result is not None:
        lines = [line for line in result.stderr.splitlines() if "minato-renpy-memory" in line]
        print(f"exit status {result.returncode}; the game logged {len(lines)} lines, the last: {lines[-1:]}")
    if result is None:
        print("FAIL: no line \"minato-renpy-memory: done\" within the time")
    if not samples:
        print("FAIL: no sample")
        sys.exit(1)
    for label, index in (("first", 0), ("middle", len(samples) // 2), ("last", len(samples) - 1)):
        moment, total, row = samples[index]
        top = sorted(row.items(), key=lambda item: -item[1])[:4]
        print(f"{label}: {moment} s, {total // 1048576} MiB; " + ", ".join(f"{n} {v // 1048576}" for n, v in top))
    print(f"{len(samples)} samples in {table}")


if __name__ == "__main__":
    main()
