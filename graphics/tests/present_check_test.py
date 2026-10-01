#!/usr/bin/env python3
"""ADR-0041 test 3: graphics.present_check. tools/present-check.py on made-up recordings: present.csv as present-test
writes it (per rate 60 and 120, three blocks of 200 frames presented 1, 2 and 3 periods on) and drawables.csv as the
App's recording writes it (row k is frame k). The tolerance of decision 11: a frame is presented no earlier than 1 ms
before the time it asked for and no later than one refresh period after it, for 99 % of the 600 frames of each rate.
- on time: every frame 0.5 ms after its time; both rates pass;
- the limits: exactly 1 ms early and exactly one period late are in, 1 ns beyond either is out;
- 6 frames of 600 out pass a rate (99.0 %), 7 fail it, also at 120 Hz alone; a frame never presented counts out;
- inputs that cannot be matched are refused: fewer drawables than frames, frames out of order, 599 frames at a rate.
Usage: present_check_test.py --tool <present-check.py> --scratch <dir>"""
import argparse
import pathlib
import shutil
import subprocess
import sys

S = 1_000_000_000
MS = 1_000_000
PRESENT_HEADER = ("api,rate,interval,frame,frame_time_ns,asked_ns,present_id,desired_ns,actual_ns,earliest_ns,"
                  "margin_ns")

failures = []


def fail(what):
    print(f"FAIL: {what}", flush=True)
    failures.append(what)


def frames(api="gles"):
    """present-test's frames: (api, rate, interval, frame, frame_time, asked), the 60 Hz blocks first."""
    made, start = [], 1_000 * S
    for rate in (60, 120):
        period = S // rate
        time = start
        for interval in (1, 2, 3):
            for _ in range(200):
                made.append((api, rate, interval, len(made), time, time + interval * period))
                time += interval * period
        start = time + 30 * period
    return made


def write(folder, rows, presented):
    """present.csv from `rows` and drawables.csv with `presented(row)` as each frame's present time (0: never)."""
    folder.mkdir(parents=True)
    with open(folder / "present.csv", "w") as out:
        out.write(PRESENT_HEADER + "\n")
        for api, rate, interval, frame, frame_time, asked in rows:
            out.write(f"{api},{rate},{interval},{frame},{frame_time},{asked},0,0,0,0,0\n")
    with open(folder / "drawables.csv", "w") as out:
        out.write("frame,next_drawable_ns,presented_ns\n")
        for row in rows:
            out.write(f"{row[3]},{row[4]},{presented(row)}\n")
    return folder


def check(args, folder, want_status, *wanted):
    result = subprocess.run([sys.executable, args.tool, str(folder / "present.csv"), str(folder / "drawables.csv")],
                            capture_output=True, text=True)
    output = result.stdout + result.stderr
    name = folder.name
    if result.returncode != want_status:
        fail(f"{name}: exit {result.returncode}, expected {want_status}:\n{output}")
    for line in wanted:
        if line not in output:
            fail(f"{name}: no line with {line!r}:\n{output}")
    if not failures or not failures[-1].startswith(name):
        print(f"ok: {name}", flush=True)


def late_by(offsets):
    """A present time: the frame's time plus offsets.get(frame, 0.5 ms)."""
    return lambda row: row[5] + offsets.get(row[3], MS // 2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--scratch", required=True)
    args = parser.parse_args()
    scratch = pathlib.Path(args.scratch)
    shutil.rmtree(scratch, ignore_errors=True)
    rows = frames()

    check(args, write(scratch / "on-time", rows, late_by({})), 0,
          "rate 60: 600 frames, 600 in tolerance (100.0 %)", "rate 120: 600 frames, 600 in tolerance (100.0 %)",
          "present-check: pass")
    # The limits, at 60 Hz: frame 0 exactly 1 ms early, frame 1 exactly a period late, both in; frame 2 1 ns more
    # than 1 ms early, frame 3 1 ns more than a period late, both out; at 120 Hz frame 600 a period of 120 Hz late.
    limits = {0: -MS, 1: S / 60, 2: -MS - 1, 3: S / 60 + 1, 600: S / 120}
    check(args, write(scratch / "limits", rows, lambda row: int(row[5] + limits.get(row[3], MS // 2))), 0,
          "rate 60: 600 frames, 598 in tolerance (99.7 %), early 1, late 1, not presented 0",
          "rate 120: 600 frames, 600 in tolerance (100.0 %)")
    six = {frame: -2 * MS for frame in range(6)}
    check(args, write(scratch / "six-early", rows, late_by(six)), 0,
          "rate 60: 600 frames, 594 in tolerance (99.0 %), early 6", "present-check: pass")
    seven = {frame: -2 * MS for frame in range(7)}
    check(args, write(scratch / "seven-early", rows, late_by(seven)), 1,
          "rate 60: 600 frames, 593 in tolerance (98.8 %), early 7", "present-check: fail")
    late120 = {frame: 10 * MS for frame in range(600, 607)}
    check(args, write(scratch / "seven-late-120", rows, late_by(late120)), 1,
          "rate 60: 600 frames, 600 in tolerance (100.0 %)", "rate 120: 600 frames, 593 in tolerance (98.8 %), early 0, "
          "late 7", "present-check: fail")
    never = write(scratch / "never-presented", rows, lambda row: 0 if row[3] < 7 else row[5] + MS // 2)
    check(args, never, 1, "rate 60: 600 frames, 593 in tolerance (98.8 %), early 0, late 0, not presented 7")

    short = write(scratch / "fewer-drawables", rows, late_by({}))
    lines = (short / "drawables.csv").read_text().splitlines()
    (short / "drawables.csv").write_text("\n".join(lines[:-1]) + "\n")
    check(args, short, 2, "1200 frames but 1199 drawables")
    swapped = rows[:]
    swapped[10], swapped[11] = swapped[11], swapped[10]
    check(args, write(scratch / "out-of-order", swapped, late_by({})), 2, "frame 11 in row 10")
    kept = [row for row in rows if row[3] != 599]
    short_rate = [(api, rate, interval, frame, frame_time, asked)
                  for frame, (api, rate, interval, _, frame_time, asked) in enumerate(kept)]
    check(args, write(scratch / "short-rate", short_rate, late_by({})), 2, "rate 60: 599 frames, not 600")

    if failures:
        print(f"{len(failures)} of the checks failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
