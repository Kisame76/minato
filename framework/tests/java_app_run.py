#!/usr/bin/env python3
"""ADR-0039's Java app tests on the Mac after test 1 (b3.3's asset-manager-test, b3.4a's resources-test, b3.4c's
config-test): app_process starts the app as test 1 starts its app (framework/tests/app_process_run.py's start()), with
framework-res as the folder /system/framework/framework-res (decision 8) and, since b3.5b1, the platform's
/system/etc/permissions/platform.xml (tools/fetch-sources.py --set framework), whose split permissions ManifestPackage
reads; while it is not fetched, the app starts without them, as Android starts without a missing file. The app's
Application class reads
resources through framework.jar and libandroid_runtime.so and prints what the sources say
(fixtures/expected/<app>.txt), or logs what Android 15 logs (fixtures/expected/<app>.logcat).

A .txt file's lines are the app's System.out lines without "<app>: ", in order; a line that starts with "logcat "
must be a line of the process's log (apk-run's stderr), anywhere. A .logcat file, from tools/android-reference.sh,
holds the app's log lines with its own tag, which the process's log must hold in that order; those with the tag
<app>-device differ by device and are only shown. A configuration begins a session (apk-run --configuration).
Prints "skipped: ..." while a part is not built.

With --apk-inspect, the app starts with its signers.json (apk-inspect --signers), as the library writes it at the
import, and each certificate SHA-256 the APK's check proved becomes framework/tests/certificate_mask.py's mark of its
role in the app's lines, as tools/android-reference.sh marks Android's (b3.5b1).

Usage: java_app_run.py [--apk-inspect <apk-inspect>] <app> <package> <application class> <art> <bionic-lib>
                       <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <app_process64> <framework.jar>
                       <framework-res folder> <app.apk> <expected> [<configuration>]
"""
import os
import re
import subprocess
import sys

import certificate_mask
from app_process_run import missing, start

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PERMISSIONS = os.path.join(ROOT, "build", "sources", "frameworks_base_etc", "data", "etc")


def main():
    args = sys.argv[1:]
    apk_inspect = None
    if args[:1] == ["--apk-inspect"] and len(args) > 1:
        apk_inspect, args = args[1], args[2:]
    if len(args) not in (15, 16):
        raise SystemExit(__doc__)
    (app, package, application, art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process,
     framework_jar, folder, apk, expected) = args[:15]
    configuration = args[15] if len(args) == 16 else None
    skipped = missing(art, bionic_system, app_process, framework_jar, apk, app)
    if not skipped and not os.path.isdir(folder):
        skipped = f"skipped: {folder} is not built (tools/build-framework-res.sh)"
    if skipped:
        print(skipped)
        return
    mounts = [("/system/framework/framework-res", folder)]
    if os.path.isfile(os.path.join(PERMISSIONS, "platform.xml")):
        mounts.append(("/system/etc/permissions", PERMISSIONS))
    signers = None
    if apk_inspect:
        os.makedirs(work, exist_ok=True)
        signers = os.path.join(work, "signers.json")
        subprocess.run([apk_inspect, "--signers", signers, apk], check=True, capture_output=True)
        if not os.path.exists(signers):
            signers = None  # an unsigned APK has none
    result = start(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process, framework_jar,
                   apk, package, application, mounts=mounts, configuration=configuration, signers=signers)
    if result is None:
        print("FAIL: no end within 300 s")
        sys.exit(1)
    if expected.endswith(".logcat"):
        marks = certificate_mask.marks(apk_inspect, apk) if apk_inspect else {}
        compare_log(app, result, expected, marks)
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


def compare_log(app, result, expected, marks):
    """The process's log lines with the app's tag against Android's, in order, with the APK's proven certificates
    marked (certificate_mask's marks); the device's lines are shown."""
    with open(expected, encoding="utf-8") as handle:
        wanted = handle.read().splitlines()
    log = certificate_mask.mask(result.stderr, marks).splitlines()
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
