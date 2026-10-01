#!/usr/bin/env python3
"""Feature flags of the framework's build (ADR-0039 decision 11, the lead's refinement), in place of AOSP's aconfig,
which is Rust: reads aconfig's flag declarations (*.aconfig below --declarations) and a release's values
(*_flag_values.textproto below --values, such as build/release's aconfig/ap3a) and writes

  feature-flags  what aapt2's --feature-flags @<file> reads: '<package>.<name>=<true|false>' per line, sorted, as
                 Soong's aconfig_text rule writes it (aconfig dump-cache --format='{fully_qualified_name}=
                 {state:bool}'). A flag without a value is DISABLED, aconfig's default; one that only a value names
                 (declared in a project we do not fetch, such as frameworks/av) comes with its value.

  java           aconfig's Java classes for every declared package, below --out in the package's folders: Flags
                 (FLAG_<NAME> constants and a static accessor per flag), the FeatureFlags interface,
                 FeatureFlagsImpl with the release's values as constants (DISABLED without one), and
                 CustomFeatureFlags, as aconfig's Java code generator names them; framework.jar compiles AOSP's
                 classes against them (ADR-0039 b3.1). Method names are aconfig's: the flag's snake_case name in
                 lowerCamelCase.

  cc             the header of aconfig's C and C++ accessors for the declared package --package, as
                 <package with dots as _>.h below --out, which the guest's Skia, hwui and minikin include
                 (ADR-0043 a): C++ `<package as namespace>::<flag>()`, constexpr, which hwui calls for its own flags,
                 and C `<package with dots as _>_<flag>(void)`, which minikin and hwui call for the text flags. Every
                 flag is a constant of the release's value, DISABLED without one, read-write flags too: the guest
                 has no flag storage. Flag names stay snake_case. Only declared flags get an accessor; a package
                 that only values name is refused. Not emitted: aconfig's provider_ object and its
                 COM_ANDROID_*-style macros, which no caller of ours uses (hwui, minikin and Skia at the tag).

--declarations may be given more than once, for declarations in several projects; with one folder the output is
the same as before. Both inputs are protobuf text format; this reads the part of it they use: fields with strings
(adjacent ones joined), names, numbers and booleans, messages in braces, and # comments.

Usage: tools/aconfig-flags.py feature-flags --declarations <folder> --values <folder> --out <file>
       tools/aconfig-flags.py java --declarations <folder> --values <folder> --out <folder>
       tools/aconfig-flags.py cc --declarations <folder> --values <folder> --package <package> --out <folder>
"""
import argparse
import os
import re
import sys
import tempfile

TOKEN = re.compile(r"""
    (?P<space>\s+) | (?P<comment>\#[^\n]*) | (?P<string>"(?:[^"\\]|\\.)*") | (?P<word>[A-Za-z0-9_.+-]+) |
    (?P<symbol>[{}:])
""", re.VERBOSE)


class FormatError(Exception):
    pass


