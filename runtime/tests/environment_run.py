#!/usr/bin/env python3
"""apk-run's --app-environment (ADR-0039 decision 2): the program starts with the environment the runtime gives
every program, init's as an app on Android 15 has it, without the defaults apk-run takes from Android's shell
(PATH, ANDROID_DATA, ANDROID_ROOT and HOME). apk-run runs the fixture env-test three times:
  - with --app-environment: no HOME, and init's PATH, an app's on Android 15 [verified: the emulator, 2026-10-01];
  - without it: the same entries, with apk-run's PATH in PATH's place and HOME=/ at the end;
  - with --app-environment and --env entries: those still come on top, a name of init's in its place.
Prints "skipped: ..." while a part is not built.

Usage: environment_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work>
"""
import fcntl
import os
import subprocess
import sys

ANDROID_PATH = ("PATH=/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:"
                "/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin")


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def main():
    if len(sys.argv) != 7:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work = sys.argv[1:]
    if not os.path.exists(os.path.join(fixtures, "env-test")):
        print(f"skipped: {os.path.join(fixtures, 'env-test')} is not built (tools/build-fixtures.sh)")
        return

    # The containers stay from run to run; runs side by side convert one at a time.
    system = os.path.join(work, "system-lib64")
    program = os.path.join(work, "bin", "env-test.dylib")
    for folder in (system, os.path.dirname(program)):
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        convert(apk_convert, os.path.join(fixtures, "env-test"), program)

    def environment(*options):
        command = [apk_run, *options, "--mount", f"/system={bionic_system}", "--libraries", f"/system/lib64={system}",
                   "--cwd", "/", "--argv0", "/data/local/tmp/env-test", program]
        try:
            result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)
        except subprocess.TimeoutExpired as timeout:
            print(f"FAIL: {' '.join(options)}: no end within {timeout.timeout} s")
            sys.exit(1)
        if result.returncode != 0:
            print(f"FAIL: {' '.join(options)}: exit status {result.returncode}")
            print("stderr, last 20 lines:")
            print("\n".join(result.stderr.splitlines()[-20:]))
            sys.exit(1)
        return result.stdout.splitlines()

    app = environment("--app-environment")
    shell = environment()
    given = environment("--app-environment", "--env", "PATH=/given", "--env", "OWN=1")

    failures = []
    if any(entry.startswith("HOME=") for entry in app):
        failures.append("with --app-environment, HOME is set")
    if ANDROID_PATH not in app:
        failures.append("with --app-environment, PATH is not an app's on Android 15")
    else:
        path = app.index(ANDROID_PATH)
        shell_path = [entry for entry in shell if entry.startswith("PATH=")]
        if shell != app[:path] + shell_path + app[path + 1:] + ["HOME=/"]:
            failures.append("without --app-environment, the entries are not the same with apk-run's PATH and HOME=/")
        if given != app[:path] + ["PATH=/given"] + app[path + 1:] + ["OWN=1"]:
            failures.append("with --app-environment, --env's entries do not come on top")
    if not failures:
        print(f"--app-environment gave init's {len(app)} entries, without apk-run's PATH and HOME")
        return
    for failure in failures:
        print(f"FAIL: {failure}")
    for name, entries in (("--app-environment", app), ("default", shell), ("with --env", given)):
        print(f"{name}: {entries}")
    sys.exit(1)


if __name__ == "__main__":
    main()
