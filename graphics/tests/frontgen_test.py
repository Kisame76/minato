#!/usr/bin/env python3
"""Test 1 of ADR-0030: the generator in tools/frontgen/.

- Over the NDK's names, its sorting equals the one taken from the NDK's C headers (ndk_headers.py, a
  reading of its own): per name, and in the counts that ADR-0029 and ADR-0030 name.
- Every alias that the XML names for a function written by hand gets that function; every exported name
  is in the table and in the export list; the five structures with function pointers are handled.
- The table's hash is stable: two runs under other hash seeds write the same files.
- Small XML samples with a variadic function, a structure by value, a long double, a char by value or a
  callback with a small result are refused.

Prints "skipped:" while vk.xml (tools/fetch-moltenvk.sh), ANGLE's registries (tools/build-angle.sh) or
the NDK is missing, so CTest stays green for everyone else."""
import argparse
import collections
import filecmp
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import textwrap
import xml.etree.ElementTree as ElementTree

import ndk_headers

# The counts of ADR-0029, decision 14 (with ADR-0030's change) and of ADR-0030, test 1 and decision 10.
ABI_COUNTS = {
    "gles": {"same": 420, "extend": 33, "repack": 8},
    "egl": {"same": 73},
    "vulkan": {"same": 233},
}
KIND_COUNTS = {
    "gles": {"plain": 413, "extend": 33, "repack": 8, "by hand": 7},
    "egl": {"plain": 58, "by hand": 15},
    "vulkan": {"plain": 149, "without allocator": 46, "by hand": 38},
}
VULKAN_HEADER_COUNTS = {"same": 706, "extend": 3, "repack": 1}
FUNCTION_POINTER_STRUCTURES = {
    "VkAllocationCallbacks", "VkDebugReportCallbackCreateInfoEXT", "VkDebugUtilsMessengerCreateInfoEXT",
    "VkDeviceDeviceMemoryReportCreateInfoEXT", "VkDirectDriverLoadingInfoLUNARG",
}
LIBS = ["vulkan", "gles", "egl"]

failures = []


def check(condition, what):
    print(("ok: " if condition else "FAIL: ") + what)
    if not condition:
        failures.append(what)


def run_generator(options, out, seed):
    environment = dict(os.environ, PYTHONHASHSEED=str(seed))
    command = [sys.executable, os.path.join(options.repo, "tools", "frontgen", "frontgen.py"), "--out", out,
               "--vk-xml", options.vk_xml, "--angle", options.angle, "--ndk", options.ndk]
    result = subprocess.run(command, env=environment, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr)
    return result.returncode == 0


def same_tree(first, second):
    names = sorted(os.listdir(first))
    if names != sorted(os.listdir(second)):
        return False
    _, mismatch, errors = filecmp.cmpfiles(first, second, names, shallow=False)
    return not mismatch and not errors


def xml_aliases(root, dialect):
    """Groups of names that the XML makes aliases of each other."""
    parent = {}

    def find(name):
        while parent.setdefault(name, name) != name:
            name = parent[name]
        return name

    for command in root.find("commands"):
        if dialect == "vulkan":
            name, target = command.get("name"), command.get("alias")
        else:
            name, alias = command.findtext("proto/name"), command.find("alias")
            target = alias.get("name") if alias is not None else None
        if name and target:
            parent[find(name)] = find(target)
    groups = collections.defaultdict(set)
    for name in list(parent):
        groups[find(name)].add(name)
    return {name: group for group in groups.values() for name in group}


def load_registry(path, extension_path=None):
    root = ElementTree.parse(path).getroot()
    if extension_path:
        extra = ElementTree.parse(extension_path).getroot()
        for section in ("types", "commands", "extensions"):
            target = root.find(section)
            for part in extra.iter(section):
                target.extend(list(part))
    return root


