#!/usr/bin/env python3
"""ADR-0039's Java app tests on the Mac after test 1 (b3.3's asset-manager-test, b3.4a's resources-test, b3.4c's
config-test): app_process starts the app as test 1 starts its app (framework/tests/app_process_run.py's start()), with
framework-res as the folder /system/framework/framework-res (decision 8). The app's Application class reads
resources through framework.jar and libandroid_runtime.so and prints what the sources say
(fixtures/expected/<app>.txt), or logs what Android 15 logs (fixtures/expected/<app>.logcat).

A .txt file's lines are the app's System.out lines without "<app>: ", in order; a line that starts with "logcat "
must be a line of the process's log (apk-run's stderr), anywhere. A .logcat file, from tools/android-reference.sh,
holds the app's log lines with its own tag, which the process's log must hold in that order; those with the tag
<app>-device differ by device and are only shown. A configuration begins a session (apk-run --configuration).
Prints "skipped: ..." while a part is not built.

Usage: java_app_run.py <app> <package> <application class> <art> <bionic-lib> <bionic-system> <icu-data>
                       <apk-convert> <apk-run> <work> <app_process64> <framework.jar> <framework-res folder>
                       <app.apk> <expected> [<configuration>]
"""
import os
import re
import sys

from app_process_run import missing, start


def main():
    if len(sys.argv) not in (16, 17):
        raise SystemExit(__doc__)
    (app, package, application, art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process,
     framework_jar, folder, apk, expected) = sys.argv[1:16]
    configuration = sys.argv[16] if len(sys.argv) == 17 else None
    skipped = missing(art, bionic_system, app_process, framework_jar, apk, app)
    if not skipped and not os.path.isdir(folder):
        skipped = f"skipped: {folder} is not built (tools/build-framework-res.sh)"
    if skipped:
        print(skipped)
        return
    result = start(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process, framework_jar,
                   apk, package, application, mounts=[("/system/framework/framework-res", folder)],
                   configuration=configuration)
    if result is None:
        print("FAIL: no end within 300 s")
        sys.exit(1)
    if expected.endswith(".logcat"):
        compare_log(app, result, expected)
        return

    with open(expected, encoding="utf-8") as handle:
        lines = [line for line in handle.read().splitlines() if line and not line.startswith("#")]
    wanted = [line for line in lines if not line.startswith("logcat ")]
    wanted_log = [line[len("logcat "):] for line in lines if line.startswith("logcat ")]
    prefix = f"{app}: "
    got = [line[len(prefix):] for line in result.stdout.splitlines() if line.startswith(prefix)]
    log = result.stderr.splitlines()
    missing_log = [line for line in wanted_log if line not in log]
    if result.returncode == 0 and got == wanted and not missing_log:
        print(f"{app} read its resources as their sources say, {len(got)} lines and {len(wanted_log)} log lines")
        return
    print(f"FAIL: exit status {result.returncode}")
    for line in wanted:
        if line not in got:
            print(f"  only expected: {line}")
    for line in got:
        if line not in wanted:
            print(f"  only ours:     {line}")
    for line in missing_log:
        print(f"  not in the log: {line}")
    print("stderr, last 40 lines:")
    print("\n".join(log[-40:]))
    sys.exit(1)


def compare_log(app, result, expected):
    """The process's log lines with the app's tag against Android's, in order; the device's lines are shown."""
    with open(expected, encoding="utf-8") as handle:
        wanted = handle.read().splitlines()
    log = result.stderr.splitlines()
    got = [line for line in log if re.match(rf"^[VDIWEF]/{re.escape(app)}: ", line)]
    device = [line for line in log if re.match(rf"^[VDIWEF]/{re.escape(app)}-device: ", line)]
    if result.returncode == 0 and got == wanted:
        print(f"{app} logged what Android 15 logs, {len(got)} lines")
        for line in device:
            print(f"  device: {line}")
        return
    print(f"FAIL: exit status {result.returncode}")
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
