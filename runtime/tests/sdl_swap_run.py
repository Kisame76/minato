#!/usr/bin/env python3
"""The SDL swap override (runtime/android/sdl.c): the loader preloads libminato-sdl.so, and SDL_GL_SwapWindow of a
library that also defines SDL_GL_GetCurrentContext (SDL 2) becomes the wrapper, by import and by dlsym alike. A swap
that fails while no context is current and the activity is paused (SDL's context backup at a pause, before the app has
seen the background events) answers 0, a dropped frame, instead of -1, which an app such as Ren'Py takes for a lost
display. apk-run runs the fixture sdl-swap-test, which flips through libfake-sdl2.so (imported, or opened with the
argument "dlsym") in five states, with libfake-activity-paused.so standing in for the runtime's minato_activity_paused:
  - without libminato-sdl.so in /system/lib64: fixtures/expected/sdl-swap-test.out, as Android prints it;
  - with it: only "paused, no context current" answers 0; a swap that fails with a context current, or while the
    activity runs, keeps SDL's -1.
The same five flips by sdl-static-swap-test, whose library has SDL 2 linked in and calls the swap's dynamic API stub
directly, as Ren'Py's librenpython.so does (fixtures/expected/sdl-static-swap-test.out): no import reaches that call,
so the runtime names libminato-sdl.so in SDL_DYNAMIC_API, SDL's own way to fill its table from another library. With
"import" the program calls the same stub through its import: both ways meet, and the swap still answers 0 once.
sdl-odd-swap-test's library has a swap stub of another shape: its table stays as SDL fills it, as on Android.
Prints "skipped: ..." while the fixture or the library is not built.

Usage: sdl_swap_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work> <expected>
"""
import fcntl
import os
import subprocess
import sys

FIXTURES = ["sdl-swap-test", "libfake-sdl2.so", "libfake-activity-paused.so", "sdl-static-swap-test",
            "libfake-sdl2-static.so", "sdl-odd-swap-test", "libfake-sdl2-odd.so"]
PROGRAMS = ["sdl-swap-test", "sdl-static-swap-test", "sdl-odd-swap-test"]
# What the runtime sets in the guest's environment for SDL 2's dynamic API.
DYNAMIC_API = "SDL_DYNAMIC_API=libminato-sdl.so"
LIBRARY = "libminato-sdl.so"


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
    outputs = {}
    for program in PROGRAMS:
        path = os.path.join(os.path.dirname(expected), program + ".out")
        with open(path) as file:
            android = file.read().splitlines()
        paused_none = [index for index, line in enumerate(android) if "paused, no context current:" in line]
        if len(paused_none) != 1:
            raise SystemExit(f"{path} lacks the one line of the paused flip without a context")
        overridden = list(android)
        overridden[paused_none[0]] = f"{program}: paused, no context current: swap 0"
        outputs[program] = (android, overridden)

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

    # The system's libraries without the override's.
    without = os.path.join(work, "system-lib64-without")
    os.makedirs(without, exist_ok=True)
    for name in os.listdir(system):
        link = os.path.join(without, name)
        if name != LIBRARY + ".dylib" and not os.path.lexists(link):
            os.symlink(os.path.join(system, name), link)
    has_library = os.path.exists(os.path.join(bionic_lib, LIBRARY))

    def run(program, arguments, libraries, environment):
        command = [apk_run, "--mount", f"/data/local/tmp={tmp}", "--mount", f"/system={bionic_system}",
                   "--libraries", f"/system/lib64={libraries}", "--cwd", "/data/local/tmp", "--argv0",
                   f"./{program}", "--env", "LD_LIBRARY_PATH=/data/local/tmp", *environment,
                   os.path.join(work, program + ".dylib"), *arguments]
        return subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60)

    failures = []
    cases = (("sdl-swap-test", []), ("sdl-swap-test", ["dlsym"]), ("sdl-static-swap-test", []),
             ("sdl-static-swap-test", ["import"]), ("sdl-odd-swap-test", []))
    for program, arguments in cases:
        android, overridden = outputs[program]
        if program == "sdl-odd-swap-test":
            overridden = android
        for libraries, environment, lines, label in (
                (without, [], android, "without the library"),
                (system, ["--env", DYNAMIC_API], overridden if has_library else android, "with the library")):
            result = run(program, arguments, libraries, environment)
            if result.returncode != 0 or result.stdout.splitlines() != lines:
                way = ' '.join(arguments) or ('import' if program == "sdl-swap-test" else 'direct call')
                failures.append(f"{program} {way} {label}: exit status "
                                f"{result.returncode}, stdout "
                                f"{result.stdout.splitlines()}, expected {lines}, stderr's end "
                                f"{result.stderr.splitlines()[-5:]}")
    if not has_library:
        failures.append(f"{LIBRARY} is not built (tools/build-bionic.sh): the override has nothing to run")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        sys.exit(1)
    print("sdl swap override: a swap that fails while paused with no context current answers 0, by import, by dlsym "
          "and through SDL's dynamic API, once where both meet; a stub of another shape and every other answer are "
          "SDL's")


if __name__ == "__main__":
    main()
