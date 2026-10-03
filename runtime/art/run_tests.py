#!/usr/bin/env python3
"""ART's run-tests on our ART (ADR-0033 test 3): which tests of AOSP's art/test make the subset, their jars, built as
ART's run_test_build.py builds them for a device, and dalvikvm's arguments, as ART's default_run.py gives them on a
device with the switch interpreter and no boot image. runtime/tests/art_run_tests.py runs the subset on the Mac,
tools/android-reference.sh --art-run-tests on Android 15.

A test of art/test is in the subset unless it breaks one of these rules, taken in this order:
  1. it has expected-stdout.txt and Java sources (src or src-art);
  2. no smali or jasmin sources and no prebuilt classes;
  3. no native code and no System.load or loadLibrary: those need ART's JNI test library;
  4. no hidden API flags, no generated sources, no imports of ART's JVMTI helpers, ASM or junit;
  5. no Runtime.exec or ProcessBuilder: the guest has no fork;
  6. a run.py of its own only runs it once through default_run, with runtime options the interpreter honors
     (RUNTIME_OPTIONS), an expected exit status, log tags or a sync;
  7. a build.py of its own only calls default_build, with what build() here does (BUILD_OPTIONS);
  8. knownfailures.json lists it neither everywhere nor for a configuration of ours (OUR_CONFIGURATIONS).

The list, fixtures/art-run-tests.txt, gives each test of the subset its reference:
  - "aosp": on Android 15 it ended with the expected exit status and printed its expected-stdout.txt and
    expected-stderr.txt;
  - "android": it ended with the expected exit status and printed something else, which
    fixtures/expected/art-run-tests/<test>.out and .err hold;
  - "out <reason>": it failed or ran out of time on Android 15, or the guest lacks what it needs.
"sample" after "aosp" or "android" marks the tests of CTest's sample.

Usage: run_tests.py subset <sources>
       run_tests.py jars <jdk-home> <r8.jar> <java-work-directory> <sources> <out> [--jobs <n>] [<test>...]
       run_tests.py device-script <sources> <device-directory>
       run_tests.py reference <sources> <results> <list> <expected>
       run_tests.py sample <list> <times> <every> <seconds> [<test>...]
<sources> holds AOSP's projects (build/sources). <java-work-directory> is where boot_jars.py left the classes of the
boot jars (<jar>.classes.jar, and all of them in all-classes.jar). <results> holds <test>.out, .err and .status from
the device script's run. <times> has a line "<test> <seconds>" for each test of a run on the Mac.
"""
import concurrent.futures
import fcntl
import functools
import hashlib
import json
import os
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import types
import zipfile

BOOT_JARS = ["core-oj", "core-libart", "core-icu4j"]
# Android 15's boot jars in their APEXes, as dalvikvm64 gets them on the device.
DEVICE_BOOT_CLASS_PATH = ("/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:"
                          "/apex/com.android.i18n/javalib/core-icu4j.jar")
# Where a test's jars lie, and its working directory, on the device and in the guest: ART's DEX_LOCATION.
DEX_LOCATIONS = "/data/local/tmp/run-test"
# How long a test may run, in seconds, on either side.
TIMEOUT = 300
# The folders of Java sources that run_test_build.py compiles.
JAVA_FOLDERS = ["src", "src2", "src-art", "src-ex", "src-ex2", "src-multidex", "src-aotex", "src-bcpex"]
# What a run.py may pass to default_run (rule 6), and the runtime options among them that the interpreter honors.
RUN_OPTIONS = {"runtime_option", "expected_exit_code", "android_log_tags", "sync"}
RUNTIME_OPTIONS = re.compile(r"-Xmx\d+m|-XX:FinalizerTimeoutMs=\d+|-XX:HeapMaxFree=\w+|-XX:LargeObjectSpace=disabled|"
                             r"-Xlockprofthreshold:\d+|-Xtarget-sdk-version:\d+|-XX:LowMemoryMode")
