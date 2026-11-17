#!/usr/bin/env python3
"""The FPS unlock (ADR-0066, steps 1 and 2): with apk-run's --frame-rate N (apkrt_set_frame_rate_unlock), the loader
preloads libminato-frame-rate.so and resolves il2cpp_add_internal_call and mono_add_internal_call, by import and by
dlsym alike, to its wrappers, which register the engine's targetFrameRate setter wrapped: a rate the app sets below N,
or -1 (Unity's default), becomes N, a higher one stays. An engine that never sets a rate keeps its own (v1). apk-run
runs the fixture frame-cap-test, whose engine library reaches the setter by import, and with the argument "swap" by
dlsym:
  - without the option, both ways: fixtures/expected/frame-cap-test.out, as Android prints it, and nothing on
    stderr, as the fixture tests want it (this run stands for the fixture's own), also with no
    libminato-frame-rate.so in /system/lib64, as nothing loads it then;
  - with 60 but no libminato-frame-rate.so: the program does not start, the error names the unlock;
  - with 120 and with 60, both ways: 30 fps before the app sets a rate, then 60 and -1 raised to N, 240 kept;
  - with 90, not a divisor of 120: apk-run's usage, exit status 2.
Prints "skipped: ..." while the fixture or the library is not built.

Usage: frame_rate_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
       frame_rate_run.py --lines 60|120   prints the lines for that rate, for tools/device-suite.sh --frame-rate
"""
import fcntl
import os
import subprocess
import sys

FIXTURES = ["frame-cap-test", "libframe-cap-api.so", "libframe-cap-engine.so"]
LIBRARY = "libminato-frame-rate.so"


def unlocked(rate):
    """frame-cap-test's lines with the unlock at `rate`."""
    return [
        "frame-cap-test: 30 fps before the app sets a rate",
        f"frame-cap-test: the app set 60, the getter answers {max(60, rate)}, the engine runs at {max(60, rate)} fps",
        f"frame-cap-test: the app set -1, the getter answers {rate}, the engine runs at {rate} fps",
        "frame-cap-test: the app set 240, the getter answers 240, the engine runs at 240 fps",
    ]


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--lines" and sys.argv[2] in ("60", "120"):
        print("\n".join(unlocked(int(sys.argv[2]))))
        return
    if len(sys.argv) != 8:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work, expected = sys.argv[1:]
    for path in [os.path.join(fixtures, name) for name in FIXTURES] + [os.path.join(bionic_lib, LIBRARY)]:
        if not os.path.exists(path):
            print(f"skipped: {path} is not built (tools/build-fixtures.sh, tools/build-bionic.sh)")
            return
    with open(expected) as file:
        output = file.read().splitlines()

    # The containers stay from run to run; runs side by side convert one at a time.
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

    # The system's libraries without the unlock's.
    without = os.path.join(work, "system-lib64-without")
    os.makedirs(without, exist_ok=True)
    for name in os.listdir(system):
        link = os.path.join(without, name)
        if name != LIBRARY + ".dylib" and not os.path.lexists(link):
            os.symlink(os.path.join(system, name), link)

    def run(options, arguments, libraries=system):
        command = [apk_run, *options, "--mount", f"/data/local/tmp={tmp}", "--mount", f"/system={bionic_system}",
                   "--libraries", f"/system/lib64={libraries}", "--cwd", "/data/local/tmp", "--argv0",
                   "./frame-cap-test", "--env", "LD_LIBRARY_PATH=/data/local/tmp",
                   os.path.join(work, "frame-cap-test.dylib"), *arguments]
        return subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)

    failures = []
    for options, lines in (([], output), (["--frame-rate", "120"], unlocked(120)),
                           (["--frame-rate", "60"], unlocked(60))):
        for arguments in ([], ["swap"]):
            result = run(options, arguments)
            if result.returncode != 0 or result.stdout.splitlines() != lines or (not options and result.stderr):
                failures.append(f"{' '.join(options + arguments) or 'plain'}: exit status {result.returncode}, stdout "
                                f"{result.stdout.splitlines()}, expected {lines}, stderr's end "
                                f"{result.stderr.splitlines()[-5:]}")
    for arguments in ([], ["swap"]):
        result = run([], arguments, without)
        if result.returncode != 0 or result.stdout.splitlines() != output:
            failures.append(f"without the library {' '.join(arguments)}: exit status {result.returncode}, stdout "
                            f"{result.stdout.splitlines()}, stderr's end {result.stderr.splitlines()[-5:]}")
    missing = run(["--frame-rate", "60"], [], without)
    if missing.returncode == 0 or "needed for the FPS unlock" not in missing.stderr:
        failures.append(f"--frame-rate 60 without the library: exit status {missing.returncode}, stderr's end "
                        f"{missing.stderr.splitlines()[-5:]}")
    refused = run(["--frame-rate", "90"], [])
    if refused.returncode != 2 or refused.stdout:
        failures.append(f"--frame-rate 90: exit status {refused.returncode}, stdout {refused.stdout.splitlines()}")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        sys.exit(1)
    print("frame rate: the setter raised to the unlock by import and by dlsym, nothing without it")


if __name__ == "__main__":
    main()
