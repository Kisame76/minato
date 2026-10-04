#!/usr/bin/env python3
"""ART's tests in the app (ADR-0033 d): stages them as the app's ART mode reads them (tools/embed-fixtures.sh puts the
stage into fixture builds), runs a stage on the Mac as the app runs it, and checks the outputs of a run.

The stage, as agreed with App (docs/plans/adr-0033-d-steps.md):
  lib64-debug/<library>       links to ART's debug flavor, libartd.so under libart.so's name. The app registers their
                              containers for /apex/com.android.art/lib64 before the release flavor's, and the runtime
                              takes a library from the first folder that has it.
  tests/<test>/command.json   how the app starts the test in the release flavor; command-debug.json in the debug one
  tests/<test>/files/         what the app copies, read-only, into the guest folder the command names: the jars
  tests/<test>/libraries/     the test's JNI libraries, whose containers are that folder's libraries
  tests.json                  for the Mac: each test's kind and reference
A command is a JSON object:
  "arguments"          argv; argv[0] is /apex/com.android.art/bin/dalvikvm64
  "environment"        "NAME=value" entries on top of the shell's PATH, ANDROID_DATA, ANDROID_ROOT and HOME, each
                       replacing the shell's entry of its name, as apk-run's --env does
  "workingDirectory"   a guest path
  "folders"            guest folders under /data, made empty; every run also has /data/dalvik-cache/arm64 and
                       /data/local/tmp
  "files"              the guest folder of files/, under /data
  "libraries"          the guest folder of libraries/, absent without JNI libraries
The tests are the five Java fixtures of runtime/CMakeLists.txt (art_run.py's runs) and ART's run-tests of
fixtures/art-run-tests.txt (art_run_tests.py's), each staged with the arguments those scripts give dalvikvm.

`run` runs staged tests through apk-run with what the app's ART mode gives them: /apex as Android has it, Conscrypt's
APEX included; bionic's /system; a new /data per test; ART's release flavor, after lib64-debug in the debug flavor.
Each test's stdout, stderr, status and measures (apk-run --measures) go to <outputs>/<test>/, as the app leaves them
for tools/device-suite.sh.
`check` compares such outputs with the references: for a Java fixture its expected stdout, the status 0, that ART's
log names the heap window once, with 4 GiB or, with --upper-half, as without increased-memory-limit, 2 GiB, for
codec-test its offsets, and for stress-test a signal handled during a collection (art_run.py); for a run-test its stdout, its stderr without the logger's lines, and its exit
status (art_run_tests.py). `selftest` stages the Java fixtures and CTest's sample of the run-tests, runs them and
checks them, so the stage is right before the app reads it.

Prints "skipped: ..." while dalvikvm, a Java fixture's jar or library, the run-tests' sources, the JDK, D8 or the
guest system's list of public libraries is missing.

Usage: device_art.py stage <art> <fixtures-bin> <root> <out> [--sample]
       device_art.py run <out> <art> <bionic-lib> <bionic-system> <icu-data> <apk-convert> <apk-run> <work> <outputs>
                     [--debug] [<test>...]
       device_art.py check <out> <outputs> <root> [--upper-half] [<test>...]
       device_art.py selftest <art> <fixtures-bin> <root> <bionic-lib> <bionic-system> <icu-data> <apk-convert>
                     <apk-run> <work> [--debug]
<root> is the repository, with build/sources, build/tools/jdk-17 and fixtures.
"""
import concurrent.futures
import glob
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "art"))
import art_run  # noqa: E402
import art_run_tests  # noqa: E402
import run_tests  # noqa: E402

DALVIKVM = f"{art_run.ART_ROOT}/bin/dalvikvm64"
# ART's debug flavor, each library under the name lib64-debug gives it: libartd.so stands in for libart.so, which
# dalvikvm opens (art_run.py says why).
DEBUG_LIBRARIES = {"libart.so": "libartd.so", "libartbased.so": "libartbased.so", "libdexfiled.so": "libdexfiled.so",
                   "libopenjdkd.so": "libopenjdkd.so", "libopenjdkjvmd.so": "libopenjdkjvmd.so",
                   "libprofiled.so": "libprofiled.so"}
# The Java fixtures as runtime/CMakeLists.txt runs them with art_run.py: a JNI library, Conscrypt, the jars after
# core-libart, codec-test's offsets and stress-test's line of what it saw.
JAVA_FIXTURES = {
    "art-hello": {},
    "codec-test": {"library": "libcodec-test.so", "offsets": True},
    "conscrypt-test": {"conscrypt": True},
    "crypto-test": {"conscrypt": True, "added_jars": ["okhttp", "bouncycastle"]},
    "stress-test": {"library": "libstress-test.so", "stress": True},
}
MARKER = ".device-art"  # in every stage, so that nothing but a stage is ever cleared


