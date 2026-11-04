#!/usr/bin/env python3
"""The LuaJIT switch (runtime/android/luajit.c): the loader preloads libminato-luajit.so, and luaL_openlibs of a library
that also defines luaJIT_setmode (LuaJIT) becomes the wrapper, by import and by dlsym alike, which calls the library's
luaL_openlibs and then luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF), so that the trace compiler never asks
for executable memory. apk-run runs the fixture luajit-switch-test, which starts a state through libfake-luajit.so
(imported, or opened with the argument "dlsym-luajit") or libfake-lua.so ("dlsym-lua", plain Lua without
luaJIT_setmode):
  - without libminato-luajit.so in /system/lib64, all three: fixtures/expected/luajit-switch-test.out, and the same with
    "plain Lua" for the plain library, as Android prints them;
  - with it: LuaJIT's engine is switched off after luaL_openlibs (the mode 0 reaches luaJIT_setmode, the report says
    "JIT engine off"), plain Lua is left alone.
Prints "skipped: ..." while the fixture or the library is not built.

Usage: luajit_switch_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

FIXTURES = ["luajit-switch-test", "libfake-luajit.so", "libfake-lua.so"]
LIBRARY = "libminato-luajit.so"


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
        luajit = file.read().splitlines()
    plain = luajit[:-2] + ["luajit-switch-test: plain Lua", "luajit-switch-test: no trace compiler"]
    # The timing that decides it: the engine is off, and the trace compiler asks for nothing, before the app's first
    # loop.
    switched = luajit[:-2] + ["luajit-switch-test: luaJIT_setmode(L, 0, 0)", "luajit-switch-test: JIT engine off",
                              "luajit-switch-test: the trace compiler asked for nothing"]

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
                   "./luajit-switch-test", "--env", "LD_LIBRARY_PATH=/data/local/tmp",
                   os.path.join(work, "luajit-switch-test.dylib"), *arguments]
        return subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)

    failures = []
    cases = ((([], luajit, switched), (["dlsym-luajit"], luajit, switched), (["dlsym-lua"], plain, plain)))
    for arguments, android, with_switch in cases:
        for libraries, lines, label in ((without, android, "without the library"),
                                        (system, with_switch if has_library else android, "with the library")):
            result = run(arguments, libraries)
            if result.returncode != 0 or result.stdout.splitlines() != lines:
                failures.append(f"{' '.join(arguments) or 'import'} {label}: exit status {result.returncode}, stdout "
                                f"{result.stdout.splitlines()}, expected {lines}, stderr's end "
                                f"{result.stderr.splitlines()[-5:]}")
    if not has_library:
        failures.append(f"{LIBRARY} is not built (tools/build-bionic.sh): the switch has nothing to run")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        sys.exit(1)
    print("luajit switch: LuaJIT's engine switched off after luaL_openlibs by import and by dlsym, "
          "plain Lua left alone")


if __name__ == "__main__":
    main()
