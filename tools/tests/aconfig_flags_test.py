#!/usr/bin/env python3
"""tools/aconfig-flags.py (ADR-0039 decision 11, the refinement): from aconfig's flag declarations and a
release's values it writes what aapt2's --feature-flags reads, one '<package>.<name>=<true|false>' per line, as
Soong's aconfig_text rule writes it (`aconfig dump-cache --format='{fully_qualified_name}={state:bool}'`). A flag
without a value is DISABLED, aconfig's default. With `java` it writes aconfig's Java classes per package, Flags,
FeatureFlags, FeatureFlagsImpl and CustomFeatureFlags, with the release's values as constants; javac must compile
them. With `cc` (ADR-0043 a) it writes one declared package's header of C and C++ accessors, constants of the
release's values, from declarations in two folders; it must compile as C11 and as C++20 with -Werror, only declared
flags get an accessor, and a package that only values name is refused. First on files in a scratch directory, then
on the tag's sources."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "aconfig-flags.py")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(text)


def run(declarations, values):
    """The tool's exit status, its output file's lines and its error text."""
    with tempfile.TemporaryDirectory() as scratch:
        out = os.path.join(scratch, "flags.txt")
        result = subprocess.run([sys.executable, TOOL, "feature-flags", "--declarations", declarations,
                                 "--values", values, "--out", out], capture_output=True, text=True)
        lines = open(out).read().splitlines() if os.path.exists(out) else None
        return result.returncode, lines, result.stderr


DECLARATIONS = """package: "android.example"
container: "system"

# A comment, as declarations have them.
flag {
    name: "shown"
    namespace: "example"
    description: "A flag with { braces } and a \\"quote\\" in its text, "
        "and a second string, which text format joins to the first."
    bug: "1"
}

flag {
    name: "hidden"
    namespace: "example"
    description: "Without a value, so DISABLED."
    bug: "2"
    is_fixed_read_only: true
}
"""

VALUE = """flag_value {
  package: "android.example"
  name: "shown"
  state: ENABLED
  permission: READ_ONLY
}
"""

# A flag that only a value names, as camera_hsum_permission, which frameworks/av declares.
VALUE_ONLY = """flag_value {
  package: "com.android.internal.camera.flags"
  name: "camera_hsum_permission"
  state: ENABLED
  permission: READ_ONLY
}
"""


# A second file of the same package, and a name with a digit: aconfig's method and constant names.
MORE = """package: "android.example"
container: "system"

flag {
    name: "enable_v2_api"
    namespace: "example"
    description: "A second file of the same package."
    bug: "3"
}
"""


def run_java(declarations, values):
    """The tool's exit status, {relative path: text} of its Java output, and its error text."""
    scratch = tempfile.mkdtemp()
    out = os.path.join(scratch, "java")
    result = subprocess.run([sys.executable, TOOL, "java", "--declarations", declarations, "--values", values,
                             "--out", out], capture_output=True, text=True)
    files = {}
    for directory, _, names in os.walk(out):
        for name in names:
            path = os.path.join(directory, name)
            files[os.path.relpath(path, out)] = open(path).read()
    return result.returncode, files, result.stderr, out


def compiles(out):
    """javac 17 of the step 1 JDK on every file below out, when the JDK is there."""
    jdks = [os.path.join(ROOT, "build/tools/jdk-17", d, "Contents/Home/bin/javac")
            for d in sorted(os.listdir(os.path.join(ROOT, "build/tools/jdk-17")))] \
        if os.path.isdir(os.path.join(ROOT, "build/tools/jdk-17")) else []
    if not jdks:
        print("note  no JDK: the Java output is not compiled")
        return True
    sources = [os.path.join(d, n) for d, _, ns in os.walk(out) for n in ns if n.endswith(".java")]
    with tempfile.TemporaryDirectory() as classes:
        result = subprocess.run([jdks[0], "-Xlint:all", "-Werror", "-d", classes] + sources,
                                capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr)
    return result.returncode == 0