def read_json(path):
    with open(path) as handle:
        return json.load(handle)


def write_json(path, value):
    with open(path, "w") as handle:
        json.dump(value, handle, indent=2)
        handle.write("\n")


def stage(art, fixtures_bin, root, out, sample=False):
    """Stages the Java fixtures and the run-tests of fixtures/art-run-tests.txt, or only CTest's sample of these, in
    `out`; returns why it cannot, or None. The run-tests' jars are built in <out>/.jars and stay from stage to stage."""
    tests_root = os.path.join(root, "build", "sources", "art", "test")
    for needed, built_by in ((os.path.join(art, "bin", "dalvikvm"), "tools/build-art.sh"),
                             (os.path.join(art, "java", "all-classes.jar"), "tools/build-art.sh"),
                             (os.path.join(tests_root, "knownfailures.json"), "tools/fetch-sources.py --set art")):
        if not os.path.exists(needed):
            return f"{needed} is not there ({built_by})"
    jdks = sorted(glob.glob(os.path.join(root, "build", "tools", "jdk-17", "*", "Contents", "Home")))
    r8 = os.path.join(root, "build", "sources", "r8", "r8.jar")
    if not jdks or not os.path.exists(r8):
        return "no JDK 17 in build/tools/jdk-17 or no D8 (tools/fetch-sources.py --set art)"
    fixtures = {}
    for name, flags in JAVA_FIXTURES.items():
        jar = os.path.join(art, "test", f"{name}.jar")
        library = os.path.join(fixtures_bin, flags["library"]) if "library" in flags else None
        for needed in [jar] + ([library] if library else []):
            if not os.path.exists(needed):
                return f"{needed} is not built (tools/build-art.sh, tools/build-fixtures.sh)"
        fixtures[name] = (jar, library, flags)
    entries = run_tests.read_list(os.path.join(root, "fixtures", "art-run-tests.txt"))
    chosen = [test for test, (reference, rest) in sorted(entries.items())
              if reference != "out" and (not sample or rest == "sample")]
    if not chosen:
        raise SystemExit("fixtures/art-run-tests.txt names no run-test" + (" for CTest's sample" if sample else ""))
    if os.path.exists(out) and not os.path.exists(os.path.join(out, MARKER)):
        raise SystemExit(f"{out} is not a stage of device_art.py, so it stays as it is")
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, MARKER), "w").close()
    errors = run_tests.build_all(tests_root, chosen, jdks[-1], r8, os.path.join(art, "java"),
                                 os.path.join(out, ".jars"), os.cpu_count() or 1)
    if errors:
        raise SystemExit("the run-tests' jars did not build: " + ", ".join(sorted(errors)))

    for part in ("tests", "lib64-debug"):
        shutil.rmtree(os.path.join(out, part), ignore_errors=True)
    os.makedirs(os.path.join(out, "lib64-debug"))
    for name, library in DEBUG_LIBRARIES.items():
        os.symlink(os.path.join(os.path.abspath(art), "lib", library), os.path.join(out, "lib64-debug", name))

    manifest = {}
    for name, (jar, library, flags) in fixtures.items():
        folder = os.path.join(out, "tests", name)
        os.makedirs(os.path.join(folder, "files"))
        shutil.copy(jar, os.path.join(folder, "files"))
        if library:
            # Also among the files: System.loadLibrary checks that the library's file is there.
            os.makedirs(os.path.join(folder, "libraries"))
            # With its time, so that `run` converts it only when the library is new.
            shutil.copy2(library, os.path.join(folder, "libraries"))
            shutil.copy(library, os.path.join(folder, "files"))
        for debug in (False, True):
            options = art_run.dalvikvm_options(debug, flags.get("conscrypt", False), flags.get("added_jars", ()),
                                               library is not None)
            command = {"arguments": [DALVIKVM] + options + ["-cp", f"/data/local/tmp/{os.path.basename(jar)}", "Main"],
                       "environment": [], "workingDirectory": "/", "folders": [], "files": "/data/local/tmp"}
            if library:
                command["libraries"] = "/data/local/tmp"
            write_json(os.path.join(folder, "command-debug.json" if debug else "command.json"), command)
        manifest[name] = {"kind": "java", "expected": f"fixtures/expected/{name}.out",
                          "offsets": flags.get("offsets", False), "stress": flags.get("stress", False)}

    # As art_run_tests.py runs them: in their own DEX_LOCATION, which is also ANDROID_DATA and the working directory.
    boot_class_path = ":".join(f"{art_run.ART_ROOT}/javalib/{name}.jar" for name in art_run.BOOT_JARS)
    for test in chosen:
        folder = os.path.join(out, "tests", test)
        os.makedirs(os.path.join(folder, "files"))
        for jar in run_tests.jar_names(tests_root, test):
            shutil.copy(os.path.join(out, ".jars", test, jar), os.path.join(folder, "files"))
        location = run_tests.dex_location(test)
        for debug in (False, True):
            command = {"arguments": [DALVIKVM] + run_tests.dalvikvm_arguments(tests_root, test, boot_class_path, debug),
                       "environment": [f"ANDROID_DATA={location}", f"DEX_LOCATION={location}"],
                       "workingDirectory": location, "folders": [f"{location}/dalvik-cache/arm64"],
                       "files": location}
            write_json(os.path.join(folder, "command-debug.json" if debug else "command.json"), command)
        manifest[test] = {"kind": "run-test", "reference": entries[test][0], "sample": entries[test][1] == "sample"}
    write_json(os.path.join(out, "tests.json"), manifest)
    return None


