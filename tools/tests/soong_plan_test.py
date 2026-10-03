#!/usr/bin/env python3
"""tools/soong_plan.py's Planner on a made-up set (ADR-0043 a1 and a2):
- cpp_std and c_std "experimental" compile as Soong compiles them at the tag, -std=gnu++2b and -std=gnu2x;
- every compile, a cc_object's assembly text too, gets its standard where Soong puts it (cc/compiler.go and
  cc/builder.go at the tag): after the module's cflags, first in its conlyflags and cppflags. A module without c_std
  or cpp_std gets Soong's gnu17 and gnu++20, and gnu_extensions: false makes them c17 and c++20. So a -std in the
  cflags gives way, as libpng's -std=gnu89 does on Android, and one in the cppflags wins;
- a library of the guest system that a shared library links statically, also through a static library's
  whole_static_libs, which the shared library links as an archive, is NEEDED and never compiled: the guest has it as a
  shared library only;
- one the shared library takes whole, directly or through whole_static_libs, is a problem, also when an archive has
  linked it before: Soong would embed it and export its symbols from the shared library, which NEEDED does not;
- a Rust library gives its C and C++ users its own include directories and nothing of its whole_static_libs, neither
  their include directories nor their generated headers (Soong's rust/library.go), also when it is left out.
Usage: soong_plan_test.py"""
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import soong_plan  # noqa: E402

failures = 0

BLUEPRINT = """cc_library_shared {
    name: "libfront",
    srcs: ["front.cpp", "front.c"],
    cpp_std: "experimental",
    c_std: "experimental",
    static_libs: ["libguest", "libarchive"],
    whole_static_libs: ["libpart", "libwholeguest", "librustffi"],
}

rust_ffi_static {
    name: "librustffi",
    crate_name: "madeup",
    srcs: ["lib.rs"],
    export_include_dirs: ["rust_include"],
    whole_static_libs: ["libbridge"],
}

cc_library_static {
    name: "libbridge",
    srcs: ["bridge.cpp"],
    export_include_dirs: ["bridge_include"],
    generated_headers: ["bridge_header"],
    export_generated_headers: ["bridge_header"],
}

genrule {
    name: "bridge_header",
    cmd: "touch $(out)",
    out: ["bridge.h"],
}

cc_library_static {
    name: "libarchive",
    srcs: ["archive.cpp"],
    whole_static_libs: ["libguestinarchive"],
}

cc_library_static {
    name: "libpart",
    srcs: ["part.cpp"],
    static_libs: ["libwholeguest"],
    whole_static_libs: ["libguestinpart"],
}

cc_library_shared {
    name: "libstd",
    srcs: ["std.c", "std.cpp"],
    cflags: ["-std=gnu89"],
    cppflags: ["-std=c++17"],
    static_libs: ["libnognu"],
    generated_headers: ["defines_header"],
}

cc_library_static {
    name: "libnognu",
    srcs: ["nognu.c", "nognu.cpp"],
    c_std: "gnu11",
    conlyflags: ["-DNOGNU"],
    gnu_extensions: false,
}

genrule {
    name: "defines_header",
    srcs: [":defines_object"],
    cmd: "touch $(out)",
    out: ["defines.h"],
}

cc_object {
    name: "defines_object",
    srcs: ["defines.cpp"],
    cflags: ["-std=c++14"],
}
"""


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(text)


def call(lines, start):
    """The text of the plan's call that begins with `start`, or ""."""
    return next((line for line in lines if line.startswith(start + "\n")), "")


def field(text, key):
    """The values of a field of a call's text, in order and unquoted."""
    match = re.search(r"^  " + key + r" (.*)$", text, re.M)
    return [value.strip('"') for value in match.group(1).split()] if match else []


def standard(flags):
    """The standard that the last -std of `flags` names, as the compiler takes it, or None."""
    return next((flag[len("-std="):] for flag in reversed(flags) if flag.startswith("-std=")), None)


