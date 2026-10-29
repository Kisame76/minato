#!/usr/bin/env python3
"""Makes a verify odex with our dex2oat (M6 c1): apk-run runs dex2oat as /apex/com.android.art/bin/dex2oat64
imageless, with the options apk-runner-35's dex2oat64 took for the oat of fixtures/oat (--compiler-filter=verify,
--base=0x70000000, our boot jars as -Xbootclasspath), on <dex-file>, recording <dex-location> as the dex file's place,
which ART compares with the APK's run-time path. make_odex() is the library call (dex2oat_verify.py,
tools/oat-status-check.sh); the command line writes <out>.odex and <out>.vdex.

Usage: dex2oat_make.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <dex-file>
                       <dex-location> <out> [--debug]
"""
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import art_run  # noqa: E402


def make_odex(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, dex_file, dex_location,
              debug=False):
    """Runs dex2oat on `dex_file` as `dex_location`; (CompletedProcess, the run folder's data/local/tmp). The odex is
    app.odex and the vdex app.vdex there."""
    program = "dex2oatd" if debug else "dex2oat"
    work = os.path.abspath(work)
    art_lib64 = art_run.convert_guest(art, bionic_lib, apk_convert, work, debug)
    container = os.path.join(work, "bin", f"{program}-64.dylib")
    with art_run.convert_lock(work):
        art_run.convert(apk_convert, os.path.join(art, "bin", program), container)
    run = os.path.join(work, "dex2oat-verify-run-debug" if debug else "dex2oat-verify-run")
    art_run.make_run_folder(run, icu_data, bionic_system)
    produced = os.path.join(run, "data", "local", "tmp")
    shutil.copy(dex_file, os.path.join(produced, "app.jar"))
    boot_class_path = ":".join(f"{art_run.ART_ROOT}/javalib/{name}.jar" for name in art_run.BOOT_JARS)
    arguments = ["--dex-file=/data/local/tmp/app.jar", f"--dex-location={dex_location}",
                 "--oat-file=/data/local/tmp/app.odex", "--oat-location=/data/local/tmp/app.odex",
                 "--instruction-set=arm64", "--compiler-filter=verify", "--base=0x70000000",
                 "--runtime-arg", f"-Xbootclasspath:{boot_class_path}",
                 "--runtime-arg", f"-Xbootclasspath-locations:{boot_class_path}", "--runtime-arg", "-Xnoimage-dex2oat"]
    command = art_run.apk_run_command(apk_run, art, bionic_system, work, art_lib64, run, arguments)
    at = command.index("--argv0")
    command[at + 1:at + 3] = [f"{art_run.ART_ROOT}/bin/dex2oat64", container]
    result = subprocess.run(command, capture_output=True, text=True, timeout=600, stdin=subprocess.DEVNULL)
    return result, produced


def main():
    arguments = [argument for argument in sys.argv[1:] if argument != "--debug"]
    if len(arguments) != 10:
        raise SystemExit(__doc__)
    art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, dex_file, dex_location, out = arguments
    result, produced = make_odex(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, dex_file,
                                 dex_location, "--debug" in sys.argv[1:])
    if result.returncode != 0:
        print(f"dex2oat exited with {result.returncode}\n{result.stderr[-3000:]}")
        return 1
    shutil.copy(os.path.join(produced, "app.odex"), out + ".odex")
    shutil.copy(os.path.join(produced, "app.vdex"), out + ".vdex")
    return 0


if __name__ == "__main__":
    sys.exit(main())
