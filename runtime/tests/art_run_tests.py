#!/usr/bin/env python3
"""ART's run-tests on ART as guest code (ADR-0033 test 3): the tests of fixtures/art-run-tests.txt, built and given
dalvikvm's arguments by runtime/art/run_tests.py as ART's harness does on a device, each run as art_run.py runs a
program, in its own DEX_LOCATION, which is also ANDROID_DATA and the working directory, as on the device. A test
passes when it ends with its expected exit status within run_tests.TIMEOUT and prints its reference: AOSP's
expected-stdout.txt and expected-stderr.txt, or Android 15's own output (fixtures/expected/art-run-tests). stderr is
compared without the lines of the guest's logger, which on Android go to logcat.
The list must hold exactly the subset of run_tests.py's rules. --sample runs the tests the list marks "sample",
otherwise the named tests or all but the "out" ones. --debug runs ART's debug flavor, --gc <collector> adds
-Xgc:<collector>, --compiled makes each jar's oat with our dex2oat at --compiler-filter=speed and runs the test
with it under nterp (M6 c2; it sets MINATO_ART_NTERP=1 for run_tests.py's arguments), --jobs runs that many
tests side by side (one by default), --times <file> writes each test's seconds. The run folder of a failed test
stays in <work>/run-tests.
Prints "skipped: ..." while dalvikvm, the boot jars' classes, the JDK, D8, the run-tests' sources, the guest
system's list of public libraries or the graphics front libs, which every VM start preloads, are missing.

Usage: art_run_tests.py <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <root>
                        [--sample] [--debug] [--compiled] [--gc <collector>] [--jobs <n>] [--times <file>] [<test>...]
<root> is the repository, with build/sources, build/tools/jdk-17 and fixtures.
"""
import concurrent.futures
import difflib
import glob
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "art"))
import art_run  # noqa: E402
import dex2oat_make  # noqa: E402
import run_tests  # noqa: E402

# A line of the guest's logger on stderr: "<priority>/<tag>: <message>" (bionic's liblog.cpp).
LOGGER_LINE = re.compile(r"^[VDIWEFS]/[^:\n]*: .*(\n|$)", re.MULTILINE)


def read(path):
    if not os.path.exists(path):
        return ""
    with open(path, errors="replace") as file:
        return file.read()


def differences(name, wanted, got, lines=12):
    """The first `lines` lines of a unified diff of `wanted` and `got`."""
    diff = list(difflib.unified_diff(wanted.splitlines(), got.splitlines(), f"{name} expected", f"{name} printed",
                                     lineterm="", n=1))
    return diff[:lines] + ([f"... {len(diff) - lines} more lines"] if len(diff) > lines else [])


def make_oats(test, setting, run, host_location, location):
    """The oat of each jar of `test` made with our dex2oat at --compiler-filter=speed, in oat/arm64 next to the jars
    as ART looks for it, and the containers that --libraries gives ART's dlopen as those odex files; (failure or None,
    apk-run's arguments)."""
    work, debug = setting["work"], setting["debug"]
    oat = os.path.join(host_location, "oat", "arm64")
    containers = os.path.join(work, "oat-containers", test + ("-debug" if debug else ""))
    shutil.rmtree(containers, ignore_errors=True)
    os.makedirs(oat)
    os.makedirs(containers)
    for name in run_tests.jar_names(setting["tests"], test):
        stem = os.path.splitext(name)[0]
        result, produced = dex2oat_make.make_odex(
            setting["art"], setting["bionic_lib"], setting["bionic_system"], setting["icu_data"],
            setting["apk_convert"], setting["apk_run"], work, os.path.join(host_location, name),
            f"{location}/{name}", debug, "speed")
        try:
            if result.returncode != 0:
                return f"{test}: dex2oat of {name} exited with {result.returncode}", []
            shutil.copy(os.path.join(produced, "app.odex"), os.path.join(oat, stem + ".odex"))
            shutil.copy(os.path.join(produced, "app.vdex"), os.path.join(oat, stem + ".vdex"))
        finally:
            dex2oat_make.discard(produced)
        art_run.convert(setting["apk_convert"], os.path.join(oat, stem + ".odex"),
                        os.path.join(containers, stem + ".odex.dylib"))
    return None, ["--libraries", f"{location}/oat/arm64={containers}"]


