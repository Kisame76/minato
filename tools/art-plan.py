#!/usr/bin/env python3
"""Turns the Android.bp files of ART and its libraries into a CMake plan for guest code (ADR-0033 a), as
tools/bionic-plan.py does for bionic: one object library per module and linkage, with the module's own flags and
include directories, custom commands for the generated sources, and the link settings of every shared library and
of dalvikvm. runtime/art/CMakeLists.txt builds the plan with the NDK compiler.

ART's own module types (art/build/art.go) are cc modules with what art.go adds when Soong loads them: art_defaults
gets the global flags of globalFlags and deviceFlags, every art_cc_* module -DART_TARGET and -DART_TARGET_ANDROID,
and the codegen properties are those of arm64. runtime/art/modules.bp, read in the same syntax, holds what Minato
puts in place of modules of projects we do not fetch and of services the iPhone does not have.

The build reads its sources from <tree> next to the plan: AOSP's tree layout of symbolic links to the fetched
files, with runtime/art/overlay's files in place of AOSP's at the same path, and runtime/art/patches applied, in
order, to real copies. So a patched header is the one every include finds, also a quoted one next to its includer,
and includes that climb out of their project ("../../external/fdlibm/fdlibm.h") find the other as in AOSP's tree.

Usage: tools/art-plan.py <sources-directory> <output.cmake>
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blueprint  # noqa: E402
import soong_plan  # noqa: E402
from soong_plan import cmake_list, cmake_quote  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS = "minato/art"  # runtime/art in the tree, with modules.bp
OVERLAY = os.path.join(ROOT, "runtime", "art", "overlay")
PATCHES = os.path.join(ROOT, "runtime", "art", "patches")

# Where the projects of the partial AOSP tree lie below build/sources (tools/fetch-sources.py --set art).
TREE = {
    "art": "art",
    "libcore": "libcore",
    "external/icu": "icu",
    "external/boringssl": "boringssl",
    "libnativehelper": "libnativehelper",
    "external/fdlibm": "fdlibm",
    "external/expat": "expat",
    "external/zlib": "zlib",
    "external/lz4": "lz4",
    "external/lzma": "lzma",
    "external/libcap": "libcap",
    "external/tinyxml2": "tinyxml2",
    "external/cpu_features": "cpu_features",
    "external/fmtlib": "fmtlib",
    "external/dlmalloc": "dlmalloc",
    "system/libbase": "libbase",
    "system/libziparchive": "libziparchive",
    "system/libprocinfo": "libprocinfo",
    "system/unwinding": "unwinding",
    "system/logging": "logging",
    "system/core": "system_core",
    "bionic": "bionic",
}

# The guest system's libraries (tools/build-bionic.sh): ART links them as they are, the plan builds none of them.
# libdl_android.so, with the linker's namespace functions, is one of them, as on Android (ADR-0033 decision 2).
GUEST_SYSTEM = ["libc", "libm", "libdl", "libdl_android", "ld-android", "liblog", "libz"]

# What the plan builds: the libraries ART loads at start or links, in its release and its debug flavor, and the
# launcher (ADR-0033 decisions 2, 10 and 11). What they link comes with them.
LIBRARIES = ["libart", "libartd", "libopenjdkjvm", "libopenjdkjvmd", "libjavacore", "libopenjdk", "libopenjdkd",
             "libicu_jni", "libartpalette-system", "libsigchain", "libnativehelper", "libnativeloader"]
# dalvikvm, and boringssl-test of runtime/art/modules.bp (ADR-0046 b).
PROGRAMS = ["dalvikvm", "boringssl-test"]

# The boot jars (ADR-0033 decision 10), which runtime/art/boot_jars.py builds; and what core-all, which Soong
# compiles them against, has beyond their sources: java.base's module-info and the stubs that let javac compile
# lambdas. Modules of projects we do not fetch that they name are compile-time stand-ins in runtime/art/java.
BOOT_JARS = ["core-oj", "core-libart", "core-icu4j"]
COMPILE_ONLY = [("libcore", ":openjdk_lambda_stub_files"), ("libcore", "luni/src/module/java/module-info.java"),
                (OURS, "java/**/*.java")]
STAND_INS = {"app-compat-annotations-source", "framework-annotations-lib", "unsupportedappusage",
             "icu_aconfig_flags_lib"}

# Dependencies left out (ADR-0033 decision 2): statsd, which the runtime reports to only through
# metrics/statsd.cc (the overlay gives it no backend, as ART's host build has none) and which nothing else of the
# set calls; and vixl, the assembler of the JIT compiler, which runtime/art/modules.bp leaves out.
DROPPED = {"libstatspull", "libstatssocket", "libstatslog_art", "libvixl", "libvixld"}

# art.go's flags (ADR-0033 decision 2): globalFlags with ART_TEST_DEBUG_GC's semispace collector and TLAB, without
# read barriers and with the page size taken at run time; deviceFlags with the frame limit and the boot image
# address of an Android 15 device; addImplicitFlags for every art_cc_* module.
GLOBAL_CFLAGS = ["-O3", "-DART_DEFAULT_GC_TYPE_IS_SS", "-DART_USE_TLAB=1", "-DART_STACK_OVERFLOW_GAP_arm=8192",
                 "-DART_STACK_OVERFLOW_GAP_arm64=8192", "-DART_STACK_OVERFLOW_GAP_riscv64=8192",
                 "-DART_STACK_OVERFLOW_GAP_x86=8192", "-DART_STACK_OVERFLOW_GAP_x86_64=8192",
                 "-DART_PAGE_SIZE_AGNOSTIC=1", "-DUSE_D8_DESUGAR=1"]
DEVICE_CFLAGS = ["-Wframe-larger-than=1736", "-DART_FRAME_SIZE_LIMIT=1736", "-DART_BASE_ADDRESS=0x70000000",
                 "-DART_BASE_ADDRESS_MIN_DELTA=(-0x1000000)", "-DART_BASE_ADDRESS_MAX_DELTA=0x1000000"]
IMPLICIT_CFLAGS = ["-DART_TARGET", "-DART_TARGET_ANDROID"]
ART_KINDS = {
    "art_cc_library": "cc_library", "art_cc_library_static": "cc_library_static", "art_cc_binary": "cc_binary",
    "art_cc_test": "cc_test", "art_cc_test_library": "cc_test_library", "art_cc_defaults": "cc_defaults",
    "art_global_defaults": "cc_defaults", "art_debug_defaults": "cc_defaults",
}

# BoringSSL's libcrypto as libjavacrypto links it, with the ARMv8 crypto extension at compile time (ADR-0046
# decision 1): BoringSSL then takes AES, PMULL, SHA-1 and SHA-256 as given (crypto/internal.h). Otherwise it reads
# them from AT_HWCAP, which reports FP and ASIMD only (runtime/src/loader.h), and runs AES without the instructions.
MODULE_CFLAGS = {"libcrypto_static": ["-march=armv8-a+crypto"]}


class ArtPlanner(soong_plan.Planner):
    """ART's set: runtime/art's modules.bp, overlay and patches, art.go's additions, and the boot jars."""

    TREE = TREE
    OURS = OURS
    OURS_FOLDER = os.path.join(ROOT, "runtime", "art")
    OVERLAY = OVERLAY
    PATCHES = PATCHES
    GUEST_SYSTEM = GUEST_SYSTEM
    DROPPED = DROPPED
    LIBRARIES = LIBRARIES
    PROGRAMS = PROGRAMS
    PREFIX = "art"
    GENERATED = "APK_ART_GENERATED"
    TREE_VARIABLE = "APK_ART_TREE"

    def adapt(self, module):
        """What Soong's load hooks and configuration add (soong_plan.Planner.adapt), and MODULE_CFLAGS."""
        super().adapt(module)
        if module.name in MODULE_CFLAGS:
            module.properties = blueprint.merge(module.properties, {"cflags": MODULE_CFLAGS[module.name]})

    def adapt_kind(self, module):
        """art.go's additions for ART's module types."""
        properties = module.properties
        if module.kind not in ART_KINDS:
            return
        kind = module.kind
        module.kind = ART_KINDS[kind]
        additions = {}
        codegen = properties.pop("codegen", {}).get("arm64")
        if codegen:
            additions = blueprint.merge(additions, {"target": {"android": codegen}})
        if kind == "art_global_defaults":
            additions = blueprint.merge(additions, {"cflags": GLOBAL_CFLAGS, "target": {"android": {"cflags": DEVICE_CFLAGS}}})
        if kind in ("art_global_defaults", "art_cc_library", "art_cc_library_static", "art_cc_binary", "art_cc_test",
                    "art_cc_test_library"):
            additions = blueprint.merge(additions, {"target": {"android": {"cflags": IMPLICIT_CFLAGS}}})
        module.properties = blueprint.merge(properties, additions)

    # The boot jars -----------------------------------------------------------------------------------------------

    def java_files(self, module, sources):
        """Tree paths of the Java sources of `sources`, without the stand-ins' modules."""
        result = []
        for source in sources:
            if source.startswith(":") and source[1:].split("{")[0] in STAND_INS:
                continue
            try:
                result += [path for path in blueprint.expand_sources(self.tree, module, [source]) if path.endswith(".java")]
            except KeyError as error:
                self.problems.append(f"{module.properties.get('name', module.directory)}: {error}")
        return result

    def boot_jars(self, description):
        """Writes what runtime/art/boot_jars.py builds the boot jars of: each jar's sources and Java resources, with
        those of its static libraries, and the compile-time sources."""
        jars, inputs = {}, []
        for name in BOOT_JARS:
            sources, resources, pending, seen = [], [], [name], set()
            while pending:
                current = pending.pop(0)
                if current in seen:
                    continue
                seen.add(current)
                module = self.tree.module(current)
                properties = blueprint.flatten(self.tree, module, "shared")
                sources += [self.local(path) for path in self.java_files(module, properties.get("srcs", []))]
                for reference in properties.get("java_resources", []):
                    group = self.tree.module(reference[1:]) if reference.startswith(":") else module
                    patterns = group.properties.get("srcs", []) if reference.startswith(":") else [reference]
                    root = os.path.normpath(os.path.join(group.directory, group.properties.get("path", "")))
                    resources += [[self.local(path), os.path.relpath(path, root)]
                                  for path in blueprint.expand_sources(self.tree, group, patterns)
                                  if os.path.isfile(self.local(path))]  # Soong's globs match files only
                for folder in properties.get("java_resource_dirs", []):
                    root = self.local(os.path.normpath(os.path.join(module.directory, folder)))
                    for directory, _, files in os.walk(root):
                        resources += [[os.path.join(directory, file), os.path.relpath(os.path.join(directory, file), root)]
                                      for file in files]
                pending += [library for library in properties.get("static_libs", []) if library not in STAND_INS]
            jars[name] = {"sources": sorted(set(sources)), "resources": sorted(resources)}
            inputs += jars[name]["sources"] + [path for path, _ in resources]
        compile_only = []
        for directory, source in COMPILE_ONLY:
            compile_only += [self.local(path) for path in
                             self.java_files(blueprint.Module("filegroup", {}, directory), [source])]
        with open(description, "w") as handle:
            json.dump({"jars": jars, "compile_only": sorted(compile_only)}, handle, indent=1)
        self.lines.append(f"art_boot_jars({cmake_quote(description)}\n  JARS {' '.join(BOOT_JARS)}\n"
                          f"  DEPENDS {cmake_list(inputs + compile_only)})")

    def plan(self, description):
        """The plan's lines: the libraries and programs, then the boot jars, whose description goes to
        `description`."""
        self.plan_linked()
        self.boot_jars(description)
        return self.lines


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    build = os.path.dirname(os.path.abspath(sys.argv[2]))
    planner = ArtPlanner(os.path.abspath(sys.argv[1]), os.path.join(build, "tree"))
    lines = planner.plan(os.path.join(build, "boot_jars.json"))
    if planner.problems:
        raise SystemExit("art-plan: " + "\n  ".join(["cannot plan the build:"] + sorted(set(planner.problems))))
    with open(sys.argv[2], "w") as handle:
        handle.write("# Generated by tools/art-plan.py from the Android.bp files of ART and its libraries. Do not edit.\n")
        handle.write("\n\n".join(lines) + "\n")
    units = [target for target in planner.units.values() if target]
    print(f"art-plan: {len(planner.linked)} libraries and programs, {len(units)} compile units, "
          f"{len(planner.generated)} generators")


if __name__ == "__main__":
    main()
