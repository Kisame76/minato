#!/usr/bin/env python3
"""Test 1 of ADR-0030: the generator in tools/frontgen/.

- Over the NDK's names, its sorting equals the one taken from the NDK's C headers (ndk_headers.py, a
  reading of its own): per name, and in the counts that ADR-0029 and ADR-0030 name.
- Every alias that the XML names for a function written by hand gets that function; every exported name
  is in the table and in the export list; the five structures with function pointers are handled.
- The table's hash is stable: two runs under other hash seeds write the same files. build/graphics/gen/,
  from which tools/build-front-libs.sh builds test 2's program, holds the same files when it exists.
- Every slot answers as the C headers' result type says (decision 3), when the host library lacks its
  function (<lib>_names.inc, for graphics/host) and without a driver (<lib>_bind.c, for the front lib).
- Small XML samples with a variadic function, a structure by value, a long double, a char by value, a
  callback with a small result, more than 8 arguments or a floating-point argument or result are refused.
- libvulkan's lists (decisions 10 and 11), against vk.xml: vkGet*ProcAddr's names with each one's
  function, level and own extensions; the offered extensions with their kind; the structures that may
  extend the create infos whose pNext chains libvulkan copies, with their sType.
- The GLES and EGL lists (ADR-0029, decision 14), against gl.xml and egl.xml with ANGLE's: eglGetProcAddress's
  names with the function each front lib hands out, and the extensions its strings withhold: exactly those
  the description leaves out by name and those with a command outside the table. libGLESv1_CM, libGLESv2 and
  libGLESv3 export minato_gles_proc, for libEGL, in version APKRT_PRIVATE beside the NDK's names.
- The names outside every registry (ADR-0041 a1): exactly Android's 7 *Bounds functions of libGLESv1_CM, which
  the NDK exports and declares in no header. They are written by hand, exported, and kept out of the table. A
  description whose list "outside" names a command of the registry, or a name no library exports, is refused.

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

# The counts of ADR-0029, decision 14 (with ADR-0030's change) and of ADR-0030, test 1 and decision 10. B1
# writes glGetBooleanv, glGetFloatv, glGetIntegerv and glGetInteger64v by hand too, whose GL_NUM_EXTENSIONS
# must count the extensions glGetStringi lists. V5's guard writes vkCreateImageView by hand (ADR-0030).
# ADR-0041 a1 adds libGLESv1_CM: 173 names beyond libGLESv2's and libGLESv3's, 169 the same in both ABIs and 4
# with small arguments (glColor4ub, glDrawTexsOES, glSampleCoveragex, glSampleCoveragexOES), and the 7 outside.
ABI_COUNTS = {
    "gles": {"same": 589, "extend": 37, "repack": 8},
    "egl": {"same": 73},
    "vulkan": {"same": 233},
}
KIND_COUNTS = {
    "gles": {"plain": 578, "extend": 37, "repack": 8, "by hand": 11},
    "egl": {"plain": 55, "by hand": 18},
    "vulkan": {"plain": 149, "without allocator": 45, "by hand": 39},
}
# ADR-0029, decision 14: the extensions the strings withhold by name, whatever their commands; and the
# private exports beside the NDK's names (B1: libEGL finds libGLES's functions with minato_gles_proc).
WITHHELD_BY_NAME = {
    "egl": {"EGL_KHR_debug", "EGL_ANDROID_blob_cache", "EGL_EXT_platform_base"},
    "gles": set(),
}
WITHHELD_PREFIX = {"egl": "EGL_ANGLE_platform_angle", "gles": None}
PRIVATE = {"gles": {"minato_gles_proc"}}
# ADR-0041 a1: the exports that no registry defines, Android's own (opengl/libs/GLES_CM/gl.cpp at the tag).
OUTSIDE = {
    "gles": {"glColorPointerBounds", "glNormalPointerBounds", "glTexCoordPointerBounds", "glVertexPointerBounds",
             "glPointSizePointerOESBounds", "glMatrixIndexPointerOESBounds", "glWeightPointerOESBounds"},
}
API = {"gles": "gles2", "egl": "egl"}
VULKAN_HEADER_COUNTS = {"same": 706, "extend": 3, "repack": 1}
FUNCTION_POINTER_STRUCTURES = {
    "VkAllocationCallbacks", "VkDebugReportCallbackCreateInfoEXT", "VkDebugUtilsMessengerCreateInfoEXT",
    "VkDeviceDeviceMemoryReportCreateInfoEXT", "VkDirectDriverLoadingInfoLUNARG",
}
LIBS = ["vulkan", "gles", "egl"]
# Decision 3, per result type or name: the answer when the host library lacks the function, and the one
# without a driver. VK_ERROR_FEATURE_NOT_PRESENT and VK_ERROR_INCOMPATIBLE_DRIVER; EGL_SUCCESS and
# EGL_NOT_INITIALIZED. Everything else answers 0 both times.
ANSWERS = {"VkResult": (-8, -9), "eglGetError": (0x3000, 0x3001)}
# Decision 11: the extensions libvulkan implements itself, with VK_GOOGLE_display_timing on its swapchains for
# Swappy (ADR-0041 decision 11). And the structures whose pNext chains libvulkan copies, for which it needs the
# size of every structure that may extend them.
OWN_EXTENSIONS = {
    "VK_KHR_surface", "VK_KHR_surface_protected_capabilities", "VK_KHR_android_surface",
    "VK_EXT_swapchain_colorspace", "VK_KHR_get_surface_capabilities2", "VK_GOOGLE_surfaceless_query",
    "VK_KHR_swapchain", "VK_KHR_incremental_present", "VK_GOOGLE_display_timing",
}
CHAINS = {"VkInstanceCreateInfo", "VkDeviceCreateInfo", "VkSubmitInfo", "VkBindSparseInfo"}
# Decision 11's extensions left out by name: Apple's and Metal's, what libvulkan pins or enables itself,
# and those that take or extend the swapchain and the surface, which libvulkan puts its own in front of.
LEFT_OUT = {
    "VK_EXT_metal_surface", "VK_EXT_headless_surface", "VK_EXT_metal_objects", "VK_EXT_external_memory_metal",
    "VK_KHR_portability_subset", "VK_EXT_layer_settings", "VK_KHR_portability_enumeration",
    "VK_KHR_present_id", "VK_KHR_present_id2", "VK_KHR_present_wait",
    "VK_KHR_present_wait2", "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1",
    "VK_EXT_surface_maintenance1", "VK_KHR_surface_maintenance1", "VK_KHR_swapchain_mutable_format",
    "VK_EXT_hdr_metadata", "VK_EXT_calibrated_timestamps", "VK_KHR_calibrated_timestamps",
}

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
    outside = OUTSIDE.get(lib, set())
    exported = set().union(*exports.values()) - outside

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

    # The names outside every registry: neither defined nor declared, written by hand, out of the table.
    defined = sorted(name for name in outside if xml_defines(registry, name) or name in headers.prototypes)
    check(report.get("outside", []) == sorted(outside) and not defined and not outside & set(table)
          and outside <= set().union(*exports.values()),
          f"{lib}: the exports outside every registry are exactly {sorted(outside)}, written by hand and out of the "
          f"table (the report's: {report.get('outside')}; defined somewhere: {defined})")

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

    # The export lists: the NDK's names, hiding everything else, and the private ones in APKRT_PRIVATE. A
    # version script with a named node names every node, so the public one is then the library's.
    private = PRIVATE.get(lib, set())
    for library, names in exports.items():
        with open(os.path.join(report["out"], library.replace(".so", ".map"))) as file:
            nodes = map_nodes(file.read())
        public = library.replace(".so", "").upper() if private else ""
        expected = {public: (names, True)} | ({"APKRT_PRIVATE": (private, False)} if private else {})
        check(nodes == expected, f"{lib}: {library.replace('.so', '.map')} lists exactly the {len(names)} names of "
                                 f"the NDK's {library} and hides the rest"
                                 + (f", and {', '.join(sorted(private))} in version APKRT_PRIVATE" if private else ""))

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


def xml_defines(root, name):
    """Whether the registry's XML defines a command of that name."""
    return any(proto.findtext("name") == name for proto in root.iter("proto"))


