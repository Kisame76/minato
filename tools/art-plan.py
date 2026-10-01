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
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blueprint  # noqa: E402

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
PROGRAMS = ["dalvikvm"]

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
SOURCE_EXTENSIONS = (".c", ".cpp", ".cc", ".S", ".s")
GENERATORS = ("genrule", "gensrcs", "cc_genrule")


def cmake_quote(value):
    """A quoted CMake argument. `$` stays: the generated paths use ${APK_ART_GENERATED}."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace(";", "\\;") + '"'


def cmake_list(values):
    return " ".join(cmake_quote(value) for value in values)


def cmake_flags(values):
    """Compile flags. A flag string can hold an option and its argument ("-include foo.h"); CMake would drop the
    repeated option when de-duplicating, so such strings go through SHELL:."""
    return " ".join(cmake_quote("SHELL:" + value if " " in value.strip() else value.strip()) for value in values)


def identifier(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def unique(values):
    result = []
    for value in values:
        if value not in result:
            result.append(value)
    return result


def read(path):
    with open(path, "rb") as handle:
        return handle.read()


def make_tree(tree, projects):
    """Brings <tree> up to date: a symbolic link for every file of `projects` (tree path -> folder) and of the
    overlay, real files for what the patches change. Files that already are what they should be stay untouched, so
    that their builds stay current. Returns the problems."""
    wanted = {}
    for tree_path, folder in projects.items():
        for directory, folders, files in os.walk(folder):
            links = [name for name in folders if os.path.islink(os.path.join(directory, name))]
            for name in files + links:
                path = os.path.join(directory, name)
                wanted[os.path.normpath(os.path.join(tree_path, os.path.relpath(path, folder)))] = path
    for directory, _, files in os.walk(OVERLAY):
        for name in files:
            path = os.path.join(directory, name)
            wanted[os.path.relpath(path, OVERLAY)] = path
    patched, problems = {}, []
    for patch in sorted(glob.glob(os.path.join(PATCHES, "*.patch"))):
        changed = re.findall(r"^\+\+\+ b/(\S+)", read(patch).decode(), re.M)
        with tempfile.TemporaryDirectory() as scratch:
            for tree_path in changed:
                os.makedirs(os.path.dirname(os.path.join(scratch, tree_path)), exist_ok=True)
                with open(os.path.join(scratch, tree_path), "wb") as handle:
                    handle.write(patched[tree_path] if tree_path in patched else read(wanted[tree_path]))
            result = subprocess.run(["patch", "-p1", "-F", "0", "-s", "-d", scratch, "-i", patch],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                problems.append(f"{os.path.basename(patch)} does not apply: {(result.stdout + result.stderr).strip()}")
                continue
            for tree_path in changed:
                patched[tree_path] = read(os.path.join(scratch, tree_path))
    present = set()
    for directory, folders, files in os.walk(tree):
        for name in files + [name for name in folders if os.path.islink(os.path.join(directory, name))]:
            present.add(os.path.relpath(os.path.join(directory, name), tree))
    for tree_path in present - wanted.keys():
        os.remove(os.path.join(tree, tree_path))
    for tree_path, target in wanted.items():
        path = os.path.join(tree, tree_path)
        if tree_path in patched:
            if os.path.isfile(path) and not os.path.islink(path) and read(path) == patched[tree_path]:
                continue
        elif os.path.islink(path) and os.readlink(path) == target:
            continue
        if os.path.lexists(path):
            os.remove(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if tree_path in patched:
            with open(path, "wb") as handle:
                handle.write(patched[tree_path])
        else:
            os.symlink(target, path)
    for directory, folders, files in os.walk(tree, topdown=False):
        if directory != tree and not os.listdir(directory):
            os.rmdir(directory)
    return problems


class Planner:
    def __init__(self, sources, tree):
        self.tree_root = tree
        projects = {tree_path: os.path.join(sources, folder) for tree_path, folder in TREE.items()}
        projects[OURS] = os.path.join(ROOT, "runtime", "art")
        self.problems = make_tree(tree, projects)
        self.tree = blueprint.Tree({tree_path: os.path.join(tree, tree_path) for tree_path in projects})
        for tree_path in TREE:
            for path in sorted(glob.glob(os.path.join(tree, tree_path, "**", "Android.bp"), recursive=True)):
                self.load(os.path.relpath(path, tree))
        self.load(f"{OURS}/modules.bp")  # last: Minato's modules take the place of those of the same name
        for module in self.tree.modules.values():
            self.adapt(module)
        self.system_includes = unique(include for name in ("libc", "libm", "libdl")
                                      for include in blueprint.exported_includes(self.tree, name, set()))
        self.lines = [f"set(APK_ART_TREE {cmake_quote(tree)})"]
        self.units = {}  # (module, linkage) -> its object library, or None with nothing to compile
        self.linked = set()  # the shared libraries and programs
        self.generated = {}

    def load(self, tree_path):
        """One Blueprint file, and the files its top-level `build` list names, as Soong reads them."""
        path = self.tree.local(tree_path)
        variables = {}
        with open(path) as handle:
            modules = blueprint.Parser(handle.read(), path, variables).file()
        for kind, properties in modules:
            if "name" in properties and kind not in ("ndk_library", "ndk_headers", "versioned_ndk_headers"):
                self.tree.modules[properties["name"]] = blueprint.Module(kind, properties, os.path.dirname(tree_path))
        for extra in variables.get("build", []):
            self.load(os.path.normpath(os.path.join(os.path.dirname(tree_path), extra)))

    def adapt(self, module):
        """What Soong's load hooks and configuration add: art.go's for ART's module types, the default branch of
        soong_config_variables, and nothing of product_variables (a user build)."""
        properties = module.properties
        properties.pop("product_variables", None)
        for settings in properties.pop("soong_config_variables", {}).values():
            module.properties = properties = blueprint.merge(properties, settings.get("conditions_default", {}))
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

    def local(self, tree_path):
        path = self.tree.local(tree_path)
        if path is None:
            self.problems.append(f"{tree_path} lies outside the fetched projects")
            return tree_path
        return os.path.abspath(path)

    def properties(self, name, linkage):
        properties = blueprint.flatten(self.tree, self.tree.module(name), linkage)
        for key in ("shared_libs", "static_libs", "whole_static_libs", "header_libs"):
            excluded = properties.get("exclude_" + key, [])
            properties[key] = unique(value for value in properties.get(key, []) if value not in excluded
                                     and value not in DROPPED)
        return properties

    # Generated files ------------------------------------------------------------------------------------------

    def tool_command(self, module, command, tools, tool_files):
        """The command with $(location ...) of its tools and tool files resolved, Python scripts run with python3;
        and the files the command runs. Paths go in as they are, as Soong's: the commands quote them where they
        need to, and ours hold no spaces."""
        locations, paths = {}, []
        for tool in tools:
            tool_module = self.tree.module(tool.lstrip(":"))
            if tool_module.kind == "cc_binary":
                path = self.host_tool(tool_module)
                locations[tool] = path
            else:
                script = tool_module.properties.get("main") or tool_module.properties["srcs"][0]
                path = self.local(os.path.normpath(os.path.join(tool_module.directory, script)))
                locations[tool] = "python3 " + path
            paths.append(path)
        for tool_file in tool_files:
            if tool_file.startswith(":"):
                continue  # a filegroup the tool reads by itself
            path = self.local(os.path.normpath(os.path.join(module.directory, tool_file)))
            locations[tool_file] = ("python3 " + path) if path.endswith(".py") else path
            paths.append(path)
        return re.sub(r"\$\(location ([^)]+)\)", lambda match: locations[match.group(1)], command), paths

    def host_tool(self, module):
        """A cc_binary that a generator runs on the Mac, built with the Mac's compiler, after the headers it has
        generated."""
        name = module.properties["name"]
        output = f"${{APK_ART_GENERATED}}/host/{name}"
        if name in self.generated:
            return output
        properties = blueprint.flatten(self.tree, module, "static")
        sources = [self.local(path) for path in blueprint.expand_sources(self.tree, module, properties.get("srcs", []))]
        includes = [self.local(os.path.normpath(os.path.join(module.directory, path)))
                    for path in [""] + properties.get("local_include_dirs", [])]
        headers = []
        for generator in properties.get("generated_headers", []):
            outputs, directory = self.generated_outputs(generator)
            headers += outputs
            includes.append(directory)
        self.lines.append(f"art_host_tool({cmake_quote(output)}\n  SOURCES {cmake_list(sources)}\n"
                          f"  INCLUDES {cmake_list(includes)}\n  DEPENDS {cmake_list(headers)})")
        self.generated[name] = output
        return output

    def object_text(self, module):
        """A cc_object compiled with -S (asm_defines.s): its assembly text."""
        name = module.properties["name"]
        output = f"${{APK_ART_GENERATED}}/{name}/{name}"
        unit = blueprint.compile_unit(self.tree, name, "static", set())
        sources = [self.local(source) for source in unit["sources"]]
        includes, system = self.includes(unit)
        self.lines.append(
            f"art_object_text({cmake_quote(output)}\n  SOURCES {cmake_list(sources)}\n"
            f"  CFLAGS {cmake_flags(unit['cflags'])}\n  CPPFLAGS {cmake_flags(unit['cppflags'])}\n"
            f"  INCLUDES {cmake_list(includes)}\n  SYSTEM_INCLUDES {cmake_list(system)})")
        return output

    def generated_outputs(self, reference):
        """Adds the generator `reference` (":name" or "name") and returns (outputs, include directory)."""
        name = reference.lstrip(":").split("{")[0]
        if name in self.generated:
            return self.generated[name]
        if name not in self.tree.modules:
            self.problems.append(f"{name}: a generator we do not have")
            return [], None
        module = self.tree.module(name)
        properties = module.properties
        directory = f"${{APK_ART_GENERATED}}/{name}"
        if module.kind == "filegroup":  # one of ours in place of a generator, with the sources it would make
            sources = [self.local(path) for path in
                       blueprint.expand_sources(self.tree, module, properties.get("srcs", []))]
            includes = [self.local(os.path.normpath(os.path.join(module.directory, path)))
                        for path in properties.get("export_include_dirs", [])]
            self.generated[name] = (sources, includes[0] if includes else None)
            return self.generated[name]
        if module.kind not in GENERATORS:
            self.problems.append(f"{name} is a {module.kind}, not a generator")
            return [], None
        command, tool_paths = self.tool_command(module, properties["cmd"], properties.get("tools", []),
                                                properties.get("tool_files", []))
        inputs = []
        for source in properties.get("srcs", []):
            if source.startswith(":") and self.tree.module(source[1:]).kind == "cc_object":
                inputs.append(self.object_text(self.tree.module(source[1:])))
            else:
                inputs += [self.local(path) for path in blueprint.expand_sources(self.tree, module, [source])]
        command = command.replace("$(genDir)", directory)
        if module.kind == "gensrcs":
            # One output per input, named after it, with the command run in the tree's root so that the tool sees
            # tree paths, as under Soong.
            outputs = []
            extension = properties.get("output_extension", "cpp")
            for path in inputs:
                relative = os.path.relpath(path, self.tree_root)
                output = f"{directory}/{os.path.splitext(relative)[0]}.{extension}"
                single = command.replace("$(in)", relative).replace("$(out)", output)
                self.lines.append(f"art_generate({cmake_quote(output)}\n  COMMAND {cmake_quote(single)}\n"
                                  f"  DEPENDS {cmake_list([path] + tool_paths)})")
                outputs.append(output)
        else:
            outputs = [f"{directory}/{out}" for out in properties["out"]]
            single = command.replace("$(in)", " ".join(inputs)).replace("$(out)", " ".join(outputs))
            self.lines.append(f"art_generate({cmake_list(outputs)}\n  COMMAND {cmake_quote(single)}\n"
                              f"  DEPENDS {cmake_list(inputs + tool_paths)})")
        self.generated[name] = (outputs, directory)
        return self.generated[name]

    def exported_generated(self, name, seen=None):
        """Generated headers a module exports through its header libraries (export_generated_headers)."""
        seen = set() if seen is None else seen
        if name in seen or name not in self.tree.modules:
            return []
        seen.add(name)
        properties = blueprint.flatten(self.tree, self.tree.module(name), "static")
        result = list(properties.get("export_generated_headers", []))
        for key in ("export_header_lib_headers", "export_static_lib_headers", "export_shared_lib_headers",
                    "whole_static_libs"):
            for dependency in properties.get(key, []):
                result += self.exported_generated(dependency, seen)
        return result

    # Compile units and libraries --------------------------------------------------------------------------------

    def includes(self, unit):
        """The unit's include directories, and bionic's as system directories after libc++'s."""
        includes, system = [], []
        for include in unit["includes"]:
            if self.tree.local(include) is None:
                continue
            (system if include in self.system_includes else includes).append(self.local(include))
        return includes, system + [self.local(include) for include in self.system_includes
                                   if self.local(include) not in system]

    def add_unit(self, name, linkage):
        key = (name, linkage)
        if key in self.units:
            return self.units[key]
        module = self.tree.module(name)
        properties = self.properties(name, linkage)
        unit = blueprint.compile_unit(self.tree, name, linkage, set())
        sources, generated_headers = [], []
        for source in unit["sources"]:
            if source.startswith(":"):
                outputs, _ = self.generated_outputs(source)
                sources += [output for output in outputs if output.endswith(SOURCE_EXTENSIONS)]
                continue
            if not source.endswith(SOURCE_EXTENSIONS):
                continue
            path = self.local(source)
            if not os.path.exists(path):
                self.problems.append(f"{name}: missing source {source}")
                continue
            sources.append(path)
        includes, system = self.includes(unit)
        for generator in properties.get("generated_sources", []):
            outputs, directory = self.generated_outputs(generator)
            sources += [output for output in outputs if output.endswith(SOURCE_EXTENSIONS)]
            generated_headers += [output for output in outputs if not output.endswith(SOURCE_EXTENSIONS)]
            if directory:
                includes.append(directory)
        header_generators = list(properties.get("generated_headers", []))
        for dependency in (properties.get("header_libs", []) + properties.get("static_libs", [])
                           + properties.get("whole_static_libs", []) + properties.get("shared_libs", [])):
            header_generators += self.exported_generated(dependency)
        for generator in unique(header_generators):
            outputs, directory = self.generated_outputs(generator)
            generated_headers += outputs
            if directory:
                includes.append(directory)
        cflags = list(unit["cflags"])
        cppflags = list(unit["cppflags"]) + (["-frtti"] if properties.get("rtti") else [])
        if properties.get("cpp_std"):
            cppflags.append(f"-std={properties['cpp_std']}")
        if properties.get("c_std"):
            unit["conlyflags"] = [f"-std={properties['c_std']}"] + list(unit["conlyflags"])
        target = f"art_{identifier(name)}_{linkage}"
        self.units[key] = target if sources else None
        if not sources:
            self.lines.append(f"# {name} ({linkage}): nothing to compile")
            return None
        self.lines.append(
            f"art_unit({target}\n"
            f"  SOURCES {cmake_list(sources)}\n"
            f"  GENERATED_HEADERS {cmake_list(unique(generated_headers))}\n"
            f"  CFLAGS {cmake_flags(cflags)}\n"
            f"  CONLYFLAGS {cmake_flags(unit['conlyflags'])}\n"
            f"  CPPFLAGS {cmake_flags(cppflags)}\n"
            f"  ASFLAGS {cmake_flags(unit['asflags'])}\n"
            f"  INCLUDES {cmake_list(unique(includes))}\n"
            f"  SYSTEM_INCLUDES {cmake_list(system)})")
        return target

    def static_closure(self, name):
        """The units a static library brings into a link: its own, those of its whole_static_libs, and, as
        archives, those of its static_libs, recursively (Soong passes them all to the final link)."""
        whole, archives, seen = [], [], set()

        def visit(dependency, whole_archive):
            if dependency in seen or dependency in DROPPED:
                return
            seen.add(dependency)
            if dependency not in self.tree.modules:
                self.problems.append(f"{name}: links {dependency}, which we do not have")
                return
            target = self.add_unit(dependency, "static")
            if target:
                (whole if whole_archive else archives).append(target)
            properties = self.properties(dependency, "static")
            for inner in properties.get("whole_static_libs", []):
                visit(inner, whole_archive)
            for inner in properties.get("static_libs", []):
                visit(inner, False)

        properties = self.properties(name, "shared")
        for dependency in properties.get("whole_static_libs", []):
            visit(dependency, True)
        for dependency in properties.get("static_libs", []):
            visit(dependency, False)
        return whole, archives

    def add_linked(self, name, program=False):
        """A shared library or program, and every shared library of the set it needs."""
        if name in self.linked:
            return
        self.linked.add(name)
        if name not in self.tree.modules:
            self.problems.append(f"{name}: a library we do not have")
            return
        module = self.tree.module(name)
        properties = self.properties(name, "shared")
        own = self.add_unit(name, "shared")
        whole, archives = self.static_closure(name)
        system = properties.get("system_shared_libs", ["libc", "libm", "libdl"])
        needed = unique(properties.get("shared_libs", []) + [library for library in system if library != name])
        for library in needed:
            if library not in GUEST_SYSTEM:
                self.add_linked(library)
        version_script = properties.get("version_script")
        if version_script:
            version_script = self.local(os.path.normpath(os.path.join(module.directory, version_script)))
        self.lines.append(
            f"art_{'program' if program else 'library'}({name}\n"
            f"  UNITS {' '.join(target for target in [own] + whole if target)}\n"
            f"  ARCHIVES {' '.join(archives)}\n"
            f"  NEEDED {cmake_list(needed)}\n"
            f"  STL {cmake_quote(properties.get('stl', 'libc++'))}\n"
            f"  VERSION_SCRIPT {cmake_quote(version_script or '')}\n"
            f"  LDFLAGS {cmake_flags(properties.get('ldflags', []))}\n"
            f"  NOCRT {'TRUE' if properties.get('nocrt') else 'FALSE'})")

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
        for library in LIBRARIES:
            self.add_linked(library)
        for program in PROGRAMS:
            self.add_linked(program, program=True)
        self.boot_jars(description)
        return self.lines


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    build = os.path.dirname(os.path.abspath(sys.argv[2]))
    planner = Planner(os.path.abspath(sys.argv[1]), os.path.join(build, "tree"))
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
