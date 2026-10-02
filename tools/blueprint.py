#!/usr/bin/env python3
"""Reads Android.bp files (Blueprint) and resolves cc modules the way Soong does for one target,
as far as building bionic needs it (ADR-0021): defaults, arch and target variants, the shared and
static parts, filegroups, whole_static_libs and exported include directories.

Only the syntax bionic's build files use: modules with properties, variables with = and +=,
strings, integers, booleans, lists, maps, '+' on lists and strings, // and /* */ comments.
"""
import fnmatch
import glob
import os
import re

TOKEN = re.compile(r"""
    (?P<space>\s+) | (?P<line_comment>//[^\n]*) | (?P<block_comment>/\*.*?\*/) |
    (?P<string>"(?:[^"\\]|\\.)*"|`[^`]*`) | (?P<integer>-?\d+) |
    (?P<name>[A-Za-z_][A-Za-z0-9_.]*) | (?P<symbol>\+=|[{}\[\]():,=+])
""", re.VERBOSE | re.DOTALL)


class ParseError(Exception):
    pass


def tokenize(text, path):
    tokens, position = [], 0
    while position < len(text):
        match = TOKEN.match(text, position)
        if match is None:
            raise ParseError(f"{path}: unexpected {text[position:position + 20]!r}")
        position = match.end()
        kind = match.lastgroup
        if kind in ("space", "line_comment", "block_comment"):
            continue
        value = match.group(kind)
        if kind == "string":
            value = bytes(value[1:-1], "utf-8").decode("unicode_escape") if value[0] == '"' else value[1:-1]
        elif kind == "integer":
            value = int(value)
        tokens.append((kind, value))
    tokens.append(("end", None))
    return tokens


class Parser:
    def __init__(self, text, path, variables):
        self.tokens = tokenize(text, path)
        self.index = 0
        self.path = path
        self.variables = variables

    def peek(self):
        return self.tokens[self.index]

    def take(self, kind=None, value=None):
        token = self.tokens[self.index]
        if (kind and token[0] != kind) or (value is not None and token[1] != value):
            raise ParseError(f"{self.path}: expected {value or kind}, got {token[1]!r}")
        self.index += 1
        return token[1]

    def file(self):
        modules = []
        while self.peek()[0] != "end":
            name = self.take("name")
            if self.peek() in (("symbol", "="), ("symbol", "+=")):
                operator = self.take("symbol")
                value = self.expression()
                if operator == "+=":
                    value = self.variables[name] + value
                self.variables[name] = value
            else:
                closing = "}" if self.peek() == ("symbol", "{") else ")"
                self.take("symbol")
                modules.append((name, self.properties(closing)))
        return modules

    def properties(self, closing):
        result = {}
        while self.peek() != ("symbol", closing):
            key = self.take("name")
            self.take("symbol", ":" if closing == "}" else "=")
            result[key] = self.expression()
            if self.peek() == ("symbol", ","):
                self.take("symbol")
        self.take("symbol", closing)
        return result

    def expression(self):
        value = self.term()
        while self.peek() == ("symbol", "+"):
            self.take("symbol")
            value = value + self.term()
        return value

    def term(self):
        kind, value = self.peek()
        if kind in ("string", "integer"):
            self.index += 1
            return value
        if kind == "name":
            self.index += 1
            if value in ("true", "false"):
                return value == "true"
            if value not in self.variables:
                raise ParseError(f"{self.path}: unknown variable {value}")
            return self.variables[value]
        if value == "[":
            self.take("symbol")
            items = []
            while self.peek() != ("symbol", "]"):
                items.append(self.expression())
                if self.peek() == ("symbol", ","):
                    self.take("symbol")
            self.take("symbol", "]")
            return items
        if value == "{":
            self.take("symbol")
            return self.properties("}")
        raise ParseError(f"{self.path}: unexpected {value!r}")