# What a build.py may pass to default_build (rule 7): what build() does.
BUILD_OPTIONS = {"use_desugar", "api_level", "javac_args", "javac_source_arg", "javac_target_arg", "d8_flags",
                 "zip_compression_method", "zip_align_bytes"}
# knownfailures.json's configurations that ours is: on a device, interpreted, without a boot image or dex2oat, in
# ART's release (ndebug) and debug flavors; and the build's variables as ours sets them (ADR-0033 decision 5).
OUR_CONFIGURATIONS = {"target", "interpreter", "no-image", "no-prebuild", "debug", "ndebug"}
OUR_BUILD = {"ART_USE_READ_BARRIER": "false", "ART_DEFAULT_GC_TYPE": "SS"}
# D8's API levels by the names build.py uses (run_test_build.py).
API_LEVELS = {"default-methods": 24, "parameter-annotations": 25, "agents": 26, "method-handles": 26,
              "var-handles": 28, "const-method-type": 28}


class Unsupported(Exception):
    """What a run.py or build.py does beyond what the subset allows."""


class Context:
    """The context run.py and build.py get from ART's harness, as far as tests of the subset use it: it records
    their calls of default_run and default_build, and anything else stops them."""

    def __init__(self):
        self.jvm = False
        self.host = False
        self.target = True
        self.env = types.SimpleNamespace(ANDROID_LOG_TAGS="")
        self.calls = []

    def default_run(self, args, **options):
        self.calls.append(("default_run", options))

    def default_build(self, **options):
        self.calls.append(("default_build", options))

    def __getattr__(self, name):
        raise Unsupported(f"ctx.{name}")


def script_calls(path, function, *arguments):
    """The calls of default_run or default_build that `function` of the script `path` makes."""
    context = Context()
    try:
        runpy.run_path(path)[function](context, *arguments)
    except Unsupported:
        raise
    except Exception as error:  # a script of art/test that needs more of the harness than Context has
        raise Unsupported(f"{type(error).__name__}: {error}")
    return context.calls


def java_sources(folder):
    """The .java files under `folder`, sorted, as run_test_build.py passes them to javac."""
    return sorted(os.path.join(root, name) for root, _, names in os.walk(folder) for name in names
                  if name.endswith(".java"))


@functools.lru_cache(maxsize=None)
def run_options(tests, test):
    """What the test's run.py passes to default_run, or {} without a run.py."""
    path = os.path.join(tests, test, "run.py")
    if not os.path.exists(path):
        return {}
    calls = script_calls(path, "run", types.SimpleNamespace(jvm=False, host=False, runtime_option=[]))
    if [name for name, _ in calls] != ["default_run"]:
        raise Unsupported(f"{len(calls)} runs")
    return calls[0][1]


@functools.lru_cache(maxsize=None)
def build_options(tests, test):
    """What the test's build.py passes to default_build, or {} without a build.py."""
    path = os.path.join(tests, test, "build.py")
    if not os.path.exists(path):
        return {}
    calls = script_calls(path, "build")
    if [name for name, _ in calls] != ["default_build"]:
        raise Unsupported(f"{len(calls)} builds")
    return calls[0][1]


def configuration_matches(variant):
    """Whether a variant expression of knownfailures.json ("a | b & !c") holds for a configuration of ours."""
    expression = re.sub(r"[A-Za-z0-9_-]+", lambda name: str(name.group(0) in OUR_CONFIGURATIONS), variant)
    expression = expression.replace("&", " and ").replace("|", " or ").replace("!", " not ")
    if not re.fullmatch(r"(True|False|and|or|not|[ ()])*", expression):
        raise SystemExit(f"knownfailures.json: cannot read the variant {variant!r}")
    return eval(expression, {"__builtins__": {}})


