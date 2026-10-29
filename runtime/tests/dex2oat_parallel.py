#!/usr/bin/env python3
"""Calls of dex2oat_make.make_odex() side by side do not meet (M6 c2): CTest runs the compiled tests and
runtime.dex2oat_verify at once in one work folder, and each call needs a run folder of its own. Two calls in two threads
make the odex of two different jars into the same work folder; each odex must be what a call of its own makes, and the
two must differ. Prints "skipped: ..." while ART or a jar is missing.

Usage: dex2oat_parallel.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <jar-a> <jar-b>
                           [--debug]
"""
import concurrent.futures
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import art_run  # noqa: E402
import dex2oat_make  # noqa: E402


def odex_of(arguments, jar, debug):
    result, produced = dex2oat_make.make_odex(*arguments, jar, "/data/local/tmp/app.jar", debug, "speed")
    try:
        if result.returncode != 0:
            return None, f"dex2oat of {os.path.basename(jar)} exited with {result.returncode}\n{result.stderr[-1500:]}"
        with open(os.path.join(produced, "app.odex"), "rb") as handle:
            return handle.read(), None
    except FileNotFoundError as missing:
        return None, f"dex2oat of {os.path.basename(jar)}: {missing}"
    finally:
        dex2oat_make.discard(produced)


def main():
    debug = "--debug" in sys.argv[1:]
    arguments = [argument for argument in sys.argv[1:] if argument != "--debug"]
    if len(arguments) != 9:
        raise SystemExit(__doc__)
    art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, jar_a, jar_b = arguments
    program = "dex2oatd" if debug else "dex2oat"
    for path in (os.path.join(art, "bin", "dalvikvm"), jar_a, jar_b):
        if not os.path.exists(path):
            print(f"skipped: {path} is not built")
            return 0
    missing = art_run.missing_front_lib(bionic_lib)
    if missing:
        print(missing)
        return 0
    if not os.path.exists(os.path.join(art, "bin", program)):
        print(f"{art}/bin/{program} is not built: tools/build-art.sh must build it")
        return 1
    work = os.path.abspath(work)
    fixed = (art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work)
    # The conversions of ART's libraries and of dex2oat, which the calls share, happen once before the threads.
    alone = {jar: odex_of(fixed, jar, debug) for jar in (jar_a, jar_b)}
    for jar, (odex, failure) in alone.items():
        if failure:
            print(failure)
            return 1
    with concurrent.futures.ThreadPoolExecutor(2) as pool:
        side = {jar: pool.submit(odex_of, fixed, jar, debug) for jar in (jar_a, jar_b)}
        together = {jar: future.result() for jar, future in side.items()}
    for jar, (odex, failure) in together.items():
        if failure:
            print(f"side by side: {failure}")
            return 1
        if odex != alone[jar][0]:
            print(f"side by side, {os.path.basename(jar)}'s odex is not the one a call of its own makes")
            return 1
    if together[jar_a][0] == together[jar_b][0]:
        print("the two jars made the same odex")
        return 1
    print(f"two {program} calls side by side make the odex of their own jar")
    return 0


if __name__ == "__main__":
    sys.exit(main())
