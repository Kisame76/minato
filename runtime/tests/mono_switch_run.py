#!/usr/bin/env python3
"""The Mono switch (runtime/android/mono.c): the loader preloads libminato-mono.so, and mono_jit_set_aot_mode of a
library that also defines mono_jit_init_version (Mono's embedding library) becomes the wrapper, by import and
by dlsym alike, which calls the library's mono_jit_set_aot_mode with MONO_AOT_MODE_INTERP (5) instead of the mode
the host asked for (LLVMONLY, 4, becomes INTERP_LLVMONLY, 6; the interpreter modes 5 to 7 stay), so that the JIT
never asks for executable memory. apk-run runs the fixture mono-switch-test, which picks a mode and starts the
runtime through libfake-mono.so (imported, or opened with the argument "dlsym-mono") or through
libfake-notmono.so ("dlsym-notmono", a library that defines the mode name and is not Mono):
  - without libminato-mono.so in /system/lib64: fixtures/expected/mono-switch-test.out for the default mode, as Android
    prints it, and the mode the app asked for in every case;
  - with it: the mode that reaches Mono is the interpreter's and the report says "interpreter"; the library that is not
    Mono is left alone.
Prints "skipped: ..." while the fixture or a library is not built.

Usage: mono_switch_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

FIXTURES = ["mono-switch-test", "libfake-mono.so", "libfake-notmono.so"]
LIBRARY = "libminato-mono.so"


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def lines(mode, report, compiled):
    """The app's output for the mode Mono got."""
    return [f"mono-switch-test: mono_jit_set_aot_mode({mode})", "mono-switch-test: mono_jit_init_version",
            f"mono-switch-test: {report}", f"mono-switch-test: {compiled}"]


def main():
    if len(sys.argv) != 8:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work, expected = sys.argv[1:]
    for path in [os.path.join(fixtures, name) for name in FIXTURES]:
        if not os.path.exists(path):
            print(f"skipped: {path} is not built (tools/build-fixtures.sh)")
            return
    with open(expected) as file:
        android = file.read().splitlines()  # mode 1, the JIT allowed
    interp = ("interpreter", "the interpreter ran the method")
    aot_llvm = ("AOT only", "the method has no AOT code")
    plain = ["mono-switch-test: mono_jit_set_aot_mode(1)", "mono-switch-test: not Mono", "mono-switch-test: no engine"]

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
    for name in FIXTURES[1:]:
        link = os.path.join(tmp, name + ".dylib")
        if not os.path.lexists(link):
            os.symlink(os.path.join(work, name + ".dylib"), link)

    # The system's libraries without the switch's.
    without = os.path.join(work, "system-lib64-without")
    os.makedirs(without, exist_ok=True)
    for name in os.listdir(system):
        link = os.path.join(without, name)
        if name != LIBRARY + ".dylib" and not os.path.lexists(link):
            os.symlink(os.path.join(system, name), link)
    has_library = os.path.exists(os.path.join(bionic_lib, LIBRARY))

    def run(arguments, libraries):
        command = [apk_run, "--mount", f"/data/local/tmp={tmp}", "--mount", f"/system={bionic_system}",
                   "--libraries", f"/system/lib64={libraries}", "--cwd", "/data/local/tmp", "--argv0",
                   "./mono-switch-test", "--env", "LD_LIBRARY_PATH=/data/local/tmp",
                   os.path.join(work, "mono-switch-test.dylib"), *arguments]
        return subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)

    # (arguments, what Android prints, what the switch changes it to)
    cases = (
        ([], android, lines(5, *interp)),                                  # NORMAL (JIT) becomes INTERP
        (["0"], lines(0, "JIT allowed", "the JIT asked for executable memory"), lines(5, *interp)),   # NONE
        (["2"], lines(2, "JIT allowed", "the JIT asked for executable memory"), lines(5, *interp)),   # HYBRID
        (["3"], lines(3, *aot_llvm), lines(5, *interp)),                  # FULL: the interpreter takes the rest
        (["4"], lines(4, *aot_llvm), lines(6, *interp)),                  # LLVMONLY becomes INTERP_LLVMONLY
        (["5"], lines(5, *interp), lines(5, *interp)),                    # the interpreter's own modes stay
        (["7"], lines(7, *interp), lines(7, *interp)),
        (["8"], lines(8, "JIT allowed", "the JIT asked for executable memory"), lines(5, *interp)),   # INTERP_ONLY
        (["1", "dlsym-mono"], android, lines(5, *interp)),
        (["1", "dlsym-notmono"], plain, plain),                           # not Mono: left alone
    )
    failures = []
    for arguments, before, after in cases:
        for libraries, expected_lines, label in ((without, before, "without the library"),
                                                 (system, after if has_library else before, "with the library")):
            result = run(arguments, libraries)
            if result.returncode != 0 or result.stdout.splitlines() != expected_lines:
                failures.append(f"{' '.join(arguments) or 'import'} {label}: exit status {result.returncode}, stdout "
                                f"{result.stdout.splitlines()}, expected {expected_lines}, stderr's end "
                                f"{result.stderr.splitlines()[-5:]}")
    if not has_library:
        failures.append(f"{LIBRARY} is not built (tools/build-bionic.sh): the switch has nothing to run")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        sys.exit(1)
    print("mono switch: Mono's mode becomes the interpreter's by import and by dlsym, a library that is not Mono "
          "is left alone")


if __name__ == "__main__":
    main()
