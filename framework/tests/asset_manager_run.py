#!/usr/bin/env python3
"""ADR-0039 b3.3's test on the Mac: app_process starts asset-manager-test as test 1 starts its app
(framework/tests/app_process_run.py's start()), with framework-res as the folder /system/framework/framework-res
(decision 8). The app's Application class reads framework-res and its own APK through AssetManager and ApkAssets,
whose natives libandroid_runtime.so holds and app_process registers, and prints what the sources say
(fixtures/expected/asset-manager-test.txt).

The expected file's lines are the app's System.out lines without "asset-manager-test: ", in order; a line that
starts with "logcat " must be a line of the process's log (apk-run's stderr), anywhere. Prints "skipped: ..." while
a part is not built.

Usage: asset_manager_run.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work>
                            <app_process64> <framework.jar> <framework-res folder> <asset-manager-test.apk>
                            <expected>
"""
import os
import sys

from app_process_run import missing, start

PACKAGE = "io.github.kisame76.minato.test.assetmanager"
APPLICATION = "io.github.kisame76.minato.test.assetmanager.AssetManagerApplication"
PREFIX = "asset-manager-test: "


def main():
    if len(sys.argv) != 13:
        raise SystemExit(__doc__)
    (art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process, framework_jar, folder, apk,
     expected) = sys.argv[1:]
    skipped = missing(art, bionic_system, app_process, framework_jar, apk, "asset-manager-test")
    if not skipped and not os.path.isdir(folder):
        skipped = f"skipped: {folder} is not built (tools/build-framework-res.sh)"
    if skipped:
        print(skipped)
        return
    result = start(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, app_process, framework_jar,
                   apk, PACKAGE, APPLICATION, mounts=[("/system/framework/framework-res", folder)])
    if result is None:
        print("FAIL: no end within 300 s")
        sys.exit(1)

    with open(expected, encoding="utf-8") as handle:
        lines = [line for line in handle.read().splitlines() if line and not line.startswith("#")]
    wanted = [line for line in lines if not line.startswith("logcat ")]
    wanted_log = [line[len("logcat "):] for line in lines if line.startswith("logcat ")]
    got = [line[len(PREFIX):] for line in result.stdout.splitlines() if line.startswith(PREFIX)]
    log = result.stderr.splitlines()
    missing_log = [line for line in wanted_log if line not in log]
    if result.returncode == 0 and got == wanted and not missing_log:
        print(f"asset-manager-test read framework-res and its APK through libandroid_runtime.so, {len(got)} lines "
              f"and {len(wanted_log)} log lines")
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


if __name__ == "__main__":
    main()
