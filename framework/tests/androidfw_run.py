#!/usr/bin/env python3
"""ADR-0039 b3.2's test on the Mac: androidfw-test (framework/tests/native/androidfw_test.cpp), guest code with
libandroidfw and its libraries linked in, reads framework-res and an app's resources in the guest on our loader,
and prints what the resources' sources say (fixtures/expected/androidfw-test.txt).

apk-run runs it with bionic's libraries in /system/lib64, framework-res where Minato ships it, as the folder
/system/framework/framework-res (ADR-0039 decision 8) and, for the APK provider, as
/system/framework/framework-res.apk, and the app at /data/app/androidfw-test/base.apk. Prints "skipped: ..." while
a part is not built.

Usage: androidfw_run.py <apk-convert> <apk-run> <bionic-lib> <bionic-system> <work> <androidfw-test>
                        <framework-res folder> <framework-res.apk> <app.apk> <expected>
"""
import os
import subprocess
import sys

PREFIX = "androidfw-test: "


def convert(apk_convert, elf, container):
    """The container of `elf`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    os.makedirs(os.path.dirname(container), exist_ok=True)
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def expected_lines(path):
    with open(path, encoding="utf-8") as handle:
        return [line for line in handle.read().splitlines() if line and not line.startswith("#")]


def main():
    if len(sys.argv) != 11:
        raise SystemExit(__doc__)
    (apk_convert, apk_run, bionic_lib, bionic_system, work, program, folder, framework_apk, app,
     expected) = sys.argv[1:]
    for needed, how in ((program, "tools/build-framework-native.sh"), (folder, "tools/build-framework-res.sh"),
                        (framework_apk, "tools/build-framework-res.sh"), (app, "tools/androidfw-check.sh"),
                        (os.path.join(bionic_lib, "libc.so"), "tools/build-bionic.sh")):
        if not os.path.exists(needed):
            print(f"skipped: {needed} is not built ({how})")
            return

    libraries = os.path.join(work, "system-lib64")
    for name in sorted(os.listdir(bionic_lib)):
        if name.endswith(".so"):
            convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(libraries, name + ".dylib"))
    container = os.path.join(work, "bin", "androidfw-test.dylib")
    convert(apk_convert, program, container)

    command = [apk_run,
               "--mount", f"/system={bionic_system}",
               "--mount", f"/system/framework/framework-res={folder}",
               "--mount", f"/system/framework/framework-res.apk={framework_apk}",
               "--mount", f"/data/app/androidfw-test/base.apk={app}",
               "--libraries", f"/system/lib64={libraries}",
               "--cwd", "/", "--argv0", "/data/local/tmp/androidfw-test",
               container, "/system/framework/framework-res", "/system/framework/framework-res.apk",
               "/data/app/androidfw-test/base.apk"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=300)
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL: no end within {timeout.timeout} s")
        sys.exit(1)
    got = [line[len(PREFIX):] for line in result.stdout.splitlines() if line.startswith(PREFIX)]
    wanted = expected_lines(expected)
    if result.returncode == 0 and got == wanted:
        print(f"androidfw-test read framework-res and the app as their sources say, {len(got)} lines")
        return
    print(f"FAIL: exit status {result.returncode}")
    for line in wanted:
        if line not in got:
            print(f"  only expected: {line}")
    for line in got:
        if line not in wanted:
            print(f"  only ours:     {line}")
    if got != wanted and sorted(got) == sorted(wanted):
        print("  the same lines in another order")
    print("stderr, last 40 lines:")
    print("\n".join(result.stderr.splitlines()[-40:]))
    sys.exit(1)


if __name__ == "__main__":
    main()