def check_java(declarations, values):
    status, files, error, out = run_java(declarations, values)
    check(status == 0, f"java runs: {error}")
    folder = "android/example/"
    check(sorted(files) == [folder + n for n in ("CustomFeatureFlags.java", "FeatureFlags.java",
                                                   "FeatureFlagsImpl.java", "Flags.java")],
          f"the four classes of android.example, none for a package only a value names: {sorted(files)}")
    flags = files.get(folder + "Flags.java", "")
    for line in ('public static final String FLAG_SHOWN = "android.example.shown";',
                 'public static final String FLAG_HIDDEN = "android.example.hidden";',
                 'public static final String FLAG_ENABLE_V2_API = "android.example.enable_v2_api";',
                 "public static boolean shown() {", "public static boolean enableV2Api() {",
                 "return FEATURE_FLAGS.enableV2Api();"):
        check(line in flags, f"Flags.java has {line}")
    check("boolean enableV2Api();" in files.get(folder + "FeatureFlags.java", ""), "FeatureFlags declares it")
    impl = files.get(folder + "FeatureFlagsImpl.java", "")
    for method, value in (("shown", "true"), ("hidden", "false"), ("enableV2Api", "false")):
        check(f"public boolean {method}() {{\n        return {value};\n    }}" in impl,
              f"FeatureFlagsImpl.{method} returns {value}")
    custom = files.get(folder + "CustomFeatureFlags.java", "")
    check("return getValue(Flags.FLAG_SHOWN, FeatureFlags::shown);" in custom, "CustomFeatureFlags asks its getter")
    check(compiles(out), "javac compiles the output with every lint")


# A value for a flag its package does not declare: the cc mode writes no accessor for it.
VALUE_UNDECLARED = """flag_value {
  package: "android.example"
  name: "undeclared"
  state: ENABLED
  permission: READ_ONLY
}
"""

# A C and a C++ caller of the cc mode's header, as minikin (C names) and hwui (C++ names) call theirs.
C_CALLER = """#include <android_example.h>
int main(void) {
    return android_example_shown() && !android_example_hidden() && !android_example_enable_v2_api() ? 0 : 1;
}
"""
CPP_CALLER = """#include <android_example.h>
static_assert(android::example::shown(), "a constant expression");
static_assert(!android::example::hidden() && !android::example::enable_v2_api(), "DISABLED");
int main() { return android_example_shown() ? 0 : 1; }
"""


def run_cc(declaration_folders, values, package):
    """The cc mode's exit status, {relative path: text} of its output, and its error text."""
    out = os.path.join(tempfile.mkdtemp(), "include")
    command = [sys.executable, TOOL, "cc"]
    for folder in declaration_folders:
        command += ["--declarations", folder]
    result = subprocess.run(command + ["--values", values, "--package", package, "--out", out],
                            capture_output=True, text=True)
    files = {}
    for directory, _, names in os.walk(out):
        for name in names:
            path = os.path.join(directory, name)
            files[os.path.relpath(path, out)] = open(path).read()
    return result.returncode, files, result.stderr, out