def structures_with_function_pointers(root):
    """Vulkan's structures with a function pointer member, Vulkan SC's own left out."""
    callbacks = {t.findtext("proto/name") for t in root.iter("type") if t.get("category") == "funcpointer"}
    required = collections.defaultdict(set)
    for parent in list(root.iter("feature")) + list(root.iter("extension")):
        apis = set((parent.get("api") or parent.get("supported") or "").split(","))
        for part in parent.iter("require"):
            for kind in part.findall("type"):
                required[kind.get("name")] |= apis
    found = set()
    for kind in root.iter("type"):
        if kind.get("category") in ("struct", "union"):
            if any(member.findtext("type") in callbacks for member in kind.findall("member")):
                if "vulkan" in required[kind.get("name")]:
                    found.add(kind.get("name"))
    return found


def signature(prototype):
    """A prototype in the report's short form (i4s a signed 32-bit integer, f8 a double, p a pointer, cb a
    callback, v void), from the headers' reading."""
    def code(value):
        if value.kind == "int":
            return f"i{value.size}{'s' if value.signed else 'u'}"
        if value.kind == "float":
            return f"f{value.size}"
        return {"pointer": "p", "callback": "cb", "void": "v"}.get(value.kind, value.kind)

    return f"{code(prototype.result)}({', '.join(code(param) for param in prototype.params)})"


def check_lib(lib, report, headers, exports, registry):
    table = {entry["name"]: entry for entry in report["table"]}
    exported = set().union(*exports.values())

    # The sorting per name, against the headers' reading ("refused: <why>" compares as "refused").
    def word(verdict):
        return verdict.split(":")[0] if verdict else verdict

    wrong = [f"{name}: {report['abi'].get(name)} != {ndk_headers.classify(prototype)}"
             for name, prototype in sorted(headers.prototypes.items())
             if word(report["abi"].get(name)) != word(ndk_headers.classify(prototype))]
    check(not wrong, f"{lib}: the sorting of all {len(headers.prototypes)} header prototypes equals the headers' "
                     f"({len(wrong)} differ{': ' + '; '.join(wrong[:8]) if wrong else ''})")
    wrong = [f"{name}: {report['signatures'].get(name)} != {signature(prototype)}"
             for name, prototype in sorted(headers.prototypes.items())
             if report["signatures"].get(name) != signature(prototype)]
    check(not wrong, f"{lib}: the arguments and results of all {len(headers.prototypes)} header prototypes equal the "
                     f"headers' ({len(wrong)} differ{': ' + '; '.join(wrong[:6]) if wrong else ''})")
    counts = collections.Counter(report["abi"].get(name) for name in exported)
    check(dict(counts) == ABI_COUNTS[lib], f"{lib}: the sorting of the {len(exported)} exports is {dict(counts)}, "
                                           f"expected {ABI_COUNTS[lib]}")
    if lib == "vulkan":
        counts = collections.Counter(report["abi"].get(name) for name in headers.prototypes)
        check(dict(counts) == VULKAN_HEADER_COUNTS,
              f"vulkan: the sorting of vulkan_core.h's and vulkan_android.h's {len(headers.prototypes)} commands is "
              f"{dict(counts)}, expected {VULKAN_HEADER_COUNTS}")

    # The stub kinds of the exports.
    missing = sorted(exported - set(table))
    check(not missing, f"{lib}: every exported name is in the table (missing: {missing[:8]})")
    counts = collections.Counter(table[name]["kind"] for name in exported if name in table)
    check(dict(counts) == KIND_COUNTS[lib], f"{lib}: the kinds of the exports are {dict(counts)}, "
                                            f"expected {KIND_COUNTS[lib]}")
    if lib == "vulkan":
        wrong = sorted(name for name in exported if name in table and table[name]["kind"] != "by hand"
                       and (table[name]["kind"] == "without allocator")
                       != ndk_headers.takes_allocator(headers.prototypes[name]))
        check(not wrong, f"vulkan: exactly the exports that take an allocator and are not written by hand go "
                         f"without it ({wrong[:8]})")

    # The export lists.
    for library, names in exports.items():
        with open(os.path.join(report["out"], library.replace(".so", ".map"))) as file:
            listed = set(re.findall(r"^\s*([A-Za-z_]\w*);\s*$", file.read(), re.M))
        check(listed == names, f"{lib}: {library.replace('.so', '.map')} lists exactly the {len(names)} "
                               f"names of the NDK's {library}")

    # Aliases of functions written by hand.
    aliases = xml_aliases(registry, "vulkan" if lib == "vulkan" else "gl")
    hand = {name for name, entry in table.items() if entry["kind"] == "by hand" and entry["hand"] == name}
    wrong, seen = [], 0
    for name in hand:
        for alias in aliases.get(name, {name}) - {name}:
            if alias in table:
                seen += 1
                if table[alias]["kind"] != "by hand" or table[alias]["hand"] not in hand \
                        or table[alias]["hand"] not in aliases.get(name, {name}):
                    wrong.append(f"{alias} -> {table[alias]['kind']} {table[alias].get('hand')}")
    check(not wrong, f"{lib}: the {seen} aliases in the table of functions written by hand get those functions "
                     f"({wrong[:8]})")

    # The table's order and hash.
    names = [entry["name"] for entry in report["table"]]
    digest = hashlib.sha256("".join(name + "\n" for name in names).encode()).hexdigest()[:16]
    check(report["hash"] == digest, f"{lib}: the hash {report['hash']} is SHA-256's first 8 bytes over the "
                                    f"table's {len(names)} names")
    with open(os.path.join(report["out"], f"{lib}_table.h")) as file:
        header = file.read()
    check(f"0x{digest}ull" in header and f"{lib.upper()}_TABLE_LENGTH {len(names) + 1}u" in header,
          f"{lib}: {lib}_table.h carries the hash and the length with the entry's slot")
    check(names == sorted(names) and len(names) == len(set(names)), f"{lib}: the table is sorted, each name once")


