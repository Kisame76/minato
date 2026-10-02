#!/usr/bin/env python3
"""Turns the Android.bp files of a set of AOSP projects into a CMake plan for guest code, as Soong would build them
for one target: one object library per module and linkage, with the module's own flags and include directories, custom
commands for the generated sources, and the link settings of every shared library and program the set builds. The
planner of a set subclasses Planner with the set's names, as tools/art-plan.py does for ART.

The build reads its sources from <tree> next to the plan: AOSP's tree layout of symbolic links to the fetched files,
with the overlay's files in place of AOSP's at the same path, and the patches applied, in order, to real copies. So a
patched header is the one every include finds, also a quoted one next to its includer, and includes that climb out of
their project ("../../external/fdlibm/fdlibm.h") find the other as in AOSP's tree. In a set's host-side folders,
the copies also take the host side of every preprocessor test of __ANDROID__, which the NDK's compiler defines.
"""
import glob
import os
import re
import subprocess
import sys
import tempfile
from types import MappingProxyType

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blueprint  # noqa: E402

SOURCE_EXTENSIONS = (".c", ".cpp", ".cc", ".S", ".s")
GENERATORS = ("genrule", "gensrcs", "cc_genrule")
# What Soong compiles for cpp_std and c_std "experimental" (cc/config/global.go at the tag).
EXPERIMENTAL_STD = {"cpp_std": "gnu++2b", "c_std": "gnu2x"}
# The host side's tool, unifdef 2.12 of macOS's base system, the files it reads, and a test it left: one it cannot
# decide, such as defined(__ANDROID__) || defined(X), or a macro defined from __ANDROID__.
UNIFDEF = "/usr/bin/unifdef"
C_FILES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")
ANDROID_TEST = re.compile(rb"^[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif|define)\b.*__ANDROID__.*$", re.M)
# A C++ raw string literal, which unifdef 2.12 does not know: it would read its quotes and its lines.
RAW_STRING = re.compile(rb'R"([^()\\\s]{0,16})\((.*?)\)\1"', re.S)


def cmake_quote(value):
    """A quoted CMake argument. `$` stays: the generated paths use the plan's variable of the generated files."""
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


def take_host_side(data):
    """A C or C++ file's `data` with the host side of its preprocessor tests of __ANDROID__, as unifdef -U__ANDROID__
    resolves them, and a problem or None. unifdef reads each raw string literal as a placeholder, which gets the
    literal back after. It takes a comment at the end of a file without a newline for an unterminated one, so it reads
    the file with the newline, which the last line loses again."""
    raw_strings = []

    def placeholder(match):
        raw_strings.append(match.group(0))
        return b"MINATO_RAW_STRING_%d_" % (len(raw_strings) - 1)

    masked = RAW_STRING.sub(placeholder, data)
    ended = data.endswith(b"\n")
    result = subprocess.run([UNIFDEF, "-U__ANDROID__"], input=masked if ended else masked + b"\n",
                            capture_output=True)
    if result.returncode not in (0, 1):
        return data, " ".join(result.stderr.decode(errors="replace").split())
    output = result.stdout
    for index, raw_string in enumerate(raw_strings):
        output = output.replace(b"MINATO_RAW_STRING_%d_" % index, raw_string, 1)
    if not ended and output.endswith(data.rsplit(b"\n", 1)[-1] + b"\n"):
        output = output[:-1]
    left = ANDROID_TEST.search(output)
    if left:
        return data, "unifdef leaves " + left.group(0).decode(errors="replace").strip()
    return output, None


