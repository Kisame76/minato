#!/usr/bin/env python3
"""libffi closures without executable memory (ADR-0072, runtime/android/ffi.c): the loader preloads libminato-ffi.so,
and ffi_closure_alloc, ffi_closure_free and ffi_prep_closure_loc of a library that defines the three become its pool
wrappers, so that a closure's code pointer is a trampoline in the signed library instead of memory the app made
executable, which the runtime refuses. apk-run runs the fixture ffi-closure-test (a program that makes closures as
ctypes does) against libfake-ffi.so, which stands in for libffi by its names and by the layout of its legacy closure
path (it asks for RWX memory with mmap, as libffi does): - import, without libminato-ffi.so in /system/lib64: the
allocation gets EACCES and the program says so (on Android it succeeds: fixtures/expected/ffi-closure-test.out); -
import, with it: fixtures/expected/ffi-closure-test.out, as Android prints it; - symbolic (libfake-ffi-sym.so, linked
-Bsymbolic, calls the three names from inside by direct branches, as Ren'Py's statically linked libffi does): a
relocation hook cannot see those calls; STATIC_HOOKED says whether the entry hook of the converter (ADR-0072 decision 3
b) exists yet. Prints "skipped: ..." while the fixture or the library is not built.

Usage: ffi_closures_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

FIXTURES = ["ffi-closure-test", "libfake-ffi.so", "libfake-ffi-sym.so"]
LIBRARY = "libminato-ffi.so"
STATIC_HOOKED = True
REFUSED = ["ffi-closure-test: closure 0: no executable memory"]


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
                   "./ffi-closure-test", "--env", "LD_LIBRARY_PATH=/data/local/tmp",
                   os.path.join(work, "ffi-closure-test.dylib"), *arguments]
        return subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)

    failures = []
    cases = (([], False, REFUSED, 1, "import without the library"),
             ([], True, android, 0, "import with the library"),
             (["symbolic"], True, android if STATIC_HOOKED else REFUSED, 0 if STATIC_HOOKED else 1,
              "symbolic with the library"),
             (["symbolic-dlsym"], True, android if STATIC_HOOKED else REFUSED, 0 if STATIC_HOOKED else 1,
              "symbolic, names looked up with dlsym"))
    for arguments, with_library, lines, status, label in cases:
        result = run(arguments, system if with_library else without)
        if result.returncode != status or result.stdout.splitlines() != lines:
            failures.append(f"{label}: exit status {result.returncode}, stdout {result.stdout.splitlines()}, expected "
                            f"{lines} (status {status}), stderr's end {result.stderr.splitlines()[-5:]}")
    if not has_library:
        failures.append(f"{LIBRARY} is not built (tools/build-bionic.sh): the pool has nothing to run")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        sys.exit(1)
    print("ffi closures: pooled trampolines for the imported names"
          + (", and for the calls inside the library" if STATIC_HOOKED else ""))


if __name__ == "__main__":
    main()