def check_structures(report, registry):
    found = structures_with_function_pointers(registry)
    check(found == FUNCTION_POINTER_STRUCTURES, f"vulkan: vk.xml has exactly the five structures with function "
                                                f"pointers ({sorted(found)})")
    handled = report.get("structures", {})
    check(set(handled) == found and all(handled.values()),
          f"vulkan: the generator handles each of them ({handled})")


SAMPLES = {
    "variadic": ("a variadic function", "vulkan", """
        <command><proto><type>void</type> <name>vkSampleVariadic</name></proto>
            <param><type>uint32_t</type> <name>count</name></param><param>...</param></command>"""),
    "structure by value": ("a structure by value", "vulkan", """
        <command><proto><type>void</type> <name>vkSampleStructure</name></proto>
            <param><type>VkExtent2D</type> <name>extent</name></param></command>"""),
    "long double": ("a long double", "vulkan", """
        <command><proto><type>void</type> <name>vkSampleLongDouble</name></proto>
            <param>long double <name>value</name></param></command>"""),
    "char by value": ("a char by value", "vulkan", """
        <command><proto><type>void</type> <name>vkSampleChar</name></proto>
            <param><type>char</type> <name>value</name></param></command>"""),
    "callback with a small result": ("a result under 32 bits", "vulkan", """
        <command><proto><type>void</type> <name>vkSampleCallback</name></proto>
            <param><type>PFN_vkSampleSmall</type> <name>callback</name></param></command>"""),
    "GL char by value": ("a char by value", "gl", """
        <command><proto>void <name>glSampleChar</name></proto>
            <param><ptype>GLchar</ptype> <name>value</name></param></command>"""),
    "GL callback with a small result": ("a result under 32 bits", "gl", """
        <command><proto>void <name>glSampleCallback</name></proto>
            <param><ptype>GLSAMPLEPROC</ptype> <name>callback</name></param></command>"""),
}
CONTROL = {
    "vulkan": """
        <command><proto><type>VkResult</type> <name>vkSampleFine</name></proto>
            <param><type>uint32_t</type> <name>count</name></param>
            <param>const <type>VkExtent2D</type>* <name>pExtent</name></param>
            <param><type>uint16_t</type> <name>pattern</name></param></command>""",
    "gl": """
        <command><proto>void <name>glSampleFine</name></proto>
            <param><ptype>GLboolean</ptype> <name>flag</name></param>
            <param>const <ptype>GLchar</ptype> *<name>text</name></param></command>""",
}
REGISTRY = {
    "vulkan": """<registry><types>
        <type requires="vk_platform" name="void"/><type requires="vk_platform" name="char"/>
        <type requires="vk_platform" name="uint16_t"/><type requires="vk_platform" name="uint32_t"/>
        <type requires="vk_platform" name="uint8_t"/><type name="VkResult" category="enum"/>
        <type category="struct" name="VkExtent2D"><member><type>uint32_t</type> <name>width</name></member></type>
        <type category="funcpointer"><proto><type>uint8_t</type> <name>PFN_vkSampleSmall</name></proto>
            <param><type>uint32_t</type> <name>value</name></param></type>
        </types><commands>{commands}</commands></registry>""",
    "gl": """<registry><types>
        <type>typedef char <name>GLchar</name>;</type><type>typedef unsigned char <name>GLboolean</name>;</type>
        <type>typedef GLboolean (<apientry/> *<name>GLSAMPLEPROC</name>)(GLuint id);</type>
        <type>typedef unsigned int <name>GLuint</name>;</type>
        </types><commands namespace="GL">{commands}</commands></registry>""",
}