def map_nodes(text):
    """A version script's nodes: its name ("" for the anonymous one) with the names its global part lists
    and whether its local part hides everything else."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    nodes = {}
    for name, body in re.findall(r"(\w*)\s*\{(.*?)\}\s*;", text, re.S):
        listed, _, hidden = body.partition("local:")
        nodes[name] = (set(re.findall(r"([A-Za-z_]\w*)\s*;", listed)), hidden.split() == ["*;"])
    return nodes


def check_gl_lists(lib, report, root):
    """ADR-0029, decision 14: <lib>_procs.inc, eglGetProcAddress's names in the table's order, each with the
    function the front lib hands out for it (the one written by hand, else its stub), and <lib>_withheld.inc,
    the extensions the front lib's strings withhold: exactly those it leaves out by name and those with a
    command outside the table, read from the XML here."""
    table = report["table"]

    def read(name, pattern):
        path = os.path.join(report["out"], name)
        check(os.path.exists(path), f"{lib}: the generator writes {name}")
        with open(path if os.path.exists(path) else os.devnull) as file:
            return re.findall(pattern, file.read(), re.M)

    listed = read(f"{lib}_procs.inc", r"^MINATO_PROC\((\w+), (\w+)\)$")
    expected = [(entry["name"], entry["hand"] or entry["name"]) for entry in table]
    check(listed == expected, f"{lib}: {lib}_procs.inc lists the table's {len(expected)} names in its order, each "
                              f"with the function written by hand or else its stub ({len(listed)} listed)")

    names = {entry["name"] for entry in table}
    withheld, reasons = set(), {}
    for extension in root.find("extensions"):
        if API[lib] not in (extension.get("supported") or "").split("|"):
            continue
        name = extension.get("name")
        commands = [command.get("name") for part in extension.iter("require")
                    if part.get("api") in (None, API[lib]) for command in part.findall("command")]
        outside = sorted(set(commands) - names)
        prefix = WITHHELD_PREFIX[lib]
        if name in WITHHELD_BY_NAME[lib] or (prefix and name.startswith(prefix)) or outside:
            withheld.add(name)
            reasons[name] = outside
    listed = read(f"{lib}_withheld.inc", r"^MINATO_WITHHELD\((\w+)\)$")
    check(listed == sorted(withheld), f"{lib}: {lib}_withheld.inc lists the {len(withheld)} extensions left out by "
                                      f"name or with a command outside the table, sorted ({len(listed)} listed; "
                                      f"missing {sorted(withheld - set(listed))[:4]}, extra "
                                      f"{sorted(set(listed) - withheld)[:4]})")
    check(not withheld & set(report["extensions"]["offered"]), f"{lib}: no withheld extension is offered")
    # A prefix withholds the names the host library reports beyond the registry as well
    # (EGL_ANGLE_platform_angle_display_key is in none of the XML).
    prefixes = read(f"{lib}_withheld.inc", r"^MINATO_WITHHELD_PREFIX\((\w+)\)$")
    expected = [WITHHELD_PREFIX[lib]] if WITHHELD_PREFIX[lib] else []
    check(prefixes == expected, f"{lib}: {lib}_withheld.inc names the prefixes the description leaves out ({prefixes})")


def check_structures(report, registry):
    found = structures_with_function_pointers(registry)
    check(found == FUNCTION_POINTER_STRUCTURES, f"vulkan: vk.xml has exactly the five structures with function "
                                                f"pointers ({sorted(found)})")
    handled = report.get("structures", {})
    check(set(handled) == found and all(handled.values()),
          f"vulkan: the generator handles each of them ({handled})")


def ours(element):
    return element.get("api") is None or "vulkan" in element.get("api").split(",")


def vulkan_levels(root):
    """Per command, where vkGet*ProcAddr may find it (decision 10): DEVICE when its first parameter is a
    dispatchable handle that belongs to a VkDevice, INSTANCE when one that belongs to a VkInstance, GLOBAL
    otherwise. Aliases take their command's level."""
    parents, dispatchable = {}, set()
    for kind in root.find("types"):
        if kind.get("category") == "handle" and not kind.get("alias"):
            parents[kind.findtext("name")] = (kind.get("parent") or "").split(",")[0] or None
            if kind.findtext("type") == "VK_DEFINE_HANDLE":
                dispatchable.add(kind.findtext("name"))

    def owner(handle):
        while handle not in (None, "VkInstance", "VkDevice"):
            handle = parents.get(handle)
        return handle

    levels, aliases = {}, {}
    for command in root.find("commands"):
        if command.get("alias"):
            aliases[command.get("name")] = command.get("alias")
            continue
        if not ours(command):
            continue
        params = [param for param in command.findall("param") if ours(param)]
        first = params[0].findtext("type") if params else None
        levels[command.findtext("proto/name")] = \
            ("DEVICE" if owner(first) == "VkDevice" else "INSTANCE") if first in dispatchable else "GLOBAL"
    for name in aliases:
        target = name
        while target in aliases:
            target = aliases[target]
        levels[name] = levels.get(target)
    return levels