def run_test(test, reference, setting):
    """Runs `test`; returns (failure lines or [], seconds)."""
    tests, work, debug, extra = setting["tests"], setting["work"], setting["debug"], setting["extra"]
    run = os.path.join(work, "run-tests", test + ("-debug" if debug else "") + "".join(extra).replace(":", "-"))
    art_run.make_run_folder(run, setting["icu_data"], setting["bionic_system"])
    location = run_tests.dex_location(test)
    host_location = os.path.join(run, location.lstrip("/"))  # /data is the run folder's data
    os.makedirs(os.path.join(host_location, "dalvik-cache", "arm64"))
    for name in run_tests.jar_names(tests, test):
        # Read-only, as on the device (art_run.py says why).
        copied = shutil.copy(os.path.join(work, "jars", test, name), host_location)
        os.chmod(copied, 0o444)
        # The run's sync (run.py): the jar written back before the guest maps it, so its pages are clean
        # (/proc/self/smaps, 071-dexfile-map-clean). fsync, since the host's sync may return before the writes end.
        if run_tests.run_options(tests, test).get("sync"):
            descriptor = os.open(copied, os.O_RDONLY)
            os.fsync(descriptor)
            os.close(descriptor)
    boot_class_path = ":".join(f"{art_run.ART_ROOT}/javalib/{name}.jar" for name in art_run.BOOT_JARS)
    libraries = []
    if setting["compiled"]:
        oat_failure, libraries = make_oats(test, setting, run, host_location, location)
        if oat_failure:
            return [oat_failure], 0.0
    command = art_run.apk_run_command(
        setting["apk_run"], setting["art"], setting["bionic_system"], work, setting["art_lib64"], run,
        guest_arguments(tests, test, boot_class_path, debug, extra), cwd=location, jni=libraries,
        environment=[f"ANDROID_DATA={location}", f"DEX_LOCATION={location}"])
    started = time.monotonic()
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=run_tests.TIMEOUT)
    except subprocess.TimeoutExpired:
        return [f"{test}: no end within {run_tests.TIMEOUT} s"], time.monotonic() - started
    seconds = time.monotonic() - started
    if reference == "android":
        own = os.path.join(setting["root"], "fixtures", "expected", "art-run-tests", test)
        wanted_out, wanted_err = read(own + ".out"), read(own + ".err")
    else:
        wanted_out = read(os.path.join(tests, test, "expected-stdout.txt"))
        wanted_err = read(os.path.join(tests, test, "expected-stderr.txt"))
    status, should = result.returncode, run_tests.expected_status(tests, test)
    stderr = LOGGER_LINE.sub("", result.stderr)
    if status == should and result.stdout == wanted_out and stderr == wanted_err:
        shutil.rmtree(run, ignore_errors=True)
        return [], seconds
    failure = [f"{test}: exit status {status}" + (f", expected {should}" if status != should else "")]
    if result.stdout != wanted_out:
        failure += differences("stdout", wanted_out, result.stdout)
    if stderr != wanted_err:
        failure += differences("stderr", wanted_err, stderr)
    failure += ["the guest's log, last lines:"] + ["  " + line for line in result.stderr.splitlines()[-8:]]
    return failure, seconds


# A loaded Mac is slower than a phone: the FinalizerWatchdog's 10 s (Android's default) aborted 080-oom-throw at load 25.
# It comes before the test's own runtime options, so 1336-short-finalizer-timeout keeps its 500 ms.
FINALIZER_TIMEOUT = "-XX:FinalizerTimeoutMs=60000"


def guest_arguments(tests, test, boot_class_path, debug=False, extra=()):
    """dalvikvm's arguments for `test` as the Mac harness runs it: run_tests.py's, with the longer finalizer timeout."""
    return run_tests.dalvikvm_arguments(tests, test, boot_class_path, debug, [FINALIZER_TIMEOUT, *extra])