def merge(base, extra):
    """Soong's property merge: lists append, maps merge, everything else is replaced."""
    result = dict(base)
    for key, value in extra.items():
        if key in result and isinstance(result[key], list) and isinstance(value, list):
            result[key] = result[key] + value
        elif key in result and isinstance(result[key], dict) and isinstance(value, dict):
            result[key] = merge(result[key], value)
        else:
            result[key] = value
    return result


class Module:
    def __init__(self, kind, properties, directory):
        self.kind = kind
        self.properties = properties
        self.directory = directory  # relative to the source tree root, e.g. "bionic/libc"
        self.name = properties.get("name")


class Tree:
    """Android.bp files of a partial source tree; `roots` maps tree paths to local folders. `target_groups` are the
    target variants its modules are resolved for: TARGET_GROUPS's, Android's, unless given (a host flavor's for
    ADR-0043 a)."""

    def __init__(self, roots, target_groups=None):
        self.roots = roots
        self.modules = {}
        self.target_groups = list(TARGET_GROUPS if target_groups is None else target_groups)

    def local(self, tree_path):
        for prefix, folder in sorted(self.roots.items(), key=lambda item: -len(item[0])):
            if tree_path == prefix or tree_path.startswith(prefix + "/"):
                return os.path.join(folder, tree_path[len(prefix):].lstrip("/"))
        return None

    def load(self, tree_path):
        path = self.local(tree_path)
        with open(path) as handle:
            text = handle.read()
        directory = os.path.dirname(tree_path)
        for kind, properties in Parser(text, path, {}).file():
            # NDK stub libraries share the name of the real one; Soong keeps them apart, we skip them.
            if "name" in properties and kind not in ("ndk_library", "ndk_headers", "versioned_ndk_headers"):
                self.modules[properties["name"]] = Module(kind, properties, directory)

    def module(self, name):
        if name not in self.modules:
            raise KeyError(f"unknown module {name}")
        return self.modules[name]


# The variant bionic is built for: Android on arm64, as a shared library.
ARCH, OS = "arm64", "android"
TARGET_GROUPS = ["android", "linux", "bionic", "not_windows", "android_arm64", "linux_arm64", "bionic_arm64"]

# Defaults defined by Soong itself or by projects we do not have; they carry nothing bionic needs.
MISSING_DEFAULTS = set()


def flatten(tree, module, linkage):
    """The module's properties with its defaults and every variant for ARCH, the tree's target groups and `linkage`
    ("shared" or "static") merged in, as Soong would see them."""
    properties = {}
    for default in module.properties.get("defaults", []):
        if default not in tree.modules:
            MISSING_DEFAULTS.add(default)
            continue
        properties = merge(properties, flatten(tree, tree.module(default), linkage))
    own = {key: value for key, value in module.properties.items() if key != "defaults"}
    properties = merge(properties, own)
    for group in ("arch", "target", "multilib"):
        variants = properties.pop(group, {})
        keys = {"arch": [ARCH], "target": tree.target_groups, "multilib": ["lib64"]}[group]
        for key in keys:
            if key in variants:
                properties = merge(properties, variants[key])
    for part in ("shared", "static"):
        variant = properties.pop(part, {})
        if part == linkage:
            properties = merge(properties, variant)
    return properties


def expand_sources(tree, module, sources, excluded=()):
    """Tree paths of `sources` (relative to the module, with globs and :filegroup references)."""
    result = []
    for source in sources:
        if source.startswith(":"):
            reference = tree.module(source[1:].split("{")[0])
            if reference.kind == "filegroup":
                inner = reference.properties.get("srcs", [])
                result += expand_sources(tree, reference, inner, reference.properties.get("exclude_srcs", []))
            else:
                result.append(source)  # a generated source; the build generates it
            continue
        pattern = os.path.normpath(os.path.join(module.directory, source))
        if any(character in pattern for character in "*?["):
            local = tree.local(pattern)
            base = tree.local(module.directory)
            matches = sorted(glob.glob(local, recursive=True))
            result += [os.path.normpath(os.path.join(module.directory, os.path.relpath(match, base))) for match in matches]
        else:
            result.append(pattern)
    excluded_patterns = [os.path.normpath(os.path.join(module.directory, pattern)) for pattern in excluded]
    return [source for source in result if not any(fnmatch.fnmatch(source, pattern) for pattern in excluded_patterns)]


