#!/usr/bin/env python3
"""ADR-0041 test 3: graphics.present_check. tools/present-check.py on made-up recordings: present.csv as present-test
writes it (per rate 60 and 120, three blocks of 200 frames presented 1, 2 and 3 periods on, each frame with its
timeline 0, 2 periods after its frame time at 60 Hz and 3 at 120 Hz as the runtime's make_tick gives them) and
drawables.csv as the App's recording writes it (row k is frame k). Decision 11's tolerance, as changed on 2026-10-01:
a frame is presented no earlier than 1 ms before its due time and no later than one refresh period after it, where
the due time is the time it asked for, or its timeline 0 when it asked for an earlier one; for 99 % of the 600 frames
of each rate, with each block's frame times a step of its periods apart (median, 1 %).
- on time: every frame 0.5 ms after its due time; both rates pass;
- the limits of both kinds: exactly 1 ms early and exactly one period late are in, 1 ns beyond either is out; a frame
  asked for before its timeline 0 and presented at the time it asked for is early;
- 6 frames of 600 out pass a rate (99.0 %), 7 fail it, also at 120 Hz alone; a frame never presented counts out;
- a block whose frame rate falls fails its rate with every frame in tolerance: frame times 16.67 ms apart in the
  120 Hz block of 1 period, as the sitting of 2026-10-01 showed; 2 % apart from its periods it fails, 0.5 % holds;
- inputs that cannot be matched are refused: fewer drawables than frames, frames out of order, 599 frames at a rate,
  a present.csv without timeline 0.
Usage: present_check_test.py --tool <present-check.py> --scratch <dir>"""
import argparse
import pathlib
import shutil
import subprocess
import sys

S = 1_000_000_000
MS = 1_000_000
PRESENT_HEADER = ("api,rate,interval,frame,frame_time_ns,asked_ns,timeline0_ns,present_id,desired_ns,actual_ns,"
                  "earliest_ns,margin_ns")
TIMELINE0_PERIODS = {60: 2, 120: 3}

failures = []


def fail(what):
    print(f"FAIL: {what}", flush=True)
    failures.append(what)


def frames(api="gles", steps=None):
    """present-test's frames: (api, rate, interval, frame, frame_time, asked, timeline0), the 60 Hz blocks first. The
    frame times step by `steps[(rate, interval)]` periods, by the interval when it is not given."""
    made, start = [], 1_000 * S
    for rate in (60, 120):
        period = S // rate
        time = start
        for interval in (1, 2, 3):
            for _ in range(200):
                made.append((api, rate, interval, len(made), time, time + interval * period,
                             time + TIMELINE0_PERIODS[rate] * period))
                time += int((steps or {}).get((rate, interval), interval) * period)
        start = time + 30 * period
    return made


def due(row):
    """The time a frame is judged from: the time it asked for, or its timeline 0 when it asked for an earlier one."""
    return max(row[5], row[6])