def known_failures(tests):
    """The tests knownfailures.json lists for a configuration of ours, each with the entry's description."""
    with open(os.path.join(tests, "knownfailures.json")) as file:
        entries = json.load(file)
    names = [name for name in os.listdir(tests) if re.match(r"\d+-", name)]
    failures = {}
    for entry in entries:
        build = entry.get("env_vars") or {}
        if any(OUR_BUILD.get(name) != value for name, value in build.items()):
            continue
        if entry.get("variant") and not configuration_matches(entry["variant"]):
            continue
        listed = entry.get("tests") or []
        listed = [listed] if isinstance(listed, str) else list(listed)
        for pattern in entry.get("test_patterns") or []:
            listed += [name for name in names if re.fullmatch(pattern, name)]
        description = entry.get("description", "")
        description = " ".join(description) if isinstance(description, list) else description
        for name in listed:
            failures.setdefault(name, f"knownfailures.json ({entry.get('variant') or 'everywhere'}): "
                                      f"{' '.join(description.split())}")
    return failures


def reason_left_out(tests, test, known):
    """Why `test` is not in the subset, by the rules above, or None when it is."""
    folder = os.path.join(tests, test)
    entries = set(os.listdir(folder))
    if "expected-stdout.txt" not in entries:
        return "rule 1: no expected-stdout.txt"
    if not entries & {"src", "src-art"}:
        return "rule 1: no Java sources"
    if any(entry.startswith(("smali", "jasmin")) for entry in entries):
        return "rule 2: smali or jasmin"
    if any(entry.startswith("classes") for entry in entries):
        return "rule 2: prebuilt classes"
    if any(entry.endswith((".cc", ".cpp", ".c", ".h")) for entry in entries):
        return "rule 3: native code"
    text = ""
    for java in (name for name in JAVA_FOLDERS if name in entries):
        for source in java_sources(os.path.join(folder, java)):
            with open(source, errors="replace") as file:
                text += file.read()
    if re.search(r"System\.(loadLibrary|load)\(", text):
        return "rule 3: loads a native library"
    if "hiddenapi-flags.csv" in entries or "hiddenapi" in test:
        return "rule 4: hidden API flags"
    if entries & {"generate-sources", "generate-java", "util-src"}:
        return "rule 4: generated sources"
    if re.search(r"\bimport (static )?(art\.|org\.objectweb|junit|org\.junit)", text):
        return "rule 4: imports ART's JVMTI helpers, ASM or junit"
    if re.search(r"Runtime\.getRuntime\(\)\.exec|ProcessBuilder", text):
        return "rule 5: starts a process"
    try:
        options = run_options(tests, test)
    except Unsupported as error:
        return f"rule 6: run.py: {error}"
    for name in sorted(set(options) - RUN_OPTIONS):
        return f"rule 6: run.py: {name}"
    for option in options.get("runtime_option", []):
        if not RUNTIME_OPTIONS.fullmatch(option):
            return f"rule 6: run.py: {option}"
    try:
        built = build_options(tests, test)
    except Unsupported as error:
        return f"rule 7: build.py: {error}"
    for name in sorted(set(built) - BUILD_OPTIONS):
        return f"rule 7: build.py: {name}"
    return known.get(test)


def subset(tests):
    """The tests of the subset, sorted, and why each of the others is not in it."""
    known = known_failures(tests)
    included, reasons = [], {}
    for test in sorted(name for name in os.listdir(tests)
                       if re.match(r"\d+-", name) and os.path.isdir(os.path.join(tests, name))):
        reason = reason_left_out(tests, test, known)
        if reason is None:
            included.append(test)
        else:
            reasons[test] = reason
    return included, reasons


def dex_location(test):
    return f"{DEX_LOCATIONS}/{test}"


def jar_names(tests, test):
    """The jars run_test_build.py makes for `test`, which lie in its DEX_LOCATION."""
    entries = os.listdir(os.path.join(tests, test))
    return [f"{test}.jar"] + [f"{test}-{kind}.jar" for kind in ("ex", "aotex") if f"src-{kind}" in entries]


