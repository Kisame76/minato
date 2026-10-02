#!/usr/bin/env python3
"""tools/soong_plan.py's host side (ADR-0043 a1): the guest builds a host flavor, as Soong builds it for the host,
with the NDK's compiler, which defines __ANDROID__. make_tree's host_side folders therefore take the host side of
every preprocessor test of __ANDROID__ in their C and C++ files, as /usr/bin/unifdef -U__ANDROID__ resolves it, in the
tree's one copy of each file, after the patches; every other folder keeps its tests (bionic, libc++, Skia).

On a made-up set of sources:
- #ifdef, #ifndef, #if defined(__ANDROID__) && X and #elif take the host side, a test of another macro stays, and a
  file without a newline at its end keeps that end;
- a file without __ANDROID__, a file that is not C or C++, a file of a folder outside host_side (also one whose name
  only begins like a host_side folder) stay links to the sources;
- a patched file gets its patch and the host side;
- a raw string literal, which unifdef 2.12 does not know, goes through as it is, also one with a quote in it;
- an expression unifdef cannot decide (defined(__ANDROID__) || defined(X)) and a file unifdef cannot read (a comment
  without its end) are problems, never a silent device side;
- a second run leaves the tree's copies as they are.
Usage: host_side_test.py"""
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import soong_plan  # noqa: E402

failures = 0

TESTS = """#include <a.h>
#ifdef __ANDROID__  // device only
int device;
#else
int host;
#endif  // __ANDROID__
#ifndef __ANDROID__
int not_device;
#endif
#if defined(__ANDROID__) && defined(FEATURE)
int device_feature;
#endif
#if defined(FEATURE)
int feature;
#elif defined(__ANDROID__)
int device_without_feature;
#else
int host_without_feature;
#endif
#ifdef OTHER
int other;
#endif
int last;  // no newline at the end"""

HOST_SIDE = """#include <a.h>
int host;
int not_device;
#if defined(FEATURE)
int feature;
#else
int host_without_feature;
#endif
#ifdef OTHER
int other;
#endif
int last;  // no newline at the end"""

DEVICE_ONLY = "#ifdef __ANDROID__\nint device;\n#endif\n"

PATCHED = "int first;\n#ifdef __ANDROID__\nint device;\n#endif\nint second;\n"

PATCH = """A made-up patch.

--- a/frameworks/base/libs/patched.h
+++ b/frameworks/base/libs/patched.h
@@ -1,5 +1,5 @@
 int first;
 #ifdef __ANDROID__
 int device;
 #endif
-int second;
+int second_patched;
"""

UNDECIDED = "#if defined(__ANDROID__) || defined(OTHER)\nint either;\n#endif\n"

RAW = """#ifdef __ANDROID__
static const char kDevice[] = R"TEXT(
  "quoted
#endif
)TEXT";
#else
static const char kHost[] = R"(host "text")";
#endif
"""

RAW_HOST_SIDE = 'static const char kHost[] = R"(host "text")";\n'

UNREADABLE = "#ifdef __ANDROID__\nint device;\n#endif\n/* a comment without its end\n"


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(text)


def read(path):
    with open(path) as handle:
        return handle.read()


def main():
    if not os.path.exists("/usr/bin/unifdef"):
        print("skipped: /usr/bin/unifdef is missing")
        return 0
    scratch = tempfile.mkdtemp()
    try:
        sources = os.path.join(scratch, "sources")
        write(os.path.join(sources, "libs", "tests.h"), TESTS)
        write(os.path.join(sources, "libs", "plain.h"), "int plain;\n")
        write(os.path.join(sources, "libs", "notes.txt"), DEVICE_ONLY)
        write(os.path.join(sources, "libs", "patched.h"), PATCHED)
        write(os.path.join(sources, "libs", "undecided.h"), UNDECIDED)
        write(os.path.join(sources, "libs", "raw.cpp"), RAW)
        write(os.path.join(sources, "libs", "unreadable.c"), UNREADABLE)
        write(os.path.join(sources, "libsx", "near.h"), DEVICE_ONLY)
        write(os.path.join(sources, "bionic", "system.h"), DEVICE_ONLY)
        patches = os.path.join(scratch, "patches")
        write(os.path.join(patches, "0001-made-up.patch"), PATCH)
        overlay = os.path.join(scratch, "overlay")
        os.makedirs(overlay)
        tree = os.path.join(scratch, "tree")
        projects = {"frameworks/base/libs": os.path.join(sources, "libs"),
                    "frameworks/base/libsx": os.path.join(sources, "libsx"),
                    "bionic": os.path.join(sources, "bionic")}
        try:
            problems = soong_plan.make_tree(tree, projects, overlay, patches, host_side=["frameworks/base/libs"])
        except TypeError as error:
            check(False, f"make_tree takes host_side: {error}")
            return 1

        def path(name):
            return os.path.join(tree, name)

        tests = path("frameworks/base/libs/tests.h")
        check(not os.path.islink(tests) and read(tests) == HOST_SIDE,
              "tests.h is a copy with the host side of #ifdef, #ifndef, && and #elif, OTHER's test and its end kept")
        for name, text in (("frameworks/base/libs/plain.h", "int plain;\n"),
                           ("frameworks/base/libs/notes.txt", DEVICE_ONLY),
                           ("frameworks/base/libsx/near.h", DEVICE_ONLY), ("bionic/system.h", DEVICE_ONLY)):
            check(os.path.islink(path(name)) and read(path(name)) == text, f"{name} stays a link to its source")
        patched = path("frameworks/base/libs/patched.h")
        check(not os.path.islink(patched) and read(patched) == "int first;\nint second_patched;\n",
              "patched.h has its patch and the host side")
        raw = path("frameworks/base/libs/raw.cpp")
        check(not os.path.islink(raw) and read(raw) == RAW_HOST_SIDE,
              "raw.cpp takes the host side, with its raw string literals as they are")
        for name in ("undecided.h", "unreadable.c"):
            check(any(name in problem for problem in problems), f"{name} is a problem: {problems}")
        check(len(problems) == 2, f"no other problems: {problems}")
        before = {name: os.stat(path(name)).st_mtime_ns for name in ("frameworks/base/libs/tests.h",
                                                                     "frameworks/base/libs/patched.h")}
        soong_plan.make_tree(tree, projects, overlay, patches, host_side=["frameworks/base/libs"])
        after = {name: os.stat(path(name)).st_mtime_ns for name in before}
        check(before == after, "a second run leaves the copies as they are")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print(f"host-side: {'all checks passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