def make_tree(tree, projects, overlay, patches, host_side=()):
    """Brings <tree> up to date: a symbolic link for every file of `projects` (tree path -> folder) and of `overlay`,
    real files for what the patches in `patches` change, in their order, and for what the host side changes in the C
    and C++ files below the folders `host_side` (tree paths), after the patches. Files that already are what they should
    be stay untouched, so that their builds stay current. Returns the problems."""
    wanted = {}
    for tree_path, folder in projects.items():
        for directory, folders, files in os.walk(folder):
            links = [name for name in folders if os.path.islink(os.path.join(directory, name))]
            for name in files + links:
                path = os.path.join(directory, name)
                wanted[os.path.normpath(os.path.join(tree_path, os.path.relpath(path, folder)))] = path
    for directory, _, files in os.walk(overlay):
        for name in files:
            path = os.path.join(directory, name)
            wanted[os.path.relpath(path, overlay)] = path
    patched, problems = {}, []
    for patch in sorted(glob.glob(os.path.join(patches, "*.patch"))):
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
    for tree_path, target in wanted.items():
        if not tree_path.endswith(C_FILES) or not any(tree_path.startswith(folder + "/")
                                                      for folder in host_side):
            continue
        data = patched[tree_path] if tree_path in patched else read(target)
        if b"__ANDROID__" not in data:
            continue
        output, problem = take_host_side(data)
        if problem:
            problems.append(f"{tree_path}: no host side: {problem}")
        elif output != data:
            patched[tree_path] = output
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
    """The plan of one set of projects. A subclass names the set and the plan's parts:
    - TREE, the projects' tree paths and their folders below the sources; OURS, the tree path of Minato's folder
      OURS_FOLDER, whose modules.bp holds our modules in place of AOSP's of the same name; OVERLAY's files in place of
      AOSP's, and PATCHES' patches applied to real copies;
    - TARGET_GROUPS, the target variants the modules are resolved for (None: blueprint's, Android's);
    - GUEST_SYSTEM, the guest system's libraries, which the plan links but does not build; DROPPED, dependencies left
      out; LIBRARIES and PROGRAMS, what it builds;
    - PREFIX, the prefix of the plan's CMake functions and targets (<prefix>_unit, <prefix>_library, ...), GENERATED,
      the CMake variable of the generated files' folder, and TREE_VARIABLE, the one of the tree;
    - GLOBAL_INCLUDES, Soong's global includes (cc/config/global.go's commonGlobalIncludes), which Soong gives
      platform and host modules as -I after all of their own and their dependencies' (cc/compiler.go,
      cc/builder.go), those the tree holds;
    - HOST_SIDE, the tree's folders of a host flavor, whose files take the host side of __ANDROID__ (make_tree).
    adapt_kind adds what Soong's load hooks add for the set's own module types."""
    TREE = MappingProxyType({})
    OURS = None
    OURS_FOLDER = None
    OVERLAY = None
    PATCHES = None
    TARGET_GROUPS = None
    GUEST_SYSTEM = ()
    DROPPED = frozenset()
    LIBRARIES = ()
    PROGRAMS = ()
    PREFIX = None
    GENERATED = None
    TREE_VARIABLE = None
    GLOBAL_INCLUDES = ()
    HOST_SIDE = ()
    NAMED = ("OURS", "OURS_FOLDER", "OVERLAY", "PATCHES", "PREFIX", "GENERATED", "TREE_VARIABLE")

    def __init__(self, sources, tree):
        missing = [name for name in self.NAMED if getattr(self, name) is None]
        if missing:
            raise TypeError(f"{type(self).__name__} does not name {', '.join(missing)}")
        self.tree_root = tree
        projects = {tree_path: os.path.join(sources, folder) for tree_path, folder in self.TREE.items()}
        projects[self.OURS] = self.OURS_FOLDER
        self.problems = make_tree(tree, projects, self.OVERLAY, self.PATCHES, self.HOST_SIDE)
        self.tree = blueprint.Tree({tree_path: os.path.join(tree, tree_path) for tree_path in projects},
                                   self.TARGET_GROUPS)
        for tree_path in self.TREE:
            for path in sorted(glob.glob(os.path.join(tree, tree_path, "**", "Android.bp"), recursive=True)):
                self.load(os.path.relpath(path, tree))
        self.load(f"{self.OURS}/modules.bp")  # last: Minato's modules take the place of those of the same name
        for module in self.tree.modules.values():
            self.adapt(module)
        self.system_includes = unique(include for name in ("libc", "libm", "libdl")
                                      for include in blueprint.exported_includes(self.tree, name, set()))
        self.lines = [f"set({self.TREE_VARIABLE} {cmake_quote(tree)})"]
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
        """What Soong's load hooks and configuration add: the default branch of soong_config_variables, nothing of
        product_variables (a user build), and adapt_kind's."""
        properties = module.properties
        properties.pop("product_variables", None)
        for settings in properties.pop("soong_config_variables", {}).values():
            module.properties = properties = blueprint.merge(properties, settings.get("conditions_default", {}))
        self.adapt_kind(module)

    def adapt_kind(self, module):
        """What the load hooks of the set's own module types add; nothing here."""

    def generated_folder(self):
        """The generated files' folder as the plan names it."""
        return "${" + self.GENERATED + "}"

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
                                     and value not in self.DROPPED)
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
        output = f"{self.generated_folder()}/host/{name}"
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
        self.lines.append(f"{self.PREFIX}_host_tool({cmake_quote(output)}\n  SOURCES {cmake_list(sources)}\n"
                          f"  INCLUDES {cmake_list(includes)}\n  DEPENDS {cmake_list(headers)})")
        self.generated[name] = output
        return output

    def object_text(self, module):
        """A cc_object compiled with -S (asm_defines.s): its assembly text."""
        name = module.properties["name"]
        output = f"{self.generated_folder()}/{name}/{name}"
        unit = blueprint.compile_unit(self.tree, name, "static", set())
        sources = [self.local(source) for source in unit["sources"]]
        includes, system = self.includes(unit)
        self.lines.append(
            f"{self.PREFIX}_object_text({cmake_quote(output)}\n  SOURCES {cmake_list(sources)}\n"
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
        directory = f"{self.generated_folder()}/{name}"
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
                self.lines.append(f"{self.PREFIX}_generate({cmake_quote(output)}\n  COMMAND {cmake_quote(single)}\n"
                                  f"  DEPENDS {cmake_list([path] + tool_paths)})")
                outputs.append(output)
        else:
            outputs = [f"{directory}/{out}" for out in properties["out"]]
            single = command.replace("$(in)", " ".join(inputs)).replace("$(out)", " ".join(outputs))
            self.lines.append(f"{self.PREFIX}_generate({cmake_list(outputs)}\n  COMMAND {cmake_quote(single)}\n"
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
        includes += [os.path.abspath(os.path.join(self.tree_root, include)) for include in self.GLOBAL_INCLUDES
                     if os.path.isdir(os.path.join(self.tree_root, include))]
        cflags = list(unit["cflags"])
        cppflags = list(unit["cppflags"]) + (["-frtti"] if properties.get("rtti") else [])
        standards = {key: EXPERIMENTAL_STD[key] if properties.get(key) == "experimental" else properties.get(key)
                     for key in EXPERIMENTAL_STD}
        if standards["cpp_std"]:
            cppflags.append(f"-std={standards['cpp_std']}")
        if standards["c_std"]:
            unit["conlyflags"] = [f"-std={standards['c_std']}"] + list(unit["conlyflags"])
        target = f"{self.PREFIX}_{identifier(name)}_{linkage}"
        self.units[key] = target if sources else None
        if not sources:
            self.lines.append(f"# {name} ({linkage}): nothing to compile")
            return None
        self.lines.append(
            f"{self.PREFIX}_unit({target}\n"
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
        archives, those of its static_libs, recursively (Soong passes them all to the final link); and the guest
        system's libraries among them, which the guest has as shared libraries only. One it would take whole is a
        problem: Soong would embed it and export its symbols from the shared library, which NEEDED does not."""
        whole, archives, shared, seen = [], [], [], set()

        def visit(dependency, whole_archive):
            if dependency in self.GUEST_SYSTEM and whole_archive:
                self.problems.append(f"{name}: takes the guest system's {dependency} whole, which the guest has as a "
                                     "shared library only")
                return
            if dependency in seen or dependency in self.DROPPED:
                return
            seen.add(dependency)
            if dependency in self.GUEST_SYSTEM:
                shared.append(dependency)
                return
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
        return whole, archives, shared

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
        whole, archives, shared = self.static_closure(name)
        system = properties.get("system_shared_libs", ["libc", "libm", "libdl"])
        needed = unique(properties.get("shared_libs", []) + shared + [library for library in system if library != name])
        for library in needed:
            if library not in self.GUEST_SYSTEM:
                self.add_linked(library)
        version_script = properties.get("version_script")
        if version_script:
            version_script = self.local(os.path.normpath(os.path.join(module.directory, version_script)))
        self.lines.append(
            f"{self.PREFIX}_{'program' if program else 'library'}({name}\n"
            f"  UNITS {' '.join(target for target in [own] + whole if target)}\n"
            f"  ARCHIVES {' '.join(archives)}\n"
            f"  NEEDED {cmake_list(needed)}\n"
            f"  STL {cmake_quote(properties.get('stl', 'libc++'))}\n"
            f"  VERSION_SCRIPT {cmake_quote(version_script or '')}\n"
            f"  LDFLAGS {cmake_flags(properties.get('ldflags', []))}\n"
            f"  NOCRT {'TRUE' if properties.get('nocrt') else 'FALSE'})")

    def plan_linked(self):
        """The plan's lines: LIBRARIES and PROGRAMS with what they need."""
        for library in self.LIBRARIES:
            self.add_linked(library)
        for program in self.PROGRAMS:
            self.add_linked(program, program=True)
        return self.lines
