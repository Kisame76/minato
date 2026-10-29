#!/usr/bin/env python3
"""M4's test 2 on the Mac (ADR-0042 b)): a Godot APK of tools/build-godot.sh through the Java path, as
tools/android-reference.sh --app <app> runs it on the AVD. app_process starts it as the launcher does
(framework/tests/app_process_run.py's start(), with the manifest's Application), in a session of the M4 plan's display,
1290 x 2796 at scale 3, under graphics-run with the session's layers (MINATO_SESSION_LAYERS=1): the SurfaceView of
Godot's render view gets a layer of its own, on which the Mac's MoltenVK (MINATO_MOLTENVK) or ANGLE (MINATO_ANGLE)
draws its frames. apk-run's lifecycle taps the window's middle six times, a second apart after the start, on the
session's input channel, as tools/android-reference.sh taps the AVD's middle while the bench runs (test 2's gap 6):
the bench logs its first touch, in fractions of its viewport, so the line is the same on every screen.

Its log lines with the tag godot that begin "godot-bench: " must be Android 15's (fixtures/expected/<app>.logcat), as
android-reference.sh compares them. Its other lines with that tag are only shown, and one of them must be
"run <name>: N frames, 0 failures" with N at least 1. Then Godot ends its own process with SIGKILL ("Force quitting
Godot instance"), within 2 s of that line, as android-reference.sh waits for the process to be gone. Under a session,
the runtime's end handler ends apk-run on a guest's own SIGKILL with 137, 128 and the signal, as a shell shows
Android's; a process still there is killed by the harness and counts as 0 (app_process_run.run_app).

With --odex <file>, the file and its .vdex beside it lie at ART's place for base.apk, as for java_app_run.py's --odex
(M6 c1). The run's wall time, from apk-run's start to its end, is printed as "wall: <s> s" (tools/oat-start-measure.sh).

Usage: godot_java_run.py [--odex <file>] <app> <package> <art> <bionic-lib> <bionic-system> <hwui-lib> <icu-data>
                         <apk-convert>
                         <graphics-run> <work> <app_process64> <framework.jar> <framework-res folder> <app.apk>
                         <expected>
"""
import os
import re
import signal
import sys
import time

from app_process_run import missing, start

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PERMISSIONS = os.path.join(ROOT, "build", "sources", "frameworks_base_etc", "data", "etc")
# Godot's tag as logcat -v tag pads it (tools/android-reference.sh).
TAG = r"^[VDIWEF]/godot *: "
RUN = TAG + r"run [^:]+: [1-9][0-9]* frames, 0 failures$"
# The window's middle in the session's 1290 x 2796, six times: the bench runs about 2 s after the start, and apk-run
# plays the steps a second apart.
LIFECYCLE = "start," + ",".join(["tap:645:1398"] * 6)
# Its run line, whatever its counts: the run's end.
LAST = TAG + r"run [^:]+: [0-9]+ frames, [0-9]+ failures$"


def main():
    args = sys.argv[1:]
    odex = None
    if args[:1] == ["--odex"] and len(args) > 1:
        odex, args = args[1], args[2:]
    if len(args) != 15:
        raise SystemExit(__doc__)
    (app, package, art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, graphics_run, work, app_process,
     framework_jar, folder, apk, expected) = args
    if not os.path.isfile(apk):
        print(f"skipped: {apk} is not built (tools/build-godot.sh)")
        return
    skipped = missing(art, bionic_system, hwui_lib, app_process, framework_jar, apk, app)
    if not skipped and not os.path.isdir(folder):
        skipped = f"skipped: {folder} is not built (tools/build-framework-res.sh)"
    if skipped:
        print(skipped)
        return
    mounts = [("/system/framework/framework-res", folder)]
    if os.path.isfile(os.path.join(PERMISSIONS, "platform.xml")):
        mounts.append(("/system/etc/permissions", PERMISSIONS))
    began = time.monotonic()
    result = start(art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, graphics_run, work, app_process,
                   framework_jar, apk, package, None, mounts=mounts, configuration="1290x2796@3,en-US", last=LAST,
                   lifecycle=LIFECYCLE, odex=odex)
    wall = time.monotonic() - began
    if result is None:
        print("FAIL: no run line within 300 s")
        sys.exit(1)
    log = result.stderr.splitlines()
    godot = [line for line in log if re.match(TAG, line)]
    got = [line for line in godot if re.match(TAG + "godot-bench: ", line)]
    with open(expected, encoding="utf-8") as handle:
        wanted = handle.read().splitlines()
    ran = any(re.match(RUN, line) for line in godot)
    ended = result.returncode == 128 + signal.SIGKILL
    if ended and got == wanted and ran:
        print(f"{app} logged what Android 15 logs, {len(got)} lines, ran without failures and ended itself")
        print(f"wall: {wall:.3f} s")
        for line in godot:
            if line not in got:
                print(f"  device: {line}")
        return
    print(f"FAIL: exit status {result.returncode}" + ("" if ended else ", not ended by its own SIGKILL")
          + ("" if ran else ', no line "run <name>: N frames, 0 failures"'))
    for line in wanted:
        if line not in got:
            print(f"  only Android's: {line}")
    for line in got:
        if line not in wanted:
            print(f"  only ours:      {line}")
    print("stderr, last 40 lines:")
    print("\n".join(log[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
