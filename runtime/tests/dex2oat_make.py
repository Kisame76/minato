#!/usr/bin/env python3
"""Makes a verify odex with our dex2oat (M6 c1): apk-run runs dex2oat as /apex/com.android.art/bin/dex2oat64
imageless, with the options apk-runner-35's dex2oat64 took for the oat of fixtures/oat (--compiler-filter=verify,
--base=0x70000000, our boot jars as -Xbootclasspath), on <dex-file>, recording <dex-location> as the dex file's place,
which ART compares with the APK's run-time path. make_odex() is the library call (dex2oat_verify.py,
tools/oat-status-check.sh); the command line writes <out>.odex and <out>.vdex.

Usage: dex2oat_make.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <dex-file>
                       <dex-location> <out> [--debug] [--filter <compiler-filter>] [--framework <framework.jar>]
With --framework the odex is made for an app's boot class path (framework.jar included), as an app's start has it.
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import art_run  # noqa: E402


# The boot class path of an app's start (runtime/src/environment.cpp): an odex is accepted for the filters that depend
# on the boot image only if its recorded boot class path is a prefix of the runtime's, so an app's odex is made with
# all of it.
APP_BOOT_CLASS_PATH = [
    "/apex/com.android.art/javalib/core-oj.jar", "/apex/com.android.art/javalib/core-libart.jar",
    "/apex/com.android.art/javalib/okhttp.jar", "/apex/com.android.art/javalib/bouncycastle.jar",
    "/system/framework/framework.jar", "/apex/com.android.art/javalib/core-icu4j.jar",
    "/apex/com.android.conscrypt/javalib/conscrypt.jar"]


# The class loader context of an app's loader (ClassLoaderContext's text): its own PathClassLoader with no other dex
# files, below the parent loader the framework makes for it. ART runs an odex executable only if the context it was
# made for equals the loader's ("PCL[];PCL[]" in the log of ClassLoaderContext's check at the app's start).
APP_CLASS_LOADER_CONTEXT = "PCL[];PCL[]"


def make_odex(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, dex_file, dex_location,
              debug=False, compiler_filter="verify", framework_jar=None):
    """Runs dex2oat on `dex_file` as `dex_location` with --compiler-filter=`compiler_filter`; (CompletedProcess, the
    run folder's data/local/tmp). The odex is app.odex and the vdex app.vdex there. Each call has a run folder of its
    own, so calls side by side do not meet; discard() removes it. With `framework_jar` the boot class path is an app's
    (APP_BOOT_CLASS_PATH), framework.jar mounted at /system/framework, instead of the three core jars."""
    program = "dex2oatd" if debug else "dex2oat"
    work = os.path.abspath(work)
    art_lib64 = art_run.convert_guest(art, bionic_lib, apk_convert, work, debug)
    container = os.path.join(work, "bin", f"{program}-64.dylib")
    with art_run.convert_lock(work):
        art_run.convert(apk_convert, os.path.join(art, "bin", program), container)
    run = tempfile.mkdtemp(prefix="dex2oat-run-", dir=work)
    art_run.make_run_folder(run, icu_data, bionic_system,
                            os.path.join(art, "javalib", "conscrypt.jar") if framework_jar else None)
    produced = os.path.join(run, "data", "local", "tmp")
    shutil.copy(dex_file, os.path.join(produced, "app.jar"))
    if framework_jar:
        boot_class_path = ":".join(APP_BOOT_CLASS_PATH)
    else:
        boot_class_path = ":".join(f"{art_run.ART_ROOT}/javalib/{name}.jar" for name in art_run.BOOT_JARS)
    arguments = ["--dex-file=/data/local/tmp/app.jar", f"--dex-location={dex_location}",
                 "--oat-file=/data/local/tmp/app.odex", "--oat-location=/data/local/tmp/app.odex",
                 "--instruction-set=arm64", f"--compiler-filter={compiler_filter}", "--base=0x70000000",
                 "--runtime-arg", f"-Xbootclasspath:{boot_class_path}",
                 "--runtime-arg", f"-Xbootclasspath-locations:{boot_class_path}", "--runtime-arg", "-Xnoimage-dex2oat"]
    if framework_jar:
        arguments += [f"--class-loader-context={APP_CLASS_LOADER_CONTEXT}"]
    command = art_run.apk_run_command(apk_run, art, bionic_system, work, art_lib64, run, arguments)
    if framework_jar:
        framework = os.path.join(run, "system-framework")
        os.makedirs(framework)
        os.link(framework_jar, os.path.join(framework, "framework.jar"))
        command[1:1] = ["--mount", f"/system/framework={framework}"]
    at = command.index("--argv0")
    command[at + 1:at + 3] = [f"{art_run.ART_ROOT}/bin/dex2oat64", container]
    result = subprocess.run(command, capture_output=True, text=True, timeout=600, stdin=subprocess.DEVNULL)
    return result, produced


def discard(produced):
    """Removes the run folder of the make_odex() call that returned `produced`."""
    shutil.rmtree(os.path.dirname(os.path.dirname(os.path.dirname(produced))), ignore_errors=True)


def main():
    arguments = [argument for argument in sys.argv[1:] if argument != "--debug"]
    options = {}
    for option in ("--filter", "--framework"):
        if option in arguments:
            at = arguments.index(option)
            if at + 1 >= len(arguments):
                raise SystemExit(__doc__)
            options[option] = arguments[at + 1]
            del arguments[at:at + 2]
    if len(arguments) != 10:
        raise SystemExit(__doc__)
    art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, dex_file, dex_location, out = arguments
    result, produced = make_odex(art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, dex_file,
                                 dex_location, "--debug" in sys.argv[1:],
                                 options.get("--filter", "verify"), options.get("--framework"))
    if result.returncode != 0:
        print(f"dex2oat exited with {result.returncode}\n{result.stderr[-3000:]}")
        return 1
    shutil.copy(os.path.join(produced, "app.odex"), out + ".odex")
    shutil.copy(os.path.join(produced, "app.vdex"), out + ".vdex")
    discard(produced)
    return 0


if __name__ == "__main__":
    sys.exit(main())
