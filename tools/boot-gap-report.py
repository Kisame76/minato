#!/usr/bin/env python3
"""Lists the classes the boot class path's own code names but no boot jar defines.

On Android every class a boot jar's code names is on the boot class path. framework.jar is compiled against stub
classes for the parts of Android Minato does not build (build/framework/stub-list.txt), and a stub is never shipped:
code that reaches one resolves it at run time with no class loader, and ART's FindClass then throws its pre-allocated
NoClassDefFoundError, which has no name and no stack. A library that catches that error and throws it again later, on
another thread, ends the app far from the gap. This report names those classes up front, from the dex files alone.

Usage: tools/boot-gap-report.py [--from <jar>]... [--json] [<jar>...]
  <jar>        the boot class path's jars, default: the ones Minato builds (framework.jar and ART's javalib)
  --from <jar> only the classes this jar's code names (default: every jar given)
  --json       one JSON object: {"missing": {class: [jars that name it]}}
Exit status: 0 always; the report is information.
"""

import json
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_JARS = [
    "build/art/javalib/core-oj.jar",
    "build/art/javalib/core-libart.jar",
    "build/art/javalib/core-icu4j.jar",
    "build/art/javalib/okhttp.jar",
    "build/art/javalib/bouncycastle.jar",
    "build/art/javalib/conscrypt.jar",
    "build/framework/framework.jar",
]


def uleb128(data, offset):
    result = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        if byte < 0x80:
            return result, offset
        shift += 7


def dex_types(data):
    """The type descriptors a dex file names (type_ids) and the ones it defines (class_defs)."""
    if data[:4] != b"dex\n":
        raise ValueError("not a dex file")
    string_ids_size, string_ids_off, type_ids_size, type_ids_off = struct.unpack_from("<4I", data, 0x38)
    class_defs_size, class_defs_off = struct.unpack_from("<2I", data, 0x60)

    def string(index):
        (data_off,) = struct.unpack_from("<I", data, string_ids_off + 4 * index)
        _, start = uleb128(data, data_off)
        end = data.index(b"\0", start)
        return data[start:end].decode("utf-8", "replace")

    types = [string(struct.unpack_from("<I", data, type_ids_off + 4 * i)[0]) for i in range(type_ids_size)]
    defined = {types[struct.unpack_from("<I", data, class_defs_off + 32 * i)[0]] for i in range(class_defs_size)}
    return set(types), defined


def jar_types(path):
    named, defined = set(), set()
    with zipfile.ZipFile(path) as jar:
        for entry in jar.namelist():
            if entry.startswith("classes") and entry.endswith(".dex"):
                entry_named, entry_defined = dex_types(jar.read(entry))
                named |= entry_named
                defined |= entry_defined
    return named, defined


def dotted(descriptor):
    return descriptor[1:-1].replace("/", ".")


def report(jars, sources):
    defined = set()
    named_by = {}
    for jar in jars:
        named, jar_defined = jar_types(jar)
        defined |= jar_defined
        if jar in sources:
            for descriptor in named:
                element = descriptor.lstrip("[")
                if element.startswith("L"):
                    named_by.setdefault(element, []).append(Path(jar).name)
    return {dotted(d): sorted(set(by)) for d, by in sorted(named_by.items()) if d not in defined}


def main(argv):
    jars, sources, as_json = [], [], False
    args = iter(argv)
    for arg in args:
        if arg == "--from":
            sources.append(next(args))
        elif arg == "--json":
            as_json = True
        else:
            jars.append(arg)
    jars = jars or [str(ROOT / jar) for jar in DEFAULT_JARS]
    sources = [str(Path(source).resolve()) if not Path(source).is_absolute() else source for source in sources]
    jars = [str(Path(jar).resolve()) for jar in jars]
    for source in sources:
        if source not in jars:
            jars.append(source)
    missing = report(jars, set(sources or jars))
    if as_json:
        print(json.dumps({"missing": missing}, indent=1, sort_keys=True))
    else:
        for name, by in missing.items():
            print(f"{name}  ({', '.join(by)})")
        print(f"boot-gap-report: {len(missing)} classes named by the boot class path's code and defined by none of"
              f" its {len(jars)} jars", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