def check_vulkan_lists(report, root):
    """Decisions 10 and 11: vulkan_procs.inc (for vkGetInstanceProcAddr and vkGetDeviceProcAddr),
    vulkan_extensions.inc (what libvulkan may offer) and vulkan_structs.inc (for its copies of pNext
    chains), each against vk.xml."""
    table = {entry["name"]: entry for entry in report["table"]}

    def read(name, pattern):
        path = os.path.join(report["out"], name)
        check(os.path.exists(path), f"vulkan: the generator writes {name}")
        with open(path if os.path.exists(path) else os.devnull) as file:
            return re.findall(pattern, file.read(), re.M)

    versions, owners, kinds = {}, collections.defaultdict(set), {}
    for feature in root.iter("feature"):
        if "vulkan" in feature.get("api", "").split(","):
            number = tuple(int(part) for part in feature.get("number").split("."))
            for part in feature.findall("require"):
                for command in part.findall("command") if ours(part) else []:
                    versions[command.get("name")] = min(versions.get(command.get("name"), number), number)
    for extension in root.find("extensions"):
        kinds[extension.get("name")] = (extension.get("type") or "").upper()
        if extension.get("name") in OWN_EXTENSIONS:
            for part in extension.findall("require"):
                owners.update({command.get("name"): owners[command.get("name")] | {extension.get("name")}
                               for command in part.findall("command")})

    # The names, each once, in the table's order; the functions of 1.4 give NULL, so they are left out.
    levels = vulkan_levels(root)
    listed = read("vulkan_procs.inc", r"^MINATO_PROC\((\w+), (\w+), (\w+), (.+)\)$")
    expected = [name for name in table if versions.get(name) != (1, 4)]
    check([name for name, _, _, _ in listed] == expected,
          f"vulkan: vulkan_procs.inc lists the {len(expected)} names of the table below Vulkan 1.4, in its order "
          f"({len(listed)} listed)")
    wrong = []
    for name, function, level, text in listed:
        entry = table.get(name, {})
        named = set(re.findall(r"MINATO_OWNER\((\w+)\)", text))
        if function != (entry.get("hand") or name):
            wrong.append(f"{name}: function {function}")
        if level != levels.get(name):
            wrong.append(f"{name}: {level} != {levels.get(name)}")
        if named != owners.get(name, set()) or (not named and text != "0"):
            wrong.append(f"{name}: owned by {text}, expected {sorted(owners.get(name, set()))}")
    check(not wrong, f"vulkan: each name of vulkan_procs.inc has its function (the one written by hand, else its "
                     f"stub), its level from its first parameter and the own extensions that bring it "
                     f"({len(wrong)} differ{': ' + '; '.join(wrong[:6]) if wrong else ''})")
    check(sum(1 for *_, text in listed if text != "0") == 19,
          "vulkan: the 19 functions of the surface, the swapchain, VK_KHR_get_surface_capabilities2 and "
          "VK_GOOGLE_display_timing have owners")

    # The offered extensions, sorted, each with its kind, the own ones marked.
    listed = read("vulkan_extensions.inc", r"^MINATO_EXTENSION\((\w+), (\w+), (MINATO_OWNER\(\w+\)|0)\)$")
    expected = [(name, kinds.get(name), f"MINATO_OWNER({name})" if name in OWN_EXTENSIONS else "0")
                for name in sorted(report["extensions"]["offered"])]
    check(listed == expected, f"vulkan: vulkan_extensions.inc lists the {len(expected)} offered extensions with "
                              f"their kind from vk.xml ({len(listed)} listed)")
    check(OWN_EXTENSIONS <= set(report["extensions"]["offered"]), "vulkan: the own extensions are offered")
    offered = sorted(LEFT_OUT & set(report["extensions"]["offered"]))
    check(not offered, f"vulkan: none of the {len(LEFT_OUT)} extensions decision 11 leaves out by name is offered "
                       f"({offered})")

    # The structures that may extend the chains' heads: Vulkan's, from core or from an extension that is
    # neither provisional nor another platform's than Android's (those are in no header of libvulkan's),
    # required for Vulkan, not only for Vulkan SC.
    required = collections.defaultdict(set)
    for feature in root.iter("feature"):
        if "vulkan" in feature.get("api", "").split(","):
            for part in filter(ours, feature.iter("require")):
                required.update({kind.get("name"): required[kind.get("name")] | {None}
                                 for kind in part.findall("type")})
    for extension in root.find("extensions"):
        if "vulkan" in (extension.get("supported") or "").split(",") and extension.get("provisional") != "true":
            for part in filter(ours, extension.iter("require")):
                required.update({kind.get("name"): required[kind.get("name")] | {extension.get("platform")}
                                 for kind in part.findall("type")})
    expected = sorted(
        (kind.get("name"), next(member.get("values") for member in kind.findall("member")
                                if member.findtext("name") == "sType"))
        for kind in root.find("types")
        if kind.get("category") == "struct" and not kind.get("alias") and ours(kind)
        and CHAINS & set((kind.get("structextends") or "").split(","))
        and required[kind.get("name")] & {None, "android"})
    listed = read("vulkan_structs.inc", r"^MINATO_STRUCT\((\w+), (\w+)\)$")
    check(sorted(listed) == expected and len(listed) == len(set(listed)),
          f"vulkan: vulkan_structs.inc names the {len(expected)} structures that may extend "
          f"{' and '.join(sorted(CHAINS))} with their sType ({len(listed)} listed; missing "
          f"{sorted(set(expected) - set(listed))[:4]}, extra {sorted(set(listed) - set(expected))[:4]})")


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
    # apkrt_call_guest passes at most 8 integer and pointer arguments and returns x0 (decision 5).
    "callback with 9 arguments": ("more than 8 arguments", "vulkan", """
        <command><proto><type>void</type> <name>vkSampleNine</name></proto>
            <param><type>PFN_vkSampleNine</type> <name>callback</name></param></command>"""),
    "callback with a floating-point argument": ("a floating-point argument", "vulkan", """
        <command><proto><type>void</type> <name>vkSampleFloat</name></proto>
            <param><type>PFN_vkSampleFloat</type> <name>callback</name></param></command>"""),
    "callback with a floating-point result": ("a floating-point result", "vulkan", """
        <command><proto><type>void</type> <name>vkSampleFloatResult</name></proto>
            <param><type>PFN_vkSampleFloatResult</type> <name>callback</name></param></command>"""),
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
        <type category="funcpointer"><proto><type>void</type> <name>PFN_vkSampleNine</name></proto>
            """ + "".join(f"<param><type>uint32_t</type> <name>a{n}</name></param>" for n in range(9)) + """</type>
        <type requires="vk_platform" name="float"/>
        <type category="funcpointer"><proto><type>void</type> <name>PFN_vkSampleFloat</name></proto>
            <param><type>uint32_t</type> <name>index</name></param><param><type>float</type> <name>value</name></param></type>
        <type category="funcpointer"><proto><type>float</type> <name>PFN_vkSampleFloatResult</name></proto>
            <param><type>uint32_t</type> <name>index</name></param></type>
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


