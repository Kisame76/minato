#!/usr/bin/env python3
"""The loader's resolve log (ADR-0066 step 0): with MINATO_RESOLVE_LOG=<prefixes> in the program's environment, the
loader writes a line in linker64's log for each import and each dlsym that finds a name with one of the prefixes, so
that an iPhone sitting learns how an engine reaches its scripting runtime's API without a look into its libraries.
apk-run runs the fixture frame-cap-test, whose engine library imports il2cpp_add_internal_call and looks up
mono_add_internal_call with dlsym, three times:
  - without the variable: the fixture's output and no resolve line;
  - with il2cpp_,mono_: the same output, and the import's line, then the dlsym's;
  - with mono_ alone: the dlsym's line alone.
Prints "skipped: ..." while the fixture is not built.

Usage: resolve_log_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work>
"""
import fcntl
import os
import subprocess
import sys

# fixtures/expected/frame-cap-test.out
OUTPUT = [
    "frame-cap-test: 30 fps before the app sets a rate",
    "frame-cap-test: the app set 60, the getter answers 60, the engine runs at 60 fps",
    "frame-cap-test: the app set -1, the getter answers -1, the engine runs at 30 fps",
    "frame-cap-test: the app set 240, the getter answers 240, the engine runs at 240 fps",
]
IMPORT = "D/linker  : import il2cpp_add_internal_call by libframe-cap-engine.so: libframe-cap-api.so"
DLSYM = "D/linker  : dlsym mono_add_internal_call by libframe-cap-engine.so: libframe-cap-api.so"
FIXTURES = ["frame-cap-test", "libframe-cap-api.so", "libframe-cap-engine.so"]


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
    for name in FIXTURES:
        if not os.path.exists(os.path.join(fixtures, name)):
            print(f"skipped: {os.path.join(fixtures, name)} is not built (tools/build-fixtures.sh)")
            return

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

    failures = []
    for prefixes, expected in ((None, []), ("il2cpp_,mono_", [IMPORT, DLSYM]), ("mono_", [DLSYM])):
        command = [apk_run, "--mount", f"/data/local/tmp={tmp}", "--mount", f"/system={bionic_system}",
                   "--libraries", f"/system/lib64={system}", "--cwd", "/data/local/tmp", "--argv0", "./frame-cap-test",
                   "--env", "LD_LIBRARY_PATH=/data/local/tmp"]
        if prefixes is not None:
            command += ["--env", f"MINATO_RESOLVE_LOG={prefixes}"]
        result = subprocess.run(command + [os.path.join(work, "frame-cap-test.dylib")], capture_output=True, text=True,
                                errors="replace", timeout=60)
        lines = [line for line in result.stderr.splitlines()
                 if line.startswith(("D/linker  : import ", "D/linker  : dlsym "))]
        if result.returncode != 0 or result.stdout.splitlines() != OUTPUT or lines != expected:
            failures.append(f"MINATO_RESOLVE_LOG={prefixes}: exit status {result.returncode}, stdout "
                            f"{result.stdout.splitlines()}, resolve lines {lines}, expected {expected}, "
                            f"stderr's end {result.stderr.splitlines()[-5:]}")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        sys.exit(1)
    print("resolve log: the imports and dlsyms of the watched names, only with MINATO_RESOLVE_LOG")


if __name__ == "__main__":
    main()