def check_samples(options, work):
    frontgen = os.path.join(options.repo, "tools", "frontgen", "frontgen.py")

    def verdict(dialect, commands):
        path = os.path.join(work, "sample.xml")
        with open(path, "w") as file:
            file.write(textwrap.dedent(REGISTRY[dialect].format(commands=commands)))
        result = subprocess.run([sys.executable, frontgen, "--check", path, "--dialect", dialect],
                                capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    for dialect, commands in CONTROL.items():
        code, output = verdict(dialect, commands)
        check(code == 0, f"a {dialect} sample without anything to refuse passes ({output.strip()[:200]})")
    for what, (reason, dialect, commands) in SAMPLES.items():
        code, output = verdict(dialect, commands)
        lines = [line for line in output.splitlines() if "refused" in line]
        check(code != 0 and len(lines) == 1 and reason in lines[0],
              f"a sample with a {what} is refused for it ({' | '.join(lines)[:200]})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--vk-xml", required=True)
    parser.add_argument("--angle", required=True)
    parser.add_argument("--ndk", required=True)
    parser.add_argument("--work", required=True)
    options = parser.parse_args()

    registries = {
        "vulkan": (options.vk_xml, None),
        "gles": (os.path.join(options.angle, "third_party", "OpenGL-Registry", "src", "xml", "gl.xml"),
                 os.path.join(options.angle, "scripts", "gl_angle_ext.xml")),
        "egl": (os.path.join(options.angle, "third_party", "EGL-Registry", "src", "api", "egl.xml"),
                os.path.join(options.angle, "scripts", "egl_angle_ext.xml")),
    }
    missing = [path for pair in registries.values() for path in pair if path and not os.path.exists(path)]
    if missing:
        print(f"skipped: not fetched yet: {', '.join(missing)}; see tools/fetch-moltenvk.sh and tools/build-angle.sh")
        return 0
    try:
        ndk_headers.sysroot(options.ndk)
    except FileNotFoundError:
        print(f"skipped: no NDK at {options.ndk}; set ANDROID_NDK or APK_ANDROID_NDK")
        return 0

    shutil.rmtree(options.work, ignore_errors=True)
    os.makedirs(options.work)
    first, second = os.path.join(options.work, "first"), os.path.join(options.work, "second")
    generated = run_generator(options, first, 0) and run_generator(options, second, 1)
    check(generated, "the generator runs for vulkan, gles and egl")
    if not generated:
        return 1
    check(same_tree(first, second), "two runs under other hash seeds write the same files")

    for lib in LIBS:
        with open(os.path.join(first, f"{lib}_report.json")) as file:
            report = json.load(file)
        report["out"] = first
        registry = load_registry(*registries[lib])
        check_lib(lib, report, ndk_headers.Headers(options.ndk, lib), ndk_headers.ndk_exports(options.ndk, lib),
                  registry)
        if lib == "vulkan":
            check_structures(report, registry)
    check_samples(options, options.work)

    print(f"{len(failures)} of the checks failed" if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