def main():
    scratch = tempfile.mkdtemp()
    try:
        write(os.path.join(scratch, "sources", "madeup", "Android.bp"), BLUEPRINT)
        for name in ("front.cpp", "front.c", "archive.cpp", "part.cpp", "lib.rs", "bridge.cpp", "rust_include/rust.h",
                     "bridge_include/bridge.h", "std.c", "std.cpp", "nognu.c", "nognu.cpp", "defines.cpp"):
            write(os.path.join(scratch, "sources", "madeup", name), "int x;\n")
        write(os.path.join(scratch, "ours", "modules.bp"), "")

        class MadeUp(soong_plan.Planner):
            TREE = {"external/madeup": "madeup"}
            OURS = "minato/madeup"
            OURS_FOLDER = os.path.join(scratch, "ours")
            OVERLAY = os.path.join(scratch, "overlay")
            PATCHES = os.path.join(scratch, "patches")
            GUEST_SYSTEM = ["libc", "libm", "libdl", "libguest", "libguestinarchive", "libwholeguest", "libguestinpart"]
            LIBRARIES = ["libfront", "libstd"]
            DROPPED = {"librustffi"}
            PREFIX = "madeup"
            GENERATED = "MADEUP_GENERATED"
            TREE_VARIABLE = "MADEUP_TREE"

        planner = MadeUp(os.path.join(scratch, "sources"), os.path.join(scratch, "tree"))
        lines = planner.plan_linked()
        front = call(lines, "madeup_unit(madeup_libfront_shared")
        check(field(front, "CPPFLAGS")[:1] == ["-std=gnu++2b"] and "-std=gnu2x" not in field(front, "CPPFLAGS"),
              f"cpp_std \"experimental\" compiles as -std=gnu++2b:\n{front}")
        check(field(front, "CONLYFLAGS")[:1] == ["-std=gnu2x"], f"c_std \"experimental\" compiles as -std=gnu2x:\n{front}")
        own = call(lines, "madeup_unit(madeup_libstd_shared")
        check(field(own, "CFLAGS") == ["-std=gnu89"] and field(own, "CONLYFLAGS") == ["-std=gnu17"]
              and standard(field(own, "CFLAGS") + field(own, "CONLYFLAGS")) == "gnu17",
              f"Soong's gnu17 comes after the cflags' -std=gnu89:\n{own}")
        check(field(own, "CPPFLAGS")[:2] == ["-std=gnu++20", "-std=c++17"]
              and standard(field(own, "CFLAGS") + field(own, "CPPFLAGS")) == "c++17",
              f"Soong's gnu++20 comes after the cflags and before the cppflags' -std=c++17:\n{own}")
        nognu = call(lines, "madeup_unit(madeup_libnognu_static")
        check(field(nognu, "CONLYFLAGS") == ["-std=c11", "-DNOGNU"] and field(nognu, "CPPFLAGS")[:1] == ["-std=c++20"],
              f"gnu_extensions: false compiles c_std gnu11 as -std=c11 and the default C++ as -std=c++20:\n{nognu}")
        text = next((line for line in lines if line.startswith("madeup_object_text(")), "")
        check(field(text, "CPPFLAGS") == ["-std=gnu++20"] and "-std=c++14" in field(text, "CFLAGS"),
              f"the cc_object's assembly text compiles with Soong's gnu++20 after its cflags' -std=c++14:\n{text}")
        library = call(lines, "madeup_library(libfront")
        for guest in ("libguest", "libguestinarchive"):
            check(guest in field(library, "NEEDED"), f"libfront needs {guest}: {field(library, 'NEEDED')}")
        check(not any(re.search(r"madeup_libguest\w*_static", line) for line in lines),
              "the plan compiles none of the guest system's libraries")
        for guest in ("libwholeguest", "libguestinpart"):
            check(any(problem.startswith("libfront: ") and guest in problem for problem in planner.problems),
                  f"libfront taking {guest} whole is a problem: {planner.problems}")
        check(len(planner.problems) == 2, f"no other problems: {planner.problems}")
        includes = field(front, "INCLUDES")
        check(os.path.join(planner.tree_root, "external/madeup/rust_include") in includes,
              f"libfront includes the Rust library's own include directory: {includes}")
        check(not any("bridge_include" in include for include in includes),
              f"libfront includes nothing the Rust library's whole_static_libs export: {includes}")
        check(not any("bridge_header" in line for line in lines),
              "the plan generates no header of the Rust library's whole_static_libs")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print(f"soong-plan: {'all checks passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