def check_outside(options, work):
    """ADR-0041 a1: the generator refuses a description whose list "outside" names a command the registry defines,
    or a name that no library exports, besides the 7 names it takes."""
    sys.path.insert(0, os.path.join(options.repo, "tools", "frontgen"))
    import frontgen
    registry = frontgen.Registry(
        os.path.join(options.angle, "third_party", "OpenGL-Registry", "src", "xml", "gl.xml"), "gl",
        os.path.join(options.angle, "scripts", "gl_angle_ext.xml"))
    exports = frontgen.ndk_exports(options.ndk, [("libGLESv1_CM.so", 35)])
    for what, name in (("a command of the registry", "glAlphaFunc"), ("a name no library exports",
                                                                     "glNotExportedBounds")):
        path = os.path.join(work, "outside.txt")
        with open(path, "w") as file:
            file.write(f"registry gl\nlibrary libGLESv1_CM.so 35\noutside {' '.join(sorted(OUTSIDE['gles']))}\n"
                       f"outside {name}\n")
        try:
            frontgen.FrontLib("gles", frontgen.Description(path), registry, exports)
            refused = ""
        except frontgen.GeneratorError as why:
            refused = str(why)
        check("outside, but" in refused and name in refused,
              f"gles: a description with {what} ({name}) outside is refused ({refused[:120] or 'taken'})")