def write(folder, rows, presented):
    """present.csv from `rows` and drawables.csv with `presented(row)` as each frame's present time (0: never)."""
    folder.mkdir(parents=True)
    with open(folder / "present.csv", "w") as out:
        out.write(PRESENT_HEADER + "\n")
        for api, rate, interval, frame, frame_time, asked, timeline0 in rows:
            out.write(f"{api},{rate},{interval},{frame},{frame_time},{asked},{timeline0},0,0,0,0,0\n")
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
    """A present time: the frame's due time plus offsets.get(frame, 0.5 ms)."""
    return lambda row: due(row) + offsets.get(row[3], MS // 2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--scratch", required=True)
    args = parser.parse_args()
    scratch = pathlib.Path(args.scratch)
    shutil.rmtree(scratch, ignore_errors=True)
    rows = frames()

    check(args, write(scratch / "on-time", rows, late_by({})), 0,
          "rate 60: 600 frames, 600 in tolerance (100.0 %), early 0, late 0, not presented 0; 200 asked before "
          "timeline 0", "rate 120: 600 frames, 600 in tolerance (100.0 %), early 0, late 0, not presented 0; 400 asked "
          "before timeline 0", "  1 period on: 200 frames, 200 in tolerance", "frame times 16.667 ms apart (median), "
          "held", "frame times 25.000 ms apart (median), held", "present-check: pass")
    # The limits of frames asked for at or after timeline 0, at 60 Hz in the block of 2 periods (frames 200 on):
    # frame 200 exactly 1 ms early, 201 exactly a period late, both in; 202 1 ns more than 1 ms early, 203 1 ns more
    # than a period late, both out. Of frames asked for before it, at 120 Hz in the block of 1 period (600 on): 600
    # exactly 1 ms before timeline 0 and 601 exactly a period after it, both in; 602 and 603 1 ns beyond, both out.
    limits = {200: -MS, 201: S / 60, 202: -MS - 1, 203: S / 60 + 1, 600: -MS, 601: S / 120, 602: -MS - 1,
              603: S / 120 + 1}
    check(args, write(scratch / "limits", rows, lambda row: int(due(row) + limits.get(row[3], MS // 2))), 0,
          "rate 60: 600 frames, 598 in tolerance (99.7 %), early 1, late 1, not presented 0",
          "rate 120: 600 frames, 598 in tolerance (99.7 %), early 1, late 1, not presented 0")
    # Frame 0 asked for 1 period on, a period before its timeline 0, and presented 0.5 ms after the time it asked for.
    asked = write(scratch / "at-the-time-asked", rows, lambda row: row[5] + MS // 2 if row[3] == 0 else due(row) + 1000)
    check(args, asked, 0, "rate 60: 600 frames, 599 in tolerance (99.8 %), early 1, late 0")
    fell = write(scratch / "rate-falls", frames(steps={(120, 1): 2}), late_by({}))
    check(args, fell, 1, "rate 120: 600 frames, 600 in tolerance (100.0 %)",
          "  1 period on: 200 frames, 200 in tolerance", "frame times 16.667 ms apart (median), not 8.333 ms: fell",
          "present-check: fail")
    drift = write(scratch / "rate-drifts", frames(steps={(60, 3): 3.06, (120, 2): 2.01}), late_by({}))
    check(args, drift, 1, "frame times 51.000 ms apart (median), not 50.000 ms: fell",
          "frame times 16.750 ms apart (median), held", "present-check: fail")
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
    never = write(scratch / "never-presented", rows, lambda row: 0 if row[3] < 7 else due(row) + MS // 2)
    check(args, never, 1, "rate 60: 600 frames, 593 in tolerance (98.8 %), early 0, late 0, not presented 7")

    short = write(scratch / "fewer-drawables", rows, late_by({}))
    lines = (short / "drawables.csv").read_text().splitlines()
    (short / "drawables.csv").write_text("\n".join(lines[:-1]) + "\n")
    check(args, short, 2, "1200 frames but 1199 drawables")
    swapped = rows[:]
    swapped[10], swapped[11] = swapped[11], swapped[10]
    check(args, write(scratch / "out-of-order", swapped, late_by({})), 2, "frame 11 in row 10")
    kept = [row for row in rows if row[3] != 599]
    short_rate = [(api, rate, interval, frame, frame_time, asked, timeline0)
                  for frame, (api, rate, interval, _, frame_time, asked, timeline0) in enumerate(kept)]
    check(args, write(scratch / "short-rate", short_rate, late_by({})), 2, "rate 60: 599 frames, not 600")
    old = write(scratch / "without-timeline-0", rows, late_by({}))
    lines = (old / "present.csv").read_text().splitlines()
    (old / "present.csv").write_text("\n".join(",".join(cells for index, cells in enumerate(line.split(","))
                                                         if index != 6) for line in lines) + "\n")
    check(args, old, 2, "no column timeline0_ns")

    if failures:
        print(f"{len(failures)} of the checks failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