def dalvikvm_arguments(tests, test, boot_class_path, debug=False, extra=()):
    """dalvikvm's arguments for `test`, in the order ART's default_run.py gives them on a device with
    --switch-interpreter, --no-image and --no-prebuild: run.py's runtime options and `extra` first. Only the debug
    flavor's main argument differs, the name of ART's JNI test library, which no test of the subset loads."""
    location = dex_location(test)
    classpath = [f"{location}/{test}.jar"] + ([f"{location}/{test}-aotex.jar"]
                                              if f"{test}-aotex.jar" in jar_names(tests, test) else [])
    return list(extra) + run_options(tests, test).get("runtime_option", []) + [
        "-XX:SlowDebug=true", "-XjdwpProvider:none", f"-Xbootclasspath:{boot_class_path}",
        f"-Xbootclasspath-locations:{boot_class_path}", "-Xnoimage-dex2oat", "-Xnorelocate", "-Xjnigreflimit:512",
        "-Xcheck:jni", "-Xint", "-Xusejit:false", "-Ximage:/system/non-existent/boot.art",
        "-Djava.io.tmpdir=/data/local/tmp", "-XX:DumpNativeStackOnSigQuit:false",
        "-cp", ":".join(classpath), "Main", "arttestd" if debug else "arttest"]


def expected_status(tests, test):
    return int(run_options(tests, test).get("expected_exit_code", 0))


class BuildError(Exception):
    pass