def parse(text, path):
    """A text-format message as a dict of field name to a list of values; a nested message is a dict."""
    tokens, position = [], 0
    while position < len(text):
        match = TOKEN.match(text, position)
        if match is None:
            raise FormatError(f"{path}: unexpected {text[position:position + 20]!r}")
        position = match.end()
        kind = match.lastgroup
        if kind in ("space", "comment"):
            continue
        value = match.group(kind)
        if kind == "string":
            value = bytes(value[1:-1], "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
        tokens.append((kind, value))

    def message(index, closing):
        fields = {}
        while index < len(tokens):
            kind, value = tokens[index]
            if kind == "symbol" and value == "}":
                if not closing:
                    raise FormatError(f"{path}: '}}' without '{{'")
                return fields, index + 1
            if kind != "word":
                raise FormatError(f"{path}: a field name expected, not {value!r}")
            index += 1
            if index < len(tokens) and tokens[index] == ("symbol", ":"):
                index += 1
            if index >= len(tokens):
                raise FormatError(f"{path}: {value} has no value")
            kind_next, value_next = tokens[index]
            if kind_next == "symbol" and value_next == "{":
                nested, index = message(index + 1, True)
                fields.setdefault(value, []).append(nested)
            elif kind_next == "string":
                # Adjacent strings are one value, joined.
                index += 1
                while index < len(tokens) and tokens[index][0] == "string":
                    value_next += tokens[index][1]
                    index += 1
                fields.setdefault(value, []).append(value_next)
            elif kind_next == "word":
                fields.setdefault(value, []).append(value_next)
                index += 1
            else:
                raise FormatError(f"{path}: {value} has no value")
        if closing:
            raise FormatError(f"{path}: a '{{' is not closed")
        return fields, index

    return message(0, False)[0]


def single(fields, name, path, required=True):
    values = fields.get(name, [])
    if len(values) > 1:
        raise FormatError(f"{path}: {name} given {len(values)} times")
    if not values:
        if required:
            raise FormatError(f"{path}: no {name}")
        return None
    return values[0]


def files(folder, suffix):
    found = []
    for directory, subdirectories, names in os.walk(folder):
        subdirectories.sort()
        found += [os.path.join(directory, name) for name in sorted(names) if name.endswith(suffix)]
    return found


def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def declaration_files(folders):
    """The *.aconfig files below each folder, folder by folder in the order given."""
    return [path for folder in folders for path in files(folder, ".aconfig")]


def declared_flags(folders):
    """'<package>.<name>' of every flag declared below the folders."""
    names = set()
    for path in declaration_files(folders):
        fields = parse(read_text(path), path)
        package = single(fields, "package", path)
        for flag in fields.get("flag", []):
            names.add(f"{package}.{single(flag, 'name', path)}")
    return names


def declarations_by_package(folders):
    """{package: [flag names in declaration order]} of every flag declared below the folders."""
    packages = {}
    for path in declaration_files(folders):
        fields = parse(read_text(path), path)
        package = single(fields, "package", path)
        for flag in fields.get("flag", []):
            name = single(flag, "name", path)
            if name not in packages.setdefault(package, []):
                packages[package].append(name)
    return packages


def method_name(flag):
    """aconfig's Java method name: snake_case in lowerCamelCase."""
    words = flag.split("_")
    return words[0] + "".join(word[:1].upper() + word[1:] for word in words[1:])


def java_classes(package, names, states):
    """{file name: text} of aconfig's four Java classes for one package."""
    header = (f"// Generated by tools/aconfig-flags.py from aconfig's declarations and the release's values.\n"
              f"package {package};\n\n")
    flags = [(name, method_name(name), f"FLAG_{name.upper()}", states.get(f"{package}.{name}", False))
             for name in names]
    out = {}
    body = "".join(f'    /** @hide */\n    public static final String {constant} = "{package}.{name}";\n'
                   for name, _, constant, _ in flags)
    body += "".join(f"    public static boolean {method}() {{\n        return FEATURE_FLAGS.{method}();\n    }}\n"
                    for _, method, _, _ in flags)
    out["Flags.java"] = (header + "/** @hide */\npublic final class Flags {\n" + body
                         + "    private static FeatureFlags FEATURE_FLAGS = new FeatureFlagsImpl();\n}\n")
    out["FeatureFlags.java"] = (header + "/** @hide */\npublic interface FeatureFlags {\n"
                                + "".join(f"    boolean {method}();\n" for _, method, _, _ in flags) + "}\n")
    out["FeatureFlagsImpl.java"] = (
        header + "/** @hide */\npublic final class FeatureFlagsImpl implements FeatureFlags {\n"
        + "".join(f"    @Override\n    public boolean {method}() {{\n        return {'true' if value else 'false'};\n"
                  f"    }}\n" for _, method, _, value in flags) + "}\n")
    out["CustomFeatureFlags.java"] = (
        header + "import java.util.Arrays;\nimport java.util.List;\nimport java.util.function.BiPredicate;\n"
        "import java.util.function.Predicate;\n\n"
        "/** @hide */\npublic class CustomFeatureFlags implements FeatureFlags {\n"
        "    private final BiPredicate<String, Predicate<FeatureFlags>> mGetValueImpl;\n\n"
        "    public CustomFeatureFlags(BiPredicate<String, Predicate<FeatureFlags>> getValueImpl) {\n"
        "        mGetValueImpl = getValueImpl;\n    }\n"
        + "".join(f"    @Override\n    public boolean {method}() {{\n"
                  f"        return getValue(Flags.{constant}, FeatureFlags::{method});\n    }}\n"
                  for _, method, constant, _ in flags)
        + "    protected boolean getValue(String flagName, Predicate<FeatureFlags> getter) {\n"
        "        return mGetValueImpl.test(flagName, getter);\n    }\n"
        "    public List<String> getFlagNames() {\n        return Arrays.asList(\n"
        + ",\n".join(f"            Flags.{constant}" for _, _, constant, _ in flags) + ");\n    }\n}\n")
    return out


def cc_header(package, names, states):
    """The text of aconfig's C and C++ accessors for one package, each returning the release's value."""
    prefix = package.replace(".", "_")
    namespace = package.replace(".", "::")
    flags = [(name, "true" if states.get(f"{package}.{name}", False) else "false") for name in names]
    text = ("// Generated by tools/aconfig-flags.py from aconfig's declarations and the release's values.\n"
            "#pragma once\n\n#ifdef __cplusplus\n"
            f"namespace {namespace} {{\n\n")
    text += "".join(f"constexpr inline bool {name}() {{\n    return {value};\n}}\n\n" for name, value in flags)
    text += f"}}  // namespace {namespace}\n#else\n#include <stdbool.h>\n#endif\n\n"
    text += "".join(f"static inline bool {prefix}_{name}(void) {{\n    return {value};\n}}\n\n" for name, value in flags)
    return text.rstrip("\n") + "\n"


def flag_values(folder):
    """'<package>.<name>' to True (ENABLED) or False (DISABLED) for every value below folder."""
    states = {}
    for path in files(folder, "_flag_values.textproto"):
        for value in parse(read_text(path), path).get("flag_value", []):
            name = f"{single(value, 'package', path)}.{single(value, 'name', path)}"
            state = single(value, "state", path)
            if state not in ("ENABLED", "DISABLED"):
                raise FormatError(f"{path}: {name} has the state {state!r}")
            states[name] = state == "ENABLED"
    return states


def write_atomically(path, text):
    folder = os.path.dirname(os.path.abspath(path))
    os.makedirs(folder, exist_ok=True)
    handle, temporary = tempfile.mkstemp(dir=folder, prefix=".aconfig-flags-")
    with os.fdopen(handle, "w") as output:
        output.write(text)
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser(description="Feature flags of the framework's build (ADR-0039 decision 11).")
    commands = parser.add_subparsers(dest="command", required=True)
    feature_flags = commands.add_parser("feature-flags", help="aapt2's --feature-flags file")
    feature_flags.add_argument("--declarations", required=True, action="append")
    feature_flags.add_argument("--values", required=True)
    feature_flags.add_argument("--out", required=True)
    java = commands.add_parser("java", help="aconfig's Java classes per declared package")
    java.add_argument("--declarations", required=True, action="append")
    java.add_argument("--values", required=True)
    java.add_argument("--out", required=True)
    cc = commands.add_parser("cc", help="aconfig's C and C++ accessors of one declared package")
    cc.add_argument("--declarations", required=True, action="append")
    cc.add_argument("--values", required=True)
    cc.add_argument("--package", required=True)
    cc.add_argument("--out", required=True)
    arguments = parser.parse_args()

    for folder in arguments.declarations + [arguments.values]:
        if not os.path.isdir(folder):
            print(f"aconfig-flags: no folder {folder}", file=sys.stderr)
            return 1
    if arguments.command == "cc":
        try:
            states = flag_values(arguments.values)
            packages = declarations_by_package(arguments.declarations)
        except (FormatError, UnicodeDecodeError) as error:
            print(f"aconfig-flags: {error}", file=sys.stderr)
            return 1
        if arguments.package not in packages:
            print(f"aconfig-flags: no declarations of the package {arguments.package}", file=sys.stderr)
            return 1
        names = packages[arguments.package]
        header = os.path.join(arguments.out, arguments.package.replace(".", "_") + ".h")
        write_atomically(header, cc_header(arguments.package, names, states))
        print(f"aconfig-flags: {header}: {len(names)} flags, "
              f"{sum(states.get(arguments.package + '.' + name, False) for name in names)} enabled")
        return 0
    if arguments.command == "java":
        try:
            states = flag_values(arguments.values)
            packages = declarations_by_package(arguments.declarations)
        except (FormatError, UnicodeDecodeError) as error:
            print(f"aconfig-flags: {error}", file=sys.stderr)
            return 1
        for package, names in sorted(packages.items()):
            for name, text in java_classes(package, names, states).items():
                write_atomically(os.path.join(arguments.out, *package.split("."), name), text)
        print(f"aconfig-flags: {arguments.out}: {len(packages)} packages, "
              f"{sum(len(n) for n in packages.values())} flags")
        return 0
    try:
        states = flag_values(arguments.values)
        names = declared_flags(arguments.declarations) | set(states)
    except (FormatError, UnicodeDecodeError) as error:
        print(f"aconfig-flags: {error}", file=sys.stderr)
        return 1
    lines = [f"{name}={'true' if states.get(name, False) else 'false'}\n" for name in sorted(names)]
    write_atomically(arguments.out, "".join(lines))
    print(f"aconfig-flags: {arguments.out}: {len(lines)} flags, {sum(states.get(n, False) for n in names)} enabled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
