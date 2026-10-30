#!/usr/bin/env python3
"""Ren'Py on the Mac (tools/renpy-check.sh): Minato's Ren'Py test game (fixtures/renpy/minato-renpy-test), built with
Ren'Py's SDK and RAPT, through the Java path as the launcher starts it (framework/tests/app_process_run.py's start(),
with the manifest's Application), in a landscape session of the M4 plan's display, 2796 x 1290 at scale 3, as its
sensorLandscape activity has it on the iPhone, under graphics-run with the session's layers (MINATO_SESSION_LAYERS=1):
the SurfaceView of SDL gets a layer of its own, on which the Mac's ANGLE (MINATO_ANGLE) draws Ren'Py's frames.

The game prints its lines through Ren'Py's log, the tag python. Those that begin "minato-renpy-test: " must be Android
15's (fixtures/expected/minato-renpy-test.logcat, tools/android-reference.sh's form, the tag unpadded): its start,
whether its tone plays and that the tone is about 1 s in after a second's hard pause, which a wrong output rate moves,
and its end. Ren'Py's line "Hid presplash." must come first, which it logs once its first frame is drawn. The run ends
2 s after the game's last line (app_process_run.run_app); Ren'Py's quit then still runs, so the harness ends it.

Usage: renpy_run.py <app> <package> <art> <bionic-lib> <bionic-system> <hwui-lib> <icu-data> <apk-convert>
                    <graphics-run> <work> <app_process64> <framework.jar> <framework-res folder> <app.apk> <expected>
"""
import os
import re
import sys

from app_process_run import missing, start

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PERMISSIONS = os.path.join(ROOT, "build", "sources", "frameworks_base_etc", "data", "etc")
# Ren'Py's tag as logcat pads it.
TAG = r"^([VDIWEF])/python *: "
LAST = TAG + r"minato-renpy-test: done$"
FIRST_FRAME = TAG + r"Hid presplash\.$"


def main():
    args = sys.argv[1:]
    if len(args) != 15:
        raise SystemExit(__doc__)
    (app, package, art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, graphics_run, work, app_process,
     framework_jar, folder, apk, expected) = args
    skipped = missing(art, bionic_system, hwui_lib, app_process, framework_jar, apk, app)
    if not skipped and not os.path.isdir(folder):
        skipped = f"skipped: {folder} is not built (tools/build-framework-res.sh)"
    if skipped:
        print(skipped)
        return
    mounts = [("/system/framework/framework-res", folder)]
    if os.path.isfile(os.path.join(PERMISSIONS, "platform.xml")):
        mounts.append(("/system/etc/permissions", PERMISSIONS))
    result = start(art, bionic_lib, bionic_system, hwui_lib, icu_data, apk_convert, graphics_run, work, app_process,
                   framework_jar, apk, package, None, mounts=mounts, configuration="2796x1290@3,en-US", last=LAST)
    if result is None:
        print("FAIL: no line \"minato-renpy-test: done\" within 300 s")
        sys.exit(1)
    log = result.stderr.splitlines()
    # The tag unpadded, as the expected file has it.
    python = [re.sub(TAG, r"\1/python: ", line) for line in log if re.match(TAG, line)]
    got = [line for line in python if re.match(r"^[VDIWEF]/python: minato-renpy-test: ", line)]
    with open(expected, encoding="utf-8") as handle:
        wanted = handle.read().splitlines()
    first = next((index for index, line in enumerate(log) if re.match(FIRST_FRAME, line)), None)
    started = next((index for index, line in enumerate(log) if re.match(TAG + "minato-renpy-test: tone playing", line)),
                   None)
    drawn = first is not None and started is not None and first < started
    if result.returncode == 0 and got == wanted and drawn:
        print(f"{app} logged what Android 15 logs, {len(got)} lines, after its first frame")
        return
    print(f"FAIL: exit status {result.returncode}"
          + ("" if drawn else ', no "Hid presplash." before the tone\'s lines'))
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
