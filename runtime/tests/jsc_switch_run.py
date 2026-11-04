#!/usr/bin/env python3
"""The JavaScriptCore switch (runtime/android/jsc.c): the loader preloads libminato-jsc.so and, before the constructors
of a library that defines JSGlobalContextCreate run, calls its function, which sets JSC_useJIT=0 and
JavaScriptCoreUseJIT=0 (JSC then starts its interpreter instead of mprotecting a code pool executable, which iOS
refuses). apk-run runs the fixture jsc-switch-test, which opens libfake-jsc.so with dlopen and looks the names up, in
three orders:
  (none)        the library is mapped before the app's first getenv;
  getenv-first  the app reads JSC_useJIT first;
  app-sets      the app sets JSC_useJIT=1 itself, which the switch overrides.
Without libminato-jsc.so in /system/lib64 the output is Android's (the fake reads both variables in a constructor
and at its first context); with it both variables are 0 at both places and the tier is the interpreter. That an app
without JavaScriptCore keeps init's environment is runtime.app_environment's.
Prints "skipped: ..." while the fixture or the library is not built.

Usage: jsc_switch_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

FIXTURES = ["jsc-switch-test", "libfake-jsc.so"]
LIBRARY = "libminato-jsc.so"


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def main():
    if len(sys.argv) != 8:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work, expected = sys.argv[1:]
    for path in [os.path.join(fixtures, name) for name in FIXTURES]:
        if not os.path.exists(path):
            print(f"skipped: {path} is not built (tools/build-fixtures.sh)")
            return
    with open(expected) as file:
        android = file.read().splitlines()
    constructor, app, context, tier = android
    off = "JSC_useJIT=0 JavaScriptCoreUseJIT=0"
    interpreter = "jsc-switch-test: the interpreter runs, no code pool"
    mapped_first = android  # the app reads after the library is mapped: "unset" without the switch
    expected_with = {
        "": [f"jsc-switch-test: constructor: {off}", "jsc-switch-test: app: JSC_useJIT=0",
             f"jsc-switch-test: first context: {off}", interpreter],
        "getenv-first": ["jsc-switch-test: app: JSC_useJIT=unset", f"jsc-switch-test: constructor: {off}",
                         f"jsc-switch-test: first context: {off}", interpreter],
        "app-sets": [f"jsc-switch-test: constructor: {off}", "jsc-switch-test: app: JSC_useJIT=0",
                     f"jsc-switch-test: first context: {off}", interpreter],
    }
    expected_without = {
        "": mapped_first,
        "getenv-first": [app, constructor, context, tier],
        # JSC_useJIT=1 from the app: the fake's JIT stays on, as on Android.
        "app-sets": [constructor.replace("JSC_useJIT=unset", "JSC_useJIT=1"), app.replace("unset", "1"),
                     context.replace("JSC_useJIT=unset", "JSC_useJIT=1"), tier],
    }

    system = os.path.join(work, "system-lib64")
    tmp = os.path.join(work, "data-local-tmp")
    for folder in (system, tmp):
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        for name in FIXTURES:
            convert(apk_convert, os.path.join(fixtures, name), os.path.join(work, name + ".dylib"))
    link = os.path.join(tmp, "libfake-jsc.so.dylib")
    if not os.path.lexists(link):
        os.symlink(os.path.join(work, "libfake-jsc.so.dylib"), link)

    # The system's libraries without the switch's.
    without = os.path.join(work, "system-lib64-without")
    os.makedirs(without, exist_ok=True)
    for name in os.listdir(system):
        entry = os.path.join(without, name)
        if name != LIBRARY + ".dylib" and not os.path.lexists(entry):
            os.symlink(os.path.join(system, name), entry)
    has_library = os.path.exists(os.path.join(bionic_lib, LIBRARY))

    def run(arguments, libraries):
        command = [apk_run, "--mount", f"/data/local/tmp={tmp}", "--mount", f"/system={bionic_system}",
                   "--libraries", f"/system/lib64={libraries}", "--cwd", "/data/local/tmp", "--argv0",
                   "./jsc-switch-test", "--env", "LD_LIBRARY_PATH=/data/local/tmp",
                   os.path.join(work, "jsc-switch-test.dylib"), *arguments]
        return subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)

    failures = []
    for mode in ("", "getenv-first", "app-sets"):
        arguments = [mode] if mode else []
        for libraries, lines, label in ((without, expected_without[mode], "without the library"),
                                        (system, expected_with[mode] if has_library else expected_without[mode],
                                         "with the library")):
            result = run(arguments, libraries)
            if result.returncode != 0 or result.stdout.splitlines() != lines:
                failures.append(f"{mode or 'dlopen'} {label}: exit status {result.returncode}, stdout "
                                f"{result.stdout.splitlines()}, expected {lines}, stderr's end "
                                f"{result.stderr.splitlines()[-5:]}")
    if not has_library:
        failures.append(f"{LIBRARY} is not built (tools/build-bionic.sh): the switch has nothing to run")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        sys.exit(1)
    print("jsc switch: JSC_useJIT=0 and JavaScriptCoreUseJIT=0 set before the library's constructor, "
          "whether the app read the variable first or set it itself")


if __name__ == "__main__":
    main()
