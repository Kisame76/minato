#!/usr/bin/env python3
"""Feature flags of the framework's build (ADR-0039 decision 11, the lead's refinement), in place of AOSP's aconfig,
which is Rust: reads aconfig's flag declarations (*.aconfig below --declarations) and a release's values
(*_flag_values.textproto below --values, such as build/release's aconfig/ap3a) and writes

  feature-flags  what aapt2's --feature-flags @<file> reads: '<package>.<name>=<true|false>' per line, sorted, as
                 Soong's aconfig_text rule writes it (aconfig dump-cache --format='{fully_qualified_name}=
                 {state:bool}'). A flag without a value is DISABLED, aconfig's default; one that only a value names
                 (declared in a project we do not fetch, such as frameworks/av) comes with its value.

Both inputs are protobuf text format; this reads the part of it they use: fields with strings (adjacent ones
joined), names, numbers and booleans, messages in braces, and # comments.

Usage: tools/aconfig-flags.py feature-flags --declarations <folder> --values <folder> --out <file>
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


def declared_flags(folder):
    """'<package>.<name>' of every flag declared below folder."""
    names = set()
    for path in files(folder, ".aconfig"):
        fields = parse(read_text(path), path)
        package = single(fields, "package", path)
        for flag in fields.get("flag", []):
            names.add(f"{package}.{single(flag, 'name', path)}")
    return names


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
    feature_flags.add_argument("--declarations", required=True)
    feature_flags.add_argument("--values", required=True)
    feature_flags.add_argument("--out", required=True)
    arguments = parser.parse_args()

    for folder in (arguments.declarations, arguments.values):
        if not os.path.isdir(folder):
            print(f"aconfig-flags: no folder {folder}", file=sys.stderr)
            return 1
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