def run(out, art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, outputs, debug=False, names=(),
        jobs=1):
    """Runs the staged tests `names`, or all of them, as the app's ART mode runs them; returns the tests that ran.
    Each test's stdout, stderr, status and measures go to <outputs>/<test>/; its run folder stays only when it did
    not end in time."""
    manifest = read_json(os.path.join(out, "tests.json"))
    names = list(names) or sorted(manifest)
    art_lib64 = art_run.convert_guest(art, bionic_lib, apk_convert, work, False)
    first = None
    if debug:
        first = os.path.join(work, "stage-lib64-debug")
        with art_run.convert_lock(work):
            art_run.convert_directory(apk_convert, os.path.join(out, "lib64-debug"), first)
    conscrypt_jar = os.path.join(art, "javalib", "conscrypt.jar")

    def one(test):
        folder = os.path.join(out, "tests", test)
        command = read_json(os.path.join(folder, "command-debug.json" if debug else "command.json"))
        if command["arguments"][0] != DALVIKVM:
            raise SystemExit(f"{test}: argv[0] is {command['arguments'][0]}, not {DALVIKVM}")
        run_folder = os.path.join(work, "device-art-runs", test + ("-debug" if debug else ""))
        art_run.make_run_folder(run_folder, icu_data, bionic_system, conscrypt_jar)
        data = os.path.join(run_folder, "data")
        for guest in command["folders"] + [command["files"]]:
            os.makedirs(os.path.join(data, os.path.relpath(guest, "/data")), exist_ok=True)
        files = os.path.join(data, os.path.relpath(command["files"], "/data"))
        for name in sorted(os.listdir(os.path.join(folder, "files"))):
            os.chmod(shutil.copy(os.path.join(folder, "files", name), files), 0o444)
        jni = []
        if "libraries" in command:
            containers = os.path.join(work, "device-art-libraries", test)
            # Both flavors' runs convert into this folder, side by side in CTest.
            with art_run.convert_lock(work):
                art_run.convert_directory(apk_convert, os.path.join(folder, "libraries"), containers)
            jni = ["--libraries", f"{command['libraries']}={containers}"]
        target = os.path.join(outputs, test)
        shutil.rmtree(target, ignore_errors=True)
        os.makedirs(target)
        line = art_run.apk_run_command(apk_run, art, bionic_system, work, art_lib64, run_folder,
                                       command["arguments"][1:], cwd=command["workingDirectory"],
                                       environment=command["environment"], jni=jni, conscrypt=True,
                                       art_lib64_first=first, measures=os.path.join(target, "measures"))
        try:
            result = subprocess.run(line, capture_output=True, text=True, errors="replace",
                                    timeout=run_tests.TIMEOUT)
            stdout, stderr, status = result.stdout, result.stderr, str(result.returncode)
            shutil.rmtree(run_folder, ignore_errors=True)
        except subprocess.TimeoutExpired:
            stdout, stderr, status = "", f"no end within {run_tests.TIMEOUT} s\n", "timeout"
        for name, text in (("stdout", stdout), ("stderr", stderr), ("status", status + "\n")):
            with open(os.path.join(target, name), "w") as handle:
                handle.write(text)

    with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as pool:
        list(pool.map(one, names))
    return names