def check_answers(lib, report, prototypes):
    """Decision 3: graphics/host's list per slot (the table's name, the host function's name, the kind of
    result and the answer when the host library lacks the function) and the front lib's answers without a
    driver, against the result types of the NDK's and the host library's C headers."""
    with open(os.path.join(report["out"], f"{lib}_names.inc")) as file:
        listed = re.findall(r'^MINATO_NAME\((\w+), "(\w+)", (VOID|INT|FLOAT), (-?\w+)\)$', file.read(), re.M)
    bind = os.path.join(report["out"], f"{lib}_bind.c")
    check(os.path.exists(bind), f"{lib}: the generator writes {lib}_bind.c")
    with open(bind if os.path.exists(bind) else os.devnull) as file:
        no_driver = {name: int(value, 0) for name, value in
                     re.findall(rf"^    \[MINATO_{lib.upper()}_SLOT_(\w+)\] = (-?\w+),$", file.read(), re.M)}
    check([(name, host) for name, host, _, _ in listed] == [(entry["name"], entry["host"]) for entry in report["table"]],
          f"{lib}: {lib}_names.inc lists the table's {len(report['table'])} names in its order, each with its host "
          f"function's name")
    wrong = []
    for name, _, result, answer in listed:
        prototype = prototypes.get(name)
        if prototype is None:
            wrong.append(f"{name} is in no C header")
            continue
        kind = {"void": "VOID", "float": "FLOAT"}.get(prototype.result.kind, "INT")
        expected = (kind,) + (ANSWERS.get(name) or ANSWERS.get(prototype.result.text) or (0, 0))
        got = (result, int(answer, 0), no_driver.pop(name, 0))
        if got != expected:
            wrong.append(f"{name}: {got} != {expected}")
    wrong += [f"{name} is not in the table" for name in no_driver]
    check(not wrong, f"{lib}: every slot answers as its C result type says, when the host library lacks its "
                     f"function and without a driver ({len(wrong)} differ{': ' + '; '.join(wrong[:6]) if wrong else ''})")


