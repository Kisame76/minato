#!/usr/bin/env python3
"""dex2oat as guest code (M6 c1): apk-run runs dex2oat as /apex/com.android.art/bin/dex2oat64, the way it runs on
Android 15, with the files art_run.py gives dalvikvm. `dex2oat64 --help` is not an option of dex2oat: AOSP's Usage()
logs the usage and ends with exit(EXIT_FAILURE) (dex2oat.cc), so the program starts when it exits with 1 and the
usage names --compiler-filter. --debug runs dex2oatd with ART's debug flavor.
Prints "skipped: ..." while ART is not built at all (no dalvikvm), and fails while only dex2oat is missing, as a build
that leaves it out is the failure this test is for.

Usage: dex2oat_starts.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> [--debug]
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import art_run  # noqa: E402


def main():
    arguments = [argument for argument in sys.argv[1:] if argument != "--debug"]
    debug = "--debug" in sys.argv[1:]
    if len(arguments) != 7:
        raise SystemExit(__doc__)
    art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work = arguments
    program = "dex2oatd" if debug else "dex2oat"
    if not os.path.exists(os.path.join(art, "bin", "dalvikvm")):
        print(f"skipped: {art}/bin/dalvikvm is not built (tools/build-art.sh)")
        return 0
    if not os.path.exists(os.path.join(art, "bin", program)):
        print(f"{art}/bin/{program} is not built: tools/build-art.sh must build it")
        return 1
    missing = art_run.missing_front_lib(bionic_lib)
    if missing:
        print(missing)
        return 0
    work = os.path.abspath(work)
    art_lib64 = art_run.convert_guest(art, bionic_lib, apk_convert, work, debug)
    # One container per flavor: convert() keeps a container that is newer than its ELF file, and the release and the
    # debug test share <work>, so a shared name would run the release program against the debug libraries.
    container = os.path.join(work, "bin", f"{program}-64.dylib")
    with art_run.convert_lock(work):
        art_run.convert(apk_convert, os.path.join(art, "bin", program), container)
    run = os.path.join(work, "dex2oat-run-debug" if debug else "dex2oat-run")
    art_run.make_run_folder(run, icu_data, bionic_system)
    command = art_run.apk_run_command(apk_run, art, bionic_system, work, art_lib64, run, ["--help"])
    at = command.index("--argv0")
    command[at + 1:at + 3] = [f"{art_run.ART_ROOT}/bin/dex2oat64", container]
    result = subprocess.run(command, capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    if result.returncode != 1 or "Usage: dex2oat [options]" not in result.stderr or \
            "--compiler-filter=" not in result.stderr:
        print(f"{program} --help: exit {result.returncode}, the usage does not name --compiler-filter\n"
              f"{result.stderr[-2000:]}")
        return 1
    print(f"{program} starts as a guest program and prints its usage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
