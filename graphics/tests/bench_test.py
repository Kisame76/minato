#!/usr/bin/env python3
"""ADR-0037 a): graphics.bench. For each API and load, the parity benchmark's scene built for the host
(graphics-bench-host) and as its APK through activity-host (graphics-app-test's bench mode) draw a run of 9 s at
120 Hz with ticks scripted in lockstep; the APK's ticks after its first frame come 0.5 or 1 ms late, so a scene
that moved with the ticks rather than the frame index would draw other frames. It passes when both runs pass,
both logged the same CRC of frame 1000 and a CRC per load differs from the other load's, and each run's scene.csv
has a row per frame: the indices 0 to 1079 in order, the ticks rising, the CPU times above 0. It prints
"skipped:" when a program says so.

With --record, ADR-0037 b): graphics.recorder. For each API, both builds draw the light load for 1 s, 120 frames, on
graphics/host's layer, which records the run; each program checks in-process that the recording holds every drawable
once and in order (recording_check.h) and prints a "recorder:" line with the layer's notes."""
import argparse
import pathlib
import re
import subprocess
import sys

FRAMES = 1080
CRC = re.compile(r"crc frame 1000: ([0-9a-f]{8})\b")

failures = []


def fail(what):
    print(f"FAIL: {what}", flush=True)
    failures.append(what)


def run(command, what):
    """The program's output; a program that says "skipped:" ends the test as skipped."""
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                            timeout=300)
    skipped = [line for line in result.stdout.splitlines() if line.startswith("skipped:")]
    if skipped:
        print(skipped[0])
        sys.exit(0)
    if result.returncode != 0:
        tail = "\n".join(result.stdout.splitlines()[-25:])
        fail(f"{what} ended with {result.returncode}:\n{tail}")
    return result.stdout


def crc(output, what):
    found = CRC.findall(output)
    if len(found) != 1:
        fail(f"{what}: {len(found)} CRC lines of frame 1000, not 1")
        return None
    return found[0]


def check_scene(path, what):
    try:
        lines = path.read_text().splitlines()
    except OSError as error:
        fail(f"{what}: no scene.csv ({error})")
        return
    if not lines or lines[0] != "index,tick_ns,cpu_ns":
        fail(f"{what}: scene.csv lacks its header")
        return
    rows = [line.split(",") for line in lines[1:]]
    if len(rows) != FRAMES:
        fail(f"{what}: scene.csv has {len(rows)} rows, not {FRAMES}")
        return
    ticks = 0
    for number, row in enumerate(rows):
        if len(row) != 3 or not all(re.fullmatch(r"-?[0-9]+", field) for field in row):
            fail(f"{what}: scene.csv row {number} is {','.join(row)}")
            return
        index, tick, cpu = (int(field) for field in row)
        if index != number or tick <= ticks or cpu <= 0:
            fail(f"{what}: scene.csv row {number} is {','.join(row)} after tick {ticks}")
            return
        ticks = tick


def recorder_line(output, what):
    lines = [line for line in output.splitlines() if line.startswith("recorder: ")]
    if len(lines) != 1:
        fail(f"{what}: {len(lines)} recorder lines, not 1")
        return
    print(lines[0], flush=True)


def record(args, scratch):
    for api in ("gles", "vulkan"):
        host = run([args.host, api, "light", str(scratch / "record-host"), args.moltenvk, "record"],
                   f"{api}, host build")
        recorder_line(host, f"{api}, host build")
        guest = run([args.app_test, str(scratch / "record-guest"), f"bench-{api}", args.bench, args.bionic,
                     args.system, args.front, args.manifests, args.angle, args.moltenvk, "record:light"], f"{api}, APK")
        recorder_line(guest, f"{api}, APK")
    print(f"{len(failures)} failures" if failures else "PASS: graphics.recorder")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser()
    for option in ("host", "app-test", "scratch", "bench", "bionic", "system", "front", "manifests", "angle",
                   "moltenvk"):
        parser.add_argument(f"--{option}", required=True)
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    scratch = pathlib.Path(args.scratch)
    if args.record:
        return record(args, scratch)

    for api in ("gles", "vulkan"):
        crcs = {}
        for load in ("light", "heavy"):
            name = f"{api} {load}"
            host = run([args.host, api, load, str(scratch / "host"), args.moltenvk], f"{name}, host build")
            check_scene(scratch / "host" / f"{api}-{load}" / "scene.csv", f"{name}, host build")
            app = f"bench-{api}"
            guest = run([args.app_test, str(scratch / "guest"), app, args.bench, args.bionic, args.system,
                         args.front, args.manifests, args.angle, args.moltenvk, f"bench:{load}"], f"{name}, APK")
            check_scene(scratch / "guest" / app / "data" / "files" / "scene.csv", f"{name}, APK")
            host_crc, guest_crc = crc(host, f"{name}, host build"), crc(guest, f"{name}, APK")
            print(f"{name}: frame 1000's CRC {host_crc} in the host build, {guest_crc} in the APK", flush=True)
            if host_crc is not None and guest_crc is not None and host_crc != guest_crc:
                fail(f"{name}: the CRCs of frame 1000 differ")
            crcs[load] = host_crc
        if crcs["light"] is not None and crcs["light"] == crcs["heavy"]:
            fail(f"{api}: the loads drew the same frame 1000")

    print(f"{len(failures)} failures" if failures else "PASS: graphics.bench")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
