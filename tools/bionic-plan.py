#!/usr/bin/env python3
"""Turns bionic's Android.bp files into a CMake plan for the guest libc (ADR-0021): one object
library per bionic module, with the module's own flags and include directories, custom commands
for the generated sources, and the link settings of libc.so, libm.so, libdl.so, libdl_android.so and libstdc++.so.
Soong would do the same from the same files; runtime/bionic/CMakeLists.txt builds the plan with the
NDK compiler.

Usage: tools/bionic-plan.py <sources-directory> <output.cmake>
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blueprint  # noqa: E402

# Where the projects of the partial AOSP tree lie below build/sources (tools/fetch-sources.py).
TREE = {
    "bionic": "bionic",
    "external/scudo": "scudo",
    "external/gwp_asan": "gwp_asan",
    "external/arm-optimized-routines": "arm-optimized-routines",
    "system/core": "system_core",
    "build/make": "build_make",
}

# Our changes to bionic (ADR-0019, ADR-0021), relative to the repository root:
#   OVERLAY    files that replace bionic's at the same tree path, e.g. arch-arm64/bionic/setjmp.S;
#   ADDITIONS  our own files compiled as part of a bionic module, with its flags;
#   BORROWED   files of other AOSP modules compiled into a bionic module, by tree path: libc.so gets
#              linker64's TLS descriptor resolvers, because our loader does linker64's work;
#   REPLACED   modules whose sources we replace with ours, compiled with the flags of another module;
#   VERSION_SCRIPTS  more symbols a library exports.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OVERLAY = os.path.join(ROOT, "runtime", "bionic", "overlay")
OUR_INCLUDES = [os.path.join(ROOT, "runtime", "include")]
ADDITIONS = {"libc_bootstrap": [os.path.join(ROOT, "runtime", "bionic", "src", name) for name in ("libc_start.cpp", "sigreturn.S")]}
BORROWED = {"libc_bootstrap": ["bionic/linker/arch/arm64/tlsdesc_resolver.S"]}
REPLACED = {"ld-android": ("libc_bootstrap", [os.path.join(ROOT, "runtime", "bionic", "src", "ld_android.cpp")])}
VERSION_SCRIPTS = {
    "libc": [os.path.join(ROOT, "runtime", "bionic", "src", "libc_start.map")],
    "ld-android": [os.path.join(ROOT, "runtime", "bionic", "src", "ld_android.map")],
}

# Filegroups of projects we fetch only single files of, with what they contain.
FILEGROUPS = {
    "android_filesystem_config_header": ("system/core/libcutils", ["include/private/android_filesystem_config.h"]),
}
# ld-android.so is a stub that libc.so and libdl.so link against; at run time the runtime's own
# version takes its place (ADR-0021). libdl_android.so has the linker's namespace functions, which
# ART's libart, libnativeloader, libnativebridge and libopenjdk link (ADR-0033). libstdc++.so is the NDK's "system"
# C++ runtime, operator new and delete and the static guards, which apps built with ANDROID_STL=system link.
LIBRARIES = ["ld-android", "libdl", "libdl_android", "libc", "libm", "libstdc++"]

# Modules from projects we do not fetch, and what stands in for them (runtime/bionic/CMakeLists.txt):
# the NDK's liblog headers and its LLVM libunwind.
SUBSTITUTES = {"liblog_headers": "ndk_liblog_headers", "libunwind-exported": "ndk_libunwind"}
SOURCE_EXTENSIONS = (".c", ".cpp", ".cc", ".S", ".s")


def cmake_quote(value):
    """A quoted CMake argument. `$` stays: the generated paths use ${APK_BIONIC_GENERATED}."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace(";", "\\;") + '"'


def cmake_list(values):
    return " ".join(cmake_quote(value) for value in values)