def tool(command, work):
    result = subprocess.run(command, cwd=work, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        raise BuildError(f"{os.path.basename(command[0])} {command[1]} ... failed:\n{result.stderr[-2000:]}"
                         f"{result.stdout[-2000:]}")


def zip_files(work, target, names, compression, align):
    """A jar of the files `names` in `work`, stored or deflated as soong_zip makes it; stored entries start at a
    multiple of `align` bytes when it is given, as zipalign leaves them."""
    with zipfile.ZipFile(os.path.join(work, target), "w") as archive:
        for name in names:
            info = zipfile.ZipInfo(name, date_time=(2008, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if compression == "store" else zipfile.ZIP_DEFLATED
            if align and info.compress_type == zipfile.ZIP_STORED:
                info.extra = b"\0" * (-(archive.fp.tell() + 30 + len(name.encode())) % int(align))
            with open(os.path.join(work, name), "rb") as file:
                archive.writestr(info, file.read())


def default_build(work, test, jdk, r8, java, use_desugar=True, api_level=26, javac_args=(), javac_source_arg="1.8",
                  javac_target_arg="1.8", d8_flags=(), zip_compression_method="deflate", zip_align_bytes=None):
    """What run_test_build.py's default_build does in a copy of the test's folder, `work`, for a device, as far as
    tests of the subset need it: no jasmin, smali or hidden API flags. Paths are relative to `work`, as there."""
    boot = ":".join(os.path.join(java, f"{jar}.classes.jar") for jar in BOOT_JARS)
    level = API_LEVELS[api_level] if isinstance(api_level, str) else int(api_level)
    classpath = []

    def exists(name):
        return os.path.exists(os.path.join(work, name))

    def make_java(destination, *folders):
        folders = [folder for folder in folders if exists(folder)]
        if not folders:
            return False
        os.makedirs(os.path.join(work, destination), exist_ok=True)
        command = [os.path.join(jdk, "bin", "javac"), "-g", "-Xlint:-options"] + list(javac_args) + [
            "-implicit:none", "-encoding", "utf8", "-d", destination, "-source", javac_source_arg, "-target",
            javac_target_arg]
        if float(javac_target_arg) < 17:
            command += ["-bootclasspath", boot]
        if classpath:
            command += ["-classpath", ":".join(classpath)]
        for folder in folders:
            command += [os.path.relpath(source, work) for source in java_sources(os.path.join(work, folder))]
        tool(command, work)
        if exists("javac_post.sh"):
            tool(["/bin/bash", "javac_post.sh", destination], work)
        return True

    def make_dex(folder):
        # A flag "-J<option>" is the JVM's -<option>, as AOSP's d8 script passes it.
        jvm = ["-Dcom.android.tools.r8.dexContainerExperiment"] + ["-" + flag[2:] for flag in d8_flags
                                                                   if flag.startswith("-J")]
        command = [os.path.join(jdk, "bin", "java")] + jvm + ["-cp", r8, "com.android.tools.r8.D8"]
        command += [flag for flag in d8_flags if not flag.startswith("-J")]
        command += ["--min-api", str(level), "--output", folder + ".jar"]
        command += ["--lib", os.path.join(java, "all-classes.jar")] if use_desugar else ["--no-desugaring"]
        command += sorted(os.path.relpath(os.path.join(root, name), work)
                          for root, _, names in os.walk(os.path.join(work, folder))
                          for name in names if name.endswith(".class"))
        tool(command, work)
        with zipfile.ZipFile(os.path.join(work, folder + ".jar")) as archive:
            dex = archive.read("classes.dex")
        with open(os.path.join(work, folder + ".dex"), "wb") as file:
            file.write(dex)

    def make_zip(target, *names):
        zip_files(work, target, names, zip_compression_method, zip_align_bytes)

    if exists("src") and any(exists(name) for name in ("src-multidex", "src-aotex", "src-bcpex", "src-ex", "src2",
                                                        "src-ex2", "src-art")):
        make_java("classes-tmp-all", "src", "src-multidex", "src-aotex", "src-bcpex", "src-ex")
        classpath.append("classes-tmp-all")
    for kind in ("aotex", "bcpex"):
        if make_java(f"classes-{kind}", f"src-{kind}"):
            make_dex(f"classes-{kind}")
            os.replace(os.path.join(work, f"classes-{kind}.dex"), os.path.join(work, "classes.dex"))
            make_zip(f"{test}-{kind}.jar", "classes.dex")
    make_java("classes", "src")
    make_java("classes", "src-art")
    if make_java("classes2", "src-multidex"):
        make_dex("classes2")
    make_java("classes", "src2")
    if exists("classes") and os.listdir(os.path.join(work, "classes")):
        make_dex("classes")
    make_java("classes-ex", "src-ex")
    make_java("classes-ex", "src-ex2")
    if exists("classes-ex"):
        make_dex("classes-ex")
        # Stored as classes.dex in <test>-ex.jar.
        os.replace(os.path.join(work, "classes.dex"), os.path.join(work, "classes-1.dex"))
        os.replace(os.path.join(work, "classes-ex.dex"), os.path.join(work, "classes.dex"))
        make_zip(f"{test}-ex.jar", "classes.dex")
        os.replace(os.path.join(work, "classes.dex"), os.path.join(work, "classes-ex.dex"))
        os.replace(os.path.join(work, "classes-1.dex"), os.path.join(work, "classes.dex"))
    make_zip(f"{test}.jar", "classes.dex", *(["classes2.dex"] if exists("classes2.dex") else []))


def inputs_digest(tests, test, jdk, r8, java):
    """What a test's jars come from: its folder, this builder, the JDK, D8 and the boot jars' classes."""
    digest = hashlib.sha256()
    with open(os.path.abspath(__file__), "rb") as file:
        digest.update(file.read())
    folder = os.path.join(tests, test)
    for root, folders, names in os.walk(folder):
        folders.sort()
        for name in sorted(names):
            path = os.path.join(root, name)
            digest.update(os.path.relpath(path, folder).encode() + b"\0")
            with open(path, "rb") as file:
                digest.update(file.read())
    for path in [os.path.join(jdk, "release"), r8, os.path.join(java, "all-classes.jar")] + [
            os.path.join(java, f"{jar}.classes.jar") for jar in BOOT_JARS]:
        status = os.stat(path)
        digest.update(f"{path} {status.st_size} {status.st_mtime_ns}\n".encode())
    return digest.hexdigest()


def build(tests, test, jdk, r8, java, out):
    """The folder <out>/<test> with the test's jars, built again only when what they come from changed."""
    target = os.path.join(out, test)
    stamp = os.path.join(target, "inputs.sha256")
    digest = inputs_digest(tests, test, jdk, r8, java)
    if os.path.exists(stamp):
        with open(stamp) as file:
            if file.read() == digest:
                return target
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as scratch:
        work = os.path.join(scratch, test)
        shutil.copytree(os.path.join(tests, test), work)
        default_build(work, test, jdk, r8, java, **build_options(tests, test))
        built = os.path.join(scratch, "built")
        os.makedirs(built)
        for name in jar_names(tests, test):
            shutil.move(os.path.join(work, name), built)
        with open(os.path.join(built, "inputs.sha256"), "w") as file:
            file.write(digest)
        shutil.rmtree(target, ignore_errors=True)
        os.replace(built, target)
    return target


def build_all(tests, names, jdk, r8, java, out, jobs):
    """Builds the tests `names`, `jobs` at a time, while no other process builds into `out`; returns the build
    errors by test."""
    errors = {}
    # The tools run in each test's folder.
    tests, jdk, r8, java, out = (os.path.abspath(path) for path in (tests, jdk, r8, java, out))
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "build.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as pool:
            futures = {pool.submit(build, tests, name, jdk, r8, java, out): name for name in names}
            for future in concurrent.futures.as_completed(futures):
                try:
                    future.result()
                except BuildError as error:
                    errors[futures[future]] = str(error)
    return errors


def read_list(path):
    """The list's entries: test -> (reference, flags or the reason)."""
    entries = {}
    with open(path) as file:
        for line in file:
            if line.strip() and not line.startswith("#"):
                test, reference, *rest = line.split(None, 2)
                entries[test] = (reference, rest[0].strip() if rest else "")
    return entries


LIST_HEADER = """\
# ART's run-tests of ADR-0033 test 3: the subset of AOSP's art/test by the rules of runtime/art/run_tests.py, with the
# reference of each test: "aosp" (Android 15 printed the test's expected-stdout.txt and expected-stderr.txt), "android"
# (Android 15 printed what fixtures/expected/art-run-tests/<test>.out and .err hold) or "out" and why the test is left
# out. "sample" marks the tests of CTest's sample. Written by run_tests.py reference and sample from the runs of
# tools/android-reference.sh --art-run-tests; a reason of the guest's is kept from run to run.
"""


def write_list(path, entries):
    with open(path, "w") as file:
        file.write(LIST_HEADER)
        for test in sorted(entries):
            reference, rest = entries[test]
            file.write(f"{test} {reference}{' ' + rest if rest else ''}\n")


def reference(tests, results, list_path, expected):
    """Writes the list and Android 15's own outputs from the device's results; returns the tests that left the
    subset on Android 15."""
    old = read_list(list_path) if os.path.exists(list_path) else {}
    entries, left = {}, []
    os.makedirs(expected, exist_ok=True)
    for test in subset(tests)[0]:
        def result(kind):
            with open(os.path.join(results, f"{test}.{kind}"), errors="replace") as file:
                return file.read()

        def wanted(kind):
            path = os.path.join(tests, test, f"expected-{kind}.txt")
            if not os.path.exists(path):
                return ""
            with open(path, errors="replace") as file:
                return file.read()

        status, should = int(result("status").strip() or -1), expected_status(tests, test)
        reference_was, rest_was = old.get(test, ("", ""))
        flags = rest_was if reference_was in ("aosp", "android") else ""
        if reference_was == "out" and not rest_was.startswith("Android 15"):
            entries[test] = (reference_was, rest_was)  # a reason of the guest's
        elif status == 124:
            entries[test] = ("out", f"Android 15 ran out of time ({TIMEOUT} s)")
        elif status != should:
            entries[test] = ("out", f"Android 15 ended with exit status {status}, expected {should}")
        elif result("out") == wanted("stdout") and result("err") == wanted("stderr"):
            entries[test] = ("aosp", flags)
        else:
            entries[test] = ("android", flags)
        for kind in ("out", "err"):
            path = os.path.join(expected, f"{test}.{kind}")
            if entries[test][0] == "android":
                with open(path, "w") as file:
                    file.write(result(kind))
            elif os.path.exists(path):
                os.remove(path)
        if entries[test][0] == "out" and reference_was != "out":
            left.append(f"{test}: {entries[test][1]}")
    write_list(list_path, entries)
    return left


def device_script(tests, device):
    """A script for the device's shell that runs each test of the subset as the guest runs it, from the jars in
    <device>/jars/<test>, and leaves <test>.out, .err and .status in <device>/results."""
    lines = ["#!/system/bin/sh", "# Written by runtime/art/run_tests.py device-script: ART's run-tests (ADR-0033).",
             f"mkdir -p {device}/results"]
    for test in subset(tests)[0]:
        location = dex_location(test)
        arguments = " ".join(dalvikvm_arguments(tests, test, DEVICE_BOOT_CLASS_PATH))
        lines += [f"rm -rf {location} && mkdir -p {location}/dalvik-cache/arm64 && cp {device}/jars/{test}/*.jar "
                  f"{location}/ && chmod 444 {location}/*.jar && cd {location}",
                  f"ANDROID_DATA={location} DEX_LOCATION={location} timeout {TIMEOUT} dalvikvm64 {arguments} "
                  f">{device}/results/{test}.out 2>{device}/results/{test}.err </dev/null",
                  f"echo $? >{device}/results/{test}.status; cd / && rm -rf {location}"]
    return "\n".join(lines) + "\n"


def choose_sample(list_path, times_path, every, seconds, always=()):
    """Marks every <every>th test of the list's "aosp" and "android" ones whose run on the Mac took less than
    <seconds>, by name, and the tests `always`, as CTest's sample."""
    entries = read_list(list_path)
    with open(times_path) as file:
        times = {test: float(value) for test, value in (line.split() for line in file if line.strip())}
    quick = [test for test in sorted(entries) if entries[test][0] in ("aosp", "android")
             and times.get(test, seconds) < seconds]
    chosen = set(quick[::every]) | {test for test in always if entries.get(test, ("",))[0] in ("aosp", "android")}
    for test, (kind, _) in entries.items():
        if kind in ("aosp", "android"):
            entries[test] = (kind, "sample" if test in chosen else "")
    write_list(list_path, entries)
    return len(chosen)


def main():
    arguments = sys.argv[1:]
    jobs = 1
    if "--jobs" in arguments:
        at = arguments.index("--jobs")
        jobs = int(arguments[at + 1])
        del arguments[at:at + 2]
    command = arguments[0] if arguments else ""
    if command == "subset" and len(arguments) == 2:
        included, reasons = subset(os.path.join(arguments[1], "art", "test"))
        print("\n".join(included))
        counts = {}
        for reason in reasons.values():
            key = reason.split(":")[0] if reason.startswith("knownfailures") else ":".join(reason.split(":")[:2])
            counts[key] = counts.get(key, 0) + 1
        print(f"{len(included)} of {len(included) + len(reasons)} tests; left out:", file=sys.stderr)
        for key, count in sorted(counts.items(), key=lambda item: -item[1]):
            print(f"  {count:4d} {key}", file=sys.stderr)
    elif command == "jars" and len(arguments) >= 6:
        jdk, r8, java, sources, out, names = arguments[1:6] + [arguments[6:]]
        tests = os.path.join(sources, "art", "test")
        errors = build_all(tests, names or subset(tests)[0], jdk, r8, java, out, jobs)
        for test, error in sorted(errors.items()):
            print(f"FAIL: {test}: {error}", file=sys.stderr)
        sys.exit(1 if errors else 0)
    elif command == "device-script" and len(arguments) == 3:
        sys.stdout.write(device_script(os.path.join(arguments[1], "art", "test"), arguments[2]))
    elif command == "reference" and len(arguments) == 5:
        for line in reference(os.path.join(arguments[1], "art", "test"), *arguments[2:]):
            print(f"left the subset: {line}")
        kinds = [kind for kind, _ in read_list(arguments[3]).values()]
        print(f"On Android 15, {kinds.count('aosp')} printed AOSP's expected output, {kinds.count('android')} their "
              f"own; {kinds.count('out')} are out")
    elif command == "sample" and len(arguments) >= 5:
        count = choose_sample(arguments[1], arguments[2], int(arguments[3]), float(arguments[4]), arguments[5:])
        print(f"{count} tests in the sample")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