def check_values(repo, registries):
    """The answers name enumerants, which the generator reads with the value of its own API: gl.xml gives
    GL_ACTIVE_PROGRAM_EXT 0x8259 for GLES and 0x8B8D for desktop GL."""
    sys.path.insert(0, os.path.join(repo, "tools", "frontgen"))
    from registry import Registry
    values = Registry(registries["gles"][0], "gl", registries["gles"][1]).values
    check(values.get("GL_ACTIVE_PROGRAM_EXT") == 0x8259, f"the generator reads GLES's value of an enumerant that "
                                                        f"desktop GL numbers otherwise ({values.get('GL_ACTIVE_PROGRAM_EXT')})")


def check_built(repo, fresh):
    """Test 2 runs thunk-test as tools/build-front-libs.sh built it from build/graphics/gen/: files there
    that differ from the generator's own output would let test 2 pass on stale stubs."""
    built = os.path.join(repo, "build", "graphics", "gen")
    if not os.path.isdir(built):
        return
    stale = [name for name in sorted(os.listdir(fresh)) if not os.path.exists(os.path.join(built, name))
             or not filecmp.cmp(os.path.join(fresh, name), os.path.join(built, name), shallow=False)]
    check(not stale, f"build/graphics/gen/ holds the generator's current output; otherwise rerun "
                     f"tools/build-front-libs.sh ({', '.join(stale[:6])})")


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
            check_vulkan_lists(report, registry)
        else:
            check_gl_lists(lib, report, registry)
        include = os.path.join(os.path.dirname(options.vk_xml), "include") if lib == "vulkan" else \
            os.path.join(options.angle, "include")
        prototypes = ndk_headers.Headers(options.ndk, lib, include=include).prototypes
        prototypes.update(ndk_headers.Headers(options.ndk, lib).prototypes)
        check_answers(lib, report, prototypes)
    check_samples(options, options.work)
    check_outside(options, options.work)
    check_values(options.repo, registries)
    check_built(options.repo, first)

    print(f"{len(failures)} of the checks failed" if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