def cmake_flags(values):
    """Compile flags. A flag string can hold an option and its argument ("-include foo.h"); CMake
    would drop the repeated option when de-duplicating, so such strings go through SHELL:."""
    return " ".join(cmake_quote("SHELL:" + value if " " in value.strip() else value.strip()) for value in values)


class Planner:
    def __init__(self, sources):
        self.roots = {tree_path: os.path.join(sources, folder) for tree_path, folder in TREE.items()}
        self.tree = blueprint.Tree(self.roots)
        blueprint.load_all(self.tree, self.roots)
        for name, (directory, sources) in FILEGROUPS.items():
            self.tree.modules[name] = blueprint.Module("filegroup", {"name": name, "srcs": sources}, directory)
        self.lines = []
        self.units = {}
        self.generated = {}
        self.problems = []

    def local(self, tree_path):
        overlay = os.path.join(OVERLAY, tree_path)
        if os.path.isfile(overlay):  # files only: folders of the overlay are not include paths
            return overlay
        path = self.tree.local(tree_path)
        if path is None:
            self.problems.append(f"{tree_path} lies outside the fetched projects")
            return tree_path
        return os.path.abspath(path)

    def generated_output(self, reference):
        """Adds the genrule `reference` (":name") and returns the path of its output."""
        name = reference[1:]
        if name in self.generated:
            return self.generated[name]
        module = self.tree.module(name)
        if module.kind != "genrule":
            self.problems.append(f"{reference} is a {module.kind}, not a genrule")
            return None
        properties = module.properties
        output = "${APK_BIONIC_GENERATED}/" + properties["out"][0]
        inputs = [self.local(source) for source in
                  blueprint.expand_sources(self.tree, module, properties.get("srcs", []))]
        tools = {}
        for tool in properties.get("tools", []):
            tool_module = self.tree.module(tool)
            script = tool_module.properties.get("main") or tool_module.properties["srcs"][0]
            tools[tool] = self.local(os.path.normpath(os.path.join(tool_module.directory, script)))
        command = properties["cmd"]
        command = re.sub(r"\$\(location ([^)]+)\)", lambda match: "python3 '" + tools[match.group(1)] + "'", command)
        command = command.replace("$(in)", " ".join(f"'{path}'" for path in inputs))
        command = command.replace("$(out)", f"'{output}'")
        self.lines.append(f"add_custom_command(OUTPUT {cmake_quote(output)}\n"
                          f"  COMMAND sh -c {cmake_quote(command)}\n"
                          f"  DEPENDS {cmake_list(inputs + list(tools.values()))}\n"
                          f"  VERBATIM)")
        self.generated[name] = output
        return output

    def add_unit(self, unit):
        name = unit["name"]
        if name in self.units:
            return
        if name in REPLACED:
            flags_from, replacement = REPLACED[name]
            model = blueprint.compile_unit(self.tree, flags_from, "static", set())
            unit = dict(model, name=name, sources=[], shared_libs=unit["shared_libs"])
            unit["extra_sources"] = replacement
        sources = []
        for source in unit["sources"]:
            if source.startswith(":"):
                output = self.generated_output(source)
                if output:
                    sources.append(output)
                continue
            if not source.endswith(SOURCE_EXTENSIONS):
                continue
            path = self.local(source)
            if not os.path.exists(path):
                self.problems.append(f"{name}: missing source {source}")
                continue
            sources.append(path)
        sources += ADDITIONS.get(name, []) + unit.get("extra_sources", [])
        for tree_path in BORROWED.get(name, []):
            path = self.local(tree_path)
            if not os.path.exists(path):
                self.problems.append(f"{name}: missing borrowed source {tree_path}")
                continue
            sources.append(path)
        includes = [self.local(include) for include in unit["includes"] if self.tree.local(include) is not None]
        if ADDITIONS.get(name) or unit.get("extra_sources"):
            includes += OUR_INCLUDES
        for header in unit["generated_headers"]:
            output = self.generated_output(":" + header)
            if output:
                sources.append(output)  # a header: not compiled, but generated before the unit
                if "${APK_BIONIC_GENERATED}" not in includes:
                    includes.append("${APK_BIONIC_GENERATED}")
        missing_includes = [include for include in unit["includes"] if self.tree.local(include) is None]
        self.units[name] = sources
        target = "bionic_" + re.sub(r"[^A-Za-z0-9_]", "_", name)
        if not sources:
            self.lines.append(f"# {name}: nothing to compile")
            return
        self.lines.append(
            f"apk_bionic_unit({target}\n"
            f"  SOURCES {cmake_list(sources)}\n"
            f"  CFLAGS {cmake_flags(unit['cflags'])}\n"
            f"  CONLYFLAGS {cmake_flags(unit['conlyflags'])}\n"
            f"  CPPFLAGS {cmake_flags(unit['cppflags'])}\n"
            f"  ASFLAGS {cmake_flags(unit['asflags'])}\n"
            f"  INCLUDES {cmake_list(includes)})"
            + (f"\n# {name} also names include directories we do not have: {', '.join(missing_includes)}"
               if missing_includes else ""))

    def add_library(self, name):
        units, missing = blueprint.library_plan(self.tree, name)
        for unit in units:
            self.add_unit(unit)
        top = units[0]
        properties = blueprint.flatten(self.tree, self.tree.module(name), "shared")
        version_script = properties.get("version_script")
        if version_script:
            version_script = (self.generated_output(version_script) if version_script.startswith(":")
                              else self.local(os.path.join(self.tree.module(name).directory, version_script)))
        version_scripts = ([version_script] if version_script else []) + VERSION_SCRIPTS.get(name, [])
        substitutes = sorted(SUBSTITUTES[module] for module in missing if module in SUBSTITUTES)
        unknown = sorted(module for module in missing if module not in SUBSTITUTES)
        if unknown:
            self.problems.append(f"{name}: modules we do not have: {', '.join(unknown)}")
        targets = ["bionic_" + re.sub(r"[^A-Za-z0-9_]", "_", unit["name"]) for unit in units if self.units[unit["name"]]]
        static_units = sorted({static for unit in units for static in unit["static_libs"]})
        self.lines.append(
            f"apk_bionic_library({name}\n"
            f"  UNITS {' '.join(targets)}\n"
            f"  STATIC {cmake_list(static_units)}\n"
            f"  NEEDED {cmake_list(top['shared_libs'])}\n"
            f"  SUBSTITUTES {cmake_list(substitutes)}\n"
            f"  VERSION_SCRIPTS {cmake_list(version_scripts)}\n"
            f"  LDFLAGS {cmake_list(properties.get('ldflags', []))}\n"
            f"  NOCRT {'TRUE' if properties.get('nocrt') else 'FALSE'})")

    def plan(self):
        # Static libraries that a shared one links as archives (not whole) come first, so that the
        # libraries find their targets.
        for library in LIBRARIES:
            units, _ = blueprint.library_plan(self.tree, library)
            for static in sorted({static for unit in units for static in unit["static_libs"]}):
                if static in self.tree.modules and static not in self.units:
                    static_units, _ = blueprint.library_plan(self.tree, static)
                    for unit in static_units:
                        self.add_unit(unit)
        for library in LIBRARIES:
            self.add_library(library)
        return self.lines


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    planner = Planner(os.path.abspath(sys.argv[1]))
    lines = planner.plan()
    if planner.problems:
        raise SystemExit("bionic-plan: " + "\n  ".join(["cannot plan the build:"] + planner.problems))
    with open(sys.argv[2], "w") as handle:
        handle.write("# Generated by tools/bionic-plan.py from bionic's Android.bp files. Do not edit.\n")
        handle.write("\n\n".join(lines) + "\n")
    count = sum(len(sources) for sources in planner.units.values())
    print(f"bionic-plan: {len(planner.units)} modules, {count} sources, {len(planner.generated)} generated files")


if __name__ == "__main__":
    main()