def main():
    arguments = sys.argv[1:]
    flags = {flag for flag in ("--sample", "--debug", "--compiled") if flag in arguments}
    arguments = [argument for argument in arguments if argument not in flags]
    values = {}
    for option in ("--gc", "--jobs", "--times"):
        if option in arguments:
            at = arguments.index(option)
            if at + 1 >= len(arguments):
                raise SystemExit(__doc__)
            values[option] = arguments[at + 1]
            del arguments[at:at + 2]
    if len(arguments) < 8:
        raise SystemExit(__doc__)
    art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, root = arguments[:8]
    named = arguments[8:]
    debug = "--debug" in flags
    if "--compiled" in flags:
        os.environ["MINATO_ART_NTERP"] = "1"  # no -Xint: compiled code runs, and nterp for the rest
    sources = os.path.join(root, "build", "sources")
    tests = os.path.join(sources, "art", "test")
    java = os.path.join(art, "java")
    jdks = sorted(glob.glob(os.path.join(root, "build", "tools", "jdk-17", "*", "Contents", "Home")))
    r8 = os.path.join(sources, "r8", "r8.jar")
    for needed, built_by in ((os.path.join(art, "bin", "dalvikvm"), "tools/build-art.sh"),
                             (os.path.join(java, "all-classes.jar"), "tools/build-art.sh"),
                             (os.path.join(tests, "knownfailures.json"), "tools/fetch-sources.py --set art"),
                             (r8, "tools/fetch-sources.py --set art"),
                             (os.path.join(bionic_system, "etc", "public.libraries.txt"), "tools/build-bionic.sh"),
                             *((os.path.join(art_run.front_libs(bionic_lib), name), "tools/build-front-libs.sh")
                               for name in art_run.FRONT_LIBS)):
        if not os.path.exists(needed):
            print(f"skipped: {needed} is not there ({built_by})")
            return
    if not jdks:
        print("skipped: no JDK 17 in build/tools/jdk-17 (tools/fetch-sources.py --set art)")
        return

    list_path = os.path.join(root, "fixtures", "art-run-tests.txt")
    included = run_tests.subset(tests)[0]
    if os.path.exists(list_path):
        entries = run_tests.read_list(list_path)
    else:
        # Before Android 15's first run: every test of the subset against AOSP's expected output.
        print(f"no {list_path} yet: AOSP's expected output is the reference")
        entries = {test: ("aosp", "") for test in included}
    if sorted(entries) != included:
        print(f"FAIL: {list_path} is not the subset of the rules of runtime/art/run_tests.py: "
              f"{len(set(included) - set(entries))} tests missing, {len(set(entries) - set(included))} too many; "
              "run tools/android-reference.sh --art-run-tests")
        sys.exit(1)
    unknown = [test for test in named if test not in entries]
    if unknown:
        raise SystemExit(f"not in {list_path}: {' '.join(unknown)}")
    chosen = named or [test for test, (reference, rest) in sorted(entries.items())
                       if reference != "out" and ("--sample" not in flags or rest == "sample")]
    if not chosen:
        print(f"skipped: {list_path} marks no sample")
        return

    jobs = int(values.get("--jobs", "1"))
    errors = run_tests.build_all(tests, chosen, jdks[-1], r8, java, os.path.join(work, "jars"), jobs)
    extra = [f"-Xgc:{values['--gc']}"] if "--gc" in values else []
    setting = {"tests": tests, "work": os.path.abspath(work), "debug": debug, "extra": extra, "root": root,
               "art": art, "bionic_system": bionic_system, "icu_data": icu_data, "apk_run": apk_run,
               "art_lib64": art_run.convert_guest(art, bionic_lib, apk_convert, work, debug),
               "compiled": "--compiled" in flags, "bionic_lib": bionic_lib, "apk_convert": apk_convert}
    failures = [[f"{test}: the build failed:"] + error.splitlines()[:12] for test, error in sorted(errors.items())]
    times = {}
    with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as pool:
        futures = {pool.submit(run_test, test, entries[test][0], setting): test for test in chosen
                   if test not in errors}
        for future in concurrent.futures.as_completed(futures):
            failure, times[futures[future]] = future.result()
            if failure:
                failures.append(failure)
    if "--times" in values:
        with open(values["--times"], "w") as file:
            file.writelines(f"{test} {times[test]:.2f}\n" for test in sorted(times))
    how = ("the sample" if "--sample" in flags else "the run-tests") + (" in the debug flavor" if debug else "") + (
        " compiled" if "--compiled" in flags else "") + (
        f" with {extra[0]}" if extra else "")
    if failures:
        for failure in sorted(failures):
            print("FAIL: " + failure[0])
            for line in failure[1:]:
                print("    " + line)
        print(f"{len(chosen) - len(failures)} of {len(chosen)} of {how} print what Android 15 prints")
        sys.exit(1)
    print(f"{len(chosen)} of {how} print what Android 15 prints, the slowest in {max(times.values(), default=0):.1f} s")


if __name__ == "__main__":
    main()