def load_all(tree, local_root_by_tree_path):
    """Loads every Android.bp below the given tree paths."""
    for tree_path, local in local_root_by_tree_path.items():
        for path in glob.glob(os.path.join(local, "**", "Android.bp"), recursive=True):
            tree.load(os.path.normpath(os.path.join(tree_path, os.path.relpath(path, local))))


def exported_includes(tree, name, missing, seen=None):
    """Include directories a module exports to the modules that depend on it (tree paths)."""
    seen = set() if seen is None else seen
    if name in seen:
        return []
    seen.add(name)
    if name not in tree.modules:
        missing.add(name)
        return []
    module = tree.module(name)
    properties = flatten(tree, module, "static")
    result = [os.path.normpath(os.path.join(module.directory, path))
              for key in ("export_include_dirs", "export_system_include_dirs") for path in properties.get(key, [])]
    # What a module exports includes what its whole static libraries export: they are part of it.
    for key in ("export_header_lib_headers", "export_static_lib_headers", "export_shared_lib_headers",
                "whole_static_libs"):
        for dependency in properties.get(key, []):
            result += exported_includes(tree, dependency, missing, seen)
    return result


def system_shared_libs(properties, name):
    """The libc, libm and libdl a module links against: its own list, or Soong's default."""
    libraries = properties.get("system_shared_libs", ["libc", "libm", "libdl"])
    return [library for library in libraries if library != name]


def compile_unit(tree, name, linkage, missing):
    """What one module compiles: sources, flags and include directories, all as tree paths."""
    module = tree.module(name)
    properties = flatten(tree, module, linkage)
    # Soong puts the module's own directory first, unless include_build_directory is false.
    includes = [module.directory] if properties.get("include_build_directory", True) else []
    includes += [os.path.normpath(os.path.join(module.directory, path)) for path in properties.get("local_include_dirs", [])]
    includes += [os.path.normpath(path) for path in properties.get("include_dirs", [])]
    includes += [os.path.normpath(os.path.join(module.directory, path))
                 for key in ("export_include_dirs", "export_system_include_dirs") for path in properties.get(key, [])]
    for key in ("header_libs", "static_libs", "whole_static_libs", "shared_libs"):
        for dependency in properties.get(key, []):
            includes += exported_includes(tree, dependency, missing)
    for dependency in system_shared_libs(properties, name):
        includes += exported_includes(tree, dependency, missing)
    unique = []
    for include in includes:
        if include not in unique:
            unique.append(include)
    def flags(key):
        return list(properties.get(key, []))

    return {
        "name": name,
        "directory": module.directory,
        "sources": expand_sources(tree, module, properties.get("srcs", []), properties.get("exclude_srcs", [])),
        "cflags": flags("cflags"),
        "conlyflags": flags("conlyflags"),
        "cppflags": flags("cppflags"),
        "asflags": flags("asflags"),
        "includes": unique,
        "generated_headers": properties.get("generated_headers", []),
        "whole_static_libs": properties.get("whole_static_libs", []),
        "static_libs": properties.get("static_libs", []),
        "shared_libs": properties.get("shared_libs", []) + system_shared_libs(properties, name),
        "version_script": properties.get("version_script"),
        "ldflags": properties.get("ldflags", []),
    }


def library_plan(tree, name):
    """The compile units of a shared library: its own, then those of its whole_static_libs,
    recursively, each once. Returns (units, missing module names)."""
    missing, units, seen = set(), [], set()

    def visit(unit_name, linkage):
        if unit_name in seen:
            return
        seen.add(unit_name)
        if unit_name not in tree.modules:
            missing.add(unit_name)
            return
        unit = compile_unit(tree, unit_name, linkage, missing)
        units.append(unit)
        for dependency in unit["whole_static_libs"]:
            visit(dependency, "static")

    visit(name, "shared")
    return units, missing