def check(out, outputs, root, names=(), upper_half=False):
    """Compares the outputs of a run with the references; returns the failures, each a list of lines. With
    `upper_half`, the Java fixtures' heap window must be its upper 2 GiB."""
    manifest = read_json(os.path.join(out, "tests.json"))
    tests_root = os.path.join(root, "build", "sources", "art", "test")
    failures = []
    for test in list(names) or sorted(manifest):
        entry = manifest[test]
        got = os.path.join(outputs, test)
        if not os.path.isdir(got):
            failures.append([f"{test}: no output"])
            continue
        stdout = art_run_tests.read(os.path.join(got, "stdout"))
        stderr = art_run_tests.read(os.path.join(got, "stderr"))
        status = art_run_tests.read(os.path.join(got, "status")).strip()
        problems = []
        if entry["kind"] == "java":
            wanted_out, should = art_run_tests.read(os.path.join(root, entry["expected"])), "0"
            if entry["offsets"] and art_run.offsets_failure(stderr):
                problems.append(art_run.offsets_failure(stderr))
            if entry.get("stress") and art_run.stress_failure(stderr):
                problems.append(art_run.stress_failure(stderr))
            if art_run.window_failure(stderr, upper_half):
                problems.append(art_run.window_failure(stderr, upper_half))
        else:
            if entry["reference"] == "android":
                own = os.path.join(root, "fixtures", "expected", "art-run-tests", test)
                wanted_out, wanted_err = art_run_tests.read(own + ".out"), art_run_tests.read(own + ".err")
            else:
                wanted_out = art_run_tests.read(os.path.join(tests_root, test, "expected-stdout.txt"))
                wanted_err = art_run_tests.read(os.path.join(tests_root, test, "expected-stderr.txt"))
            should = str(run_tests.expected_status(tests_root, test))
            logged = art_run_tests.LOGGER_LINE.sub("", stderr)
            if logged != wanted_err:
                problems += art_run_tests.differences("stderr", wanted_err, logged)
        if status != should:
            problems.insert(0, f"exit status {status or 'missing'}, expected {should}")
        if stdout != wanted_out:
            problems += art_run_tests.differences("stdout", wanted_out, stdout)
        if problems:
            failures.append([f"{test}:"] + problems + ["the guest's log, last lines:"]
                            + ["  " + line for line in stderr.splitlines()[-8:]])
    return failures


def selftest(art, fixtures_bin, root, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, debug):
    public_libraries = os.path.join(bionic_system, "etc", "public.libraries.txt")
    if not os.path.exists(public_libraries):
        print(f"skipped: {public_libraries} is not built (tools/build-bionic.sh)")
        return
    flavor = "-debug" if debug else ""
    out = os.path.join(work, "device-art-stage" + flavor)
    why = stage(art, fixtures_bin, root, out, sample=True)
    if why:
        print(f"skipped: {why}")
        return
    outputs = os.path.join(work, "device-art-outputs" + flavor)
    names = run(out, art, bionic_lib, bionic_system, icu_data, apk_convert, apk_run, work, outputs, debug, jobs=4)
    if not names:
        print("FAIL: nothing was staged")
        sys.exit(1)
    failures = check(out, outputs, root)
    how = f"{len(names)} staged tests" + (" in the debug flavor" if debug else "")
    if failures:
        for failure in failures:
            print("FAIL: " + failure[0])
            for line in failure[1:]:
                print("    " + line)
        print(f"{len(names) - len(failures)} of {how} print what Android 15 prints when run as the app runs them")
        sys.exit(1)
    print(f"{how} print what Android 15 prints when run as the app runs them")


def main():
    arguments = sys.argv[1:]
    debug = "--debug" in arguments
    sample = "--sample" in arguments
    arguments = [argument for argument in arguments if argument not in ("--debug", "--sample")]
    if not arguments:
        raise SystemExit(__doc__)
    action, rest = arguments[0], arguments[1:]
    if action == "stage" and len(rest) == 4:
        why = stage(*rest, sample=sample)
        if why:
            raise SystemExit(why)
    elif action == "run" and len(rest) >= 9:
        run(*rest[:9], debug=debug, names=rest[9:])
    elif action == "check" and len(rest) >= 3:
        failures = check(rest[0], rest[1], rest[2], [name for name in rest[3:] if name != "--upper-half"],
                         "--upper-half" in rest[3:])
        for failure in failures:
            print("DIFFERS  " + failure[0])
            for line in failure[1:]:
                print("    " + line)
        sys.exit(1 if failures else 0)
    elif action == "selftest" and len(rest) == 9:
        selftest(*rest, debug)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