def compiles_c(out):
    """The header compiled as C11 and as C++20, each with every warning an error, and both programs run."""
    ok = True
    with tempfile.TemporaryDirectory() as scratch:
        for compiler, standard, name, text in (("cc", "-std=c11", "caller.c", C_CALLER),
                                               ("c++", "-std=c++20", "caller.cpp", CPP_CALLER)):
            source, program = os.path.join(scratch, name), os.path.join(scratch, name + ".out")
            write(source, text)
            result = subprocess.run([compiler, standard, "-Wall", "-Wextra", "-Werror", "-I", out, source, "-o", program],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                print(result.stderr)
                ok = False
                continue
            ok = ok and subprocess.run([program]).returncode == 0
    return ok


def check_cc(declarations, more, values):
    # The two files of android.example in two folders, as hwui's and the text flags lie in two projects.
    status, files, error, out = run_cc([declarations, more], values, "android.example")
    check(status == 0, f"cc runs: {error}")
    check(sorted(files) == ["android_example.h"], f"one header named after the package: {sorted(files)}")
    header = files.get("android_example.h", "")
    check("namespace android::example {" in header, "the C++ names live in the package's namespace")
    for flag, value in (("shown", "true"), ("hidden", "false"), ("enable_v2_api", "false")):
        check(f"constexpr inline bool {flag}() {{\n    return {value};\n}}" in header,
              f"C++ {flag}() returns {value}, its snake_case name unchanged")
        check(f"static inline bool android_example_{flag}(void) {{\n    return {value};\n}}" in header,
              f"C android_example_{flag}() returns {value}")
    check("undeclared" not in header, "a flag only a value names gets no accessor")
    check("provider_" not in header and "ANDROID_EXAMPLE_" not in header,
          "neither aconfig's provider_ nor its macros, which no caller of ours uses")
    check(compiles_c(out), "the header compiles as C11 and C++20 with -Werror, and the values hold")

    status, files, error, _ = run_cc([declarations, more], values, "com.android.internal.camera.flags")
    check(status == 1 and "no declarations of the package com.android.internal.camera.flags" in error
          and "Traceback" not in error and not files,
          f"a package that only values name is refused with a message, and nothing is written: {status} {error}")


def main():
    with tempfile.TemporaryDirectory() as scratch:
        declarations = os.path.join(scratch, "declarations")
        values = os.path.join(scratch, "values")
        write(os.path.join(declarations, "core/java/android/example/flags.aconfig"), DECLARATIONS)
        write(os.path.join(values, "android.example/shown_flag_values.textproto"), VALUE)
        write(os.path.join(values, "com.android.internal.camera.flags/camera_hsum_permission_flag_values.textproto"),
              VALUE_ONLY)
        write(os.path.join(declarations, "core/java/android/example/more.aconfig"), MORE)
        status, lines, error = run(declarations, values)
        check(status == 0, f"the tool runs: {error}")
        check(lines == ["android.example.enable_v2_api=false", "android.example.hidden=false",
                        "android.example.shown=true",
                        "com.android.internal.camera.flags.camera_hsum_permission=true"],
              f"every flag once, sorted, with its value or false: {lines}")

        check_java(declarations, values)

        split_declarations, split_more = os.path.join(scratch, "split", "a"), os.path.join(scratch, "split", "b")
        write(os.path.join(split_declarations, "flags.aconfig"), DECLARATIONS)
        write(os.path.join(split_more, "more.aconfig"), MORE)
        write(os.path.join(values, "android.example/undeclared_flag_values.textproto"), VALUE_UNDECLARED)
        check_cc(split_declarations, split_more, values)
        os.remove(os.path.join(values, "android.example/undeclared_flag_values.textproto"))

        write(os.path.join(values, "android.example/broken_flag_values.textproto"),
              'flag_value {\n  package: "android.example"\n  state: ENABLED\n}\n')
        status, lines, error = run(declarations, values)
        check(status != 0 and "broken_flag_values.textproto" in error and lines is None,
              f"a value without a name is refused with its file, and nothing is written: {status} {error}")

    # The tag's sources: the 7 flags of framework-res's manifest (core/res/AndroidManifest.xml), with ap3a's values.
    sources = os.path.join(ROOT, "build", "sources")
    status, lines, error = run(os.path.join(sources, "frameworks_base"),
                               os.path.join(sources, "build_release", "aconfig", "ap3a"))
    check(status == 0, f"the tool runs on the tag's sources: {error}")
    expected = {
        "android.provider.user_keys": "true",
        "com.android.internal.camera.flags.camera_hsum_permission": "true",
        "android.net.platform.flags.register_nsd_offload_engine": "false",
        "android.content.pm.quarantined_enabled": "false",
        "com.android.window.flags.screen_recording_callbacks": "true",
        "android.permission.flags.sensitive_notification_app_protection": "true",
        "android.app.contextualsearch.flags.enable_service": "true",
    }
    found = dict(line.split("=", 1) for line in lines or [])
    for name, value in expected.items():
        check(found.get(name) == value, f"{name}={value} in the tag's flags: {found.get(name)}")
    check(len(found) > 1000, f"the tag's flags are all there: {len(found)}")

    print("aconfig-flags: ok" if failures == 0 else f"aconfig-flags: {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
