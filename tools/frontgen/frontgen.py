#!/usr/bin/env python3
"""The generator of ADR-0030, decision 7: for each front lib (libvulkan, libGLESv2 with libGLESv3, libEGL)
its table of host functions, the kind of stub each name gets, its export lists and a report, from the
Khronos XML, the NDK's stubs and the front lib's description in graphics/<lib>/frontgen.txt.

  tools/frontgen/frontgen.py --out build/graphics/gen [--lib vulkan|gles|egl ...]
                             [--vk-xml PATH] [--angle DIR] [--ndk DIR]
  tools/frontgen/frontgen.py --check REGISTRY.xml --dialect vulkan|gl|egl

Writes into --out, never into the repo: <lib>_stubs.S (the table and a stub per name, guest code),
<lib>_bind.c (the front lib's first constructor, which binds the table, and each slot's answer without a
driver; guest code), <lib>_table.h (the table's layout, for the guest and the host build),
<lib>_names.inc (the provider's list in the table's order: each name with its host function's name, its
kind of result and its answer when the host library lacks it), one <library>.map per library (its
version script, which puts the description's "private" names into version APKRT_PRIVATE) and
<lib>_report.json (the sorting, name by name); for Vulkan also libvulkan's lists, vulkan_procs.inc
(vkGet*ProcAddr's names), vulkan_extensions.inc (what it may offer) and vulkan_structs.inc (the structures
in the pNext chains it copies); for GLES and EGL <lib>_procs.inc (eglGetProcAddress's names, ADR-0029,
decision 14) and <lib>_withheld.inc (the extensions the lib's strings leave out). The answers (decision 3)
are 0 unless the description's "answer" names others for a result type or a name. --check reads one
registry on its own and says, for each command and callback in it, whether it can cross between the two
ABIs. Standard library only."""
import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys

import abi
from registry import Registry

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIBS = ("vulkan", "gles", "egl")
KINDS = {"same": "plain", "extend": "extend", "repack": "repack"}
HANDLINGS = ("null", "host-callback", "removed")


class GeneratorError(Exception):
    pass


class Description:
    """graphics/<lib>/frontgen.txt: one keyword per line with its words; "#" starts a comment."""

    LISTS = ("hand", "own", "exclude", "exclude-prefix", "exclude-if-takes", "exclude-if-extends", "platform",
             "chain", "private")

    def __init__(self, path):
        self.path, self.dialect, self.libraries, self.structures, self.answers = path, None, [], {}, {}
        self.version = None
        self.lists = {keyword: set() for keyword in self.LISTS}
        with open(path) as file:
            for number, line in enumerate(file, 1):
                words = line.split("#")[0].split()
                if not words:
                    continue
                keyword, values = words[0], words[1:]
                if keyword == "registry" and len(values) == 1 and values[0] in ("vulkan", "gl", "egl"):
                    self.dialect = values[0]
                elif keyword == "version" and len(values) == 1 and all(part.isdigit() for part in values[0].split(".")):
                    self.version = tuple(int(part) for part in values[0].split("."))
                elif keyword == "library" and len(values) == 2 and values[1].isdigit():
                    self.libraries.append((values[0], int(values[1])))
                elif keyword in self.lists and values:
                    self.lists[keyword].update(values)
                elif keyword == "structure" and len(values) == 2 and values[1] in HANDLINGS:
                    self.structures[values[0]] = values[1]
                elif keyword == "answer" and len(values) == 3:
                    self.answers[values[0]] = (values[1], values[2])
                else:
                    raise GeneratorError(f"{path}:{number}: cannot read {line.strip()!r}")
        if not self.dialect or not self.libraries:
            raise GeneratorError(f"{path}: needs a registry and at least one library")


def ndk_prebuilt(ndk):
    prebuilt = sorted(glob.glob(os.path.join(ndk, "toolchains", "llvm", "prebuilt", "*")))
    if not prebuilt:
        raise GeneratorError(f"no NDK at {ndk}; set ANDROID_NDK or pass --ndk")
    return prebuilt[0]


def ndk_exports(ndk, libraries):
    """What the NDK's stub of each library exports at its API level, read with the NDK's llvm-nm."""
    prebuilt = ndk_prebuilt(ndk)
    result = {}
    for soname, level in libraries:
        stub = os.path.join(prebuilt, "sysroot", "usr", "lib", "aarch64-linux-android", str(level), soname)
        output = subprocess.run([os.path.join(prebuilt, "bin", "llvm-nm"), "-D", "--defined-only",
                                 "--format=just-symbols", stub], check=True, capture_output=True, text=True).stdout
        result[soname] = sorted({line.strip() for line in output.splitlines() if line.strip()})
    return result


def ndk_revision(ndk):
    try:
        with open(os.path.join(ndk, "source.properties")) as file:
            for line in file:
                if line.startswith("Pkg.Revision"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def digest(path):
    with open(path, "rb") as file:
        return hashlib.sha256(file.read()).hexdigest()


class FrontLib:
    """One front lib: its table, the kind of each name in it and the extensions it may offer."""

    def __init__(self, lib, description, registry, exports):
        self.lib, self.description, self.registry, self.exports = lib, description, registry, exports
        self.exported = set().union(*map(set, exports.values()))
        self.hand = description.lists["hand"]
        unknown = sorted(name for name in self.hand | self.exported if registry.command(name) is None)
        if unknown:
            raise GeneratorError(f"{lib}: not in the registry: {', '.join(unknown)}")
        self.groups = self._alias_groups()
        relevant = sorted(self._relevant())
        self.abi = {name: self._verdict(name) for name in relevant}
        self.signatures = {name: abi.signature(registry.command(name).result, registry.command(name).params)
                           for name in relevant if registry.command(name)}
        self.offered, self.left_out = self._choose_extensions()
        self.required = {}
        for extension in self.offered:
            for command, depends in registry.extensions[extension].commands:
                self.required.setdefault(command, set()).add(f"{extension} with {depends}" if depends else extension)
        names = self.exported | {name for name in registry.versions if registry.command(name)} | set(self.required)
        missing = sorted(self.hand - names)
        if missing:
            raise GeneratorError(f"{lib}: written by hand, but neither exported nor offered: {', '.join(missing)}")
        self.answers, self.answered = self._answers(), set()
        self.table = [self._entry(name) for name in sorted(names)]
        unused = sorted(set(self.answers) - self.answered)
        if unused:
            raise GeneratorError(f"{lib}: answer for neither a result type nor a name of the table: {', '.join(unused)}")
        self.structures = self._structures()
        self._check_callbacks()
        self.hash = hashlib.sha256("".join(entry["name"] + "\n" for entry in self.table).encode()).hexdigest()[:16]
        self.procs = self._procs() if registry.dialect == "vulkan" else None

    def _procs(self):
        """libvulkan's lists (decisions 10 and 11): for vkGet*ProcAddr, each name up to the description's
        version with the function to hand out, its level and the own extensions that bring it; the offered
        extensions with their kind; the structures that may extend the description's "chain" heads."""
        own, version = self.description.lists["own"], self.description.version
        if version is None:
            raise GeneratorError(f"{self.lib}: the description names no version")
        procs = []
        for entry in self.table:
            if entry["version"] and tuple(int(part) for part in entry["version"].split(".")) > version:
                continue
            owners = sorted({requirement.split(" with ")[0] for requirement in entry["extensions"]} & own)
            procs.append((entry["name"], entry["hand"] or entry["name"], self.registry.level(entry["name"]), owners))
        kinds = [(name, self.registry.extensions[name].kind.upper(), name in own) for name in self.offered]
        structures = self.registry.chain_structures(self.description.lists["chain"],
                                                    {None} | self.description.lists["platform"])
        return {"procs": procs, "extensions": kinds, "structures": structures}

    def _alias_groups(self):
        parent = {}

        def find(name):
            while parent.setdefault(name, name) != name:
                name = parent[name]
            return name

        for name, target in self.registry.aliases.items():
            parent[find(name)] = find(target)
        groups = {}
        for name in list(parent):
            groups.setdefault(find(name), set()).add(name)
        return {name: group for group in groups.values() for name in group}

    def _hand_for(self, name):
        """The function written by hand that stands for a name: its own, or that of an alias (decision 2)."""
        if name in self.hand:
            return name
        return next(iter(sorted(self.groups.get(name, set()) & self.hand)), None)

    def _relevant(self):
        """Every command of the API: its versions', its extensions' (other platforms' left out), the exports."""
        names = set(self.registry.versions) | self.exported
        for extension in self.registry.extensions.values():
            if not extension.platform or extension.platform in self.description.lists["platform"]:
                names.update(command for command, _ in extension.commands)
        return names

    def _verdict(self, name):
        command = self.registry.command(name)
        if command is None:
            return "unknown: not defined"
        try:
            return abi.classify(command.result, command.params)["abi"]
        except abi.Refused as why:
            return f"refused: {why}"

    def _takes(self, command, targets):
        """The first of targets that a command takes, directly or inside the structures it points to."""
        seen, stack = set(), [param.base for param in command.params if param.base]
        while stack:
            name = stack.pop()
            if name in targets:
                return name
            if name not in seen:
                seen.add(name)
                stack += [member for member in self.registry.members(name) if member]
        return None

    def _choose_extensions(self):
        """The extensions the front lib may offer, and why each other one is left out (decision 11)."""
        lists = self.description.lists
        prefixes = tuple(sorted(lists["exclude-prefix"]))
        offered, left_out = [], {}
        for name, extension in sorted(self.registry.extensions.items()):
            if extension.platform and extension.platform not in lists["platform"]:
                left_out[name] = f"platform {extension.platform}"
                continue
            if name in lists["exclude"] or (prefixes and name.startswith(prefixes)):
                left_out[name] = "left out by the description"
                continue
            own = name in lists["own"]
            reasons = []
            for command, _ in extension.commands:
                definition = self.registry.command(command)
                if definition is None:
                    reasons.append(f"{command} is not defined")
                    continue
                taken = self._takes(definition, lists["exclude-if-takes"])
                hand = self._hand_for(command)
                if taken and own and not hand:
                    raise GeneratorError(f"{self.lib}: {command} of {name}, which the front lib implements itself, "
                                         f"takes {taken} but is not written by hand")
                if taken and not own:
                    reasons.append(f"{command} takes {taken}")
                if hand:
                    continue
                verdict = self.abi.get(command) or self._verdict(command)
                if not verdict.startswith(("same", "extend", "repack")):
                    reasons.append(f"{command}: {verdict}")
                callbacks = sorted({value.callback for value in [definition.result] + definition.params
                                    if value.kind == "callback"})
                if callbacks:
                    reasons.append(f"{command} passes {', '.join(callbacks)} and is not written by hand")
            if not own:
                for kind in extension.types:
                    hit = sorted(self.registry.extends(kind) & lists["exclude-if-extends"])
                    if hit:
                        reasons.append(f"{kind} extends {', '.join(hit)}")
                hit = sorted(extension.enum_extends & lists["exclude-if-extends"])
                if hit:
                    reasons.append(f"it adds values to {', '.join(hit)}")
            if reasons:
                left_out[name] = "; ".join(sorted(set(reasons)))
            else:
                offered.append(name)
        return offered, left_out

    def _answers(self):
        """Decision 3: the description's answers, per result type or name, as the registry's numbers."""
        answers = {}
        for key, words in self.description.answers.items():
            unknown = [word for word in words if word not in self.registry.values]
            if unknown:
                raise GeneratorError(f"{self.lib}: answer {key}: no enumerant of the registry: {', '.join(unknown)}")
            answers[key] = [self.registry.values[word] for word in words]
        return answers

    def _entry(self, name):
        command = self.registry.command(name)
        hand = self._hand_for(name)
        result = {"void": "VOID", "float": "FLOAT"}.get(command.result.kind, "INT")
        key = name if name in self.answers else command.result.base
        if key in self.answers:
            if result != "INT":
                raise GeneratorError(f"{self.lib}: {name}: an answer for a result that is no integer or pointer")
            self.answered.add(key)
        try:
            layout = abi.classify(command.result, command.params)
        except abi.Refused as why:
            raise GeneratorError(f"{self.lib}: {name}: {why}") from None
        callbacks = sorted({value.callback for value in [command.result] + command.params if value.kind == "callback"})
        allocators = [index for index, param in enumerate(command.params)
                      if param.kind == "pointer" and param.base == "VkAllocationCallbacks"]
        if hand:
            kind = "by hand"
        elif callbacks:
            raise GeneratorError(f"{self.lib}: {name} passes {', '.join(callbacks)}, so it must be written by hand "
                                 f"(ADR-0030, decision 5)")
        elif allocators:
            if layout["abi"] != "same":
                raise GeneratorError(f"{self.lib}: {name} takes an allocator and needs {layout['abi']}: no stub kind "
                                     f"does both")
            kind = "without allocator"
        else:
            kind = KINDS[layout["abi"]]
        target = self.registry.target(name)
        number = self.registry.versions.get(name)
        return {
            "name": name, "kind": kind, "abi": layout["abi"], "extend": layout["extend"], "stack": layout["stack"],
            "allocator": allocators, "callbacks": callbacks, "hand": hand,
            "alias_of": target if target != name else None,
            "version": ".".join(map(str, number)) if number else None,
            "extensions": sorted(self.required.get(name, ())),
            "host": "EGL_" + name[3:] if self.registry.dialect == "egl" else name,
            "result": result, "answers": self.answers.get(key, [0, 0]),
        }

    def _structures(self):
        """Decision 5: every structure with a function pointer (Vulkan SC's left out) is handled, and only those."""
        required = self.registry.required_by()
        found = {name for name, element in self.registry.types.items()
                 if element.get("category") in ("struct", "union")
                 and any(member in self.registry.callbacks for member in self.registry.members(name))
                 and self.registry.api in required.get(name, set())}
        declared = set(self.description.structures)
        if found != declared:
            raise GeneratorError(f"{self.lib}: structures with function pointers not handled: "
                                 f"{sorted(found - declared)}; handled but unknown: {sorted(declared - found)}")
        return {name: self.description.structures[name] for name in sorted(found)}

    def _check_callbacks(self):
        """Decision 5: the callbacks that can reach the app need no conversion either way."""
        names = {callback for entry in self.table for callback in entry["callbacks"]}
        for structure in self.structures:
            names.update(member for member in self.registry.members(structure) if member in self.registry.callbacks)
        for name in sorted(names):
            try:
                abi.check_callback(name, *self.registry.callbacks[name])
            except abi.Refused as why:
                raise GeneratorError(f"{self.lib}: {why}") from None


def registry_paths(lib, options):
    if lib == "vulkan":
        return options.vk_xml, None
    if lib == "gles":
        return (os.path.join(options.angle, "third_party", "OpenGL-Registry", "src", "xml", "gl.xml"),
                os.path.join(options.angle, "scripts", "gl_angle_ext.xml"))
    return (os.path.join(options.angle, "third_party", "EGL-Registry", "src", "api", "egl.xml"),
            os.path.join(options.angle, "scripts", "egl_angle_ext.xml"))


COPY = {1: ("ldrb", "strb", "w16"), 2: ("ldrh", "strh", "w16"), 4: ("ldr", "str", "w16"), 8: ("ldr", "str", "x16")}


def stub(front, entry, slot):
    """One stub (decision 2): extend the small arguments, set the allocator to NULL, then branch to the
    entry with the host function in x16; with stack arguments at other places, a frame of the stub's own
    copies them into Apple's layout and calls the entry. Only x16 and x17 besides the arguments it changes,
    and x29 and x30, saved, in a frame."""
    symbol = f"minato_host_{entry['name']}" if entry["kind"] == "by hand" else entry["name"]
    params = front.registry.command(entry["name"]).params
    registers = abi.places(params)
    if entry["allocator"] and entry["abi"] != "same":
        raise GeneratorError(f"{front.lib}: {entry['name']} takes an allocator and needs {entry['abi']}")
    if 8 * slot > 32760:
        raise GeneratorError(f"{front.lib}: slot {slot} lies beyond what one ldr reaches from the table")
    lines = ["", f"    .globl {symbol}"]
    if symbol not in front.exported:
        lines.append(f"    .hidden {symbol}")
    lines += [f"    .type {symbol}, %function", "    .p2align 2", f"{symbol}:", "    .cfi_startproc"]
    lines += [f"    {instruction} {register}, {register}" for register, instruction in entry["extend"]]
    for index in entry["allocator"]:
        if registers[index][0] != "x":
            raise GeneratorError(f"{front.lib}: {entry['name']} passes its allocator on the stack")
        lines.append(f"    mov x{registers[index][1]}, xzr")
    table = f"minato_{front.lib}_table"
    load = [f"    adrp x17, {table}", f"    add x17, x17, :lo12:{table}", f"    ldr x16, [x17, #{8 * slot}]",
            "    ldr x17, [x17]"]
    if entry["abi"] != "repack":
        lines += load + ["    br x17"]
    else:
        frame = (max(apple + size for _, apple, size in entry["stack"]) + 15) // 16 * 16
        lines += ["    stp x29, x30, [sp, #-16]!", "    .cfi_def_cfa_offset 16", "    .cfi_offset w30, -8",
                  "    .cfi_offset w29, -16", "    mov x29, sp", "    .cfi_def_cfa w29, 16", f"    sub sp, sp, #{frame}"]
        for aapcs, apple, size in entry["stack"]:
            read, written, register = COPY[size]
            lines += [f"    {read} {register}, [x29, #{16 + aapcs}]", f"    {written} {register}, [sp, #{apple}]"]
        lines += load + ["    blr x17", "    mov sp, x29", "    ldp x29, x30, [sp], #16", "    .cfi_def_cfa sp, 0",
                         "    .cfi_restore w30", "    .cfi_restore w29", "    ret"]
    return lines + ["    .cfi_endproc", f"    .size {symbol}, . - {symbol}"]


def stubs(front, note):
    """<lib>_stubs.S: the table, zero until the runtime binds it (decision 3), and a stub per name in the
    table's order."""
    table, length = f"minato_{front.lib}_table", len(front.table) + 1
    lines = [f"// {note}",
             f"// Guest code. {table} holds the runtime's entry apkrt_host_call in slot 0 and the host functions",
             "// after it; each stub loads its host function into x16 and the entry into x17 and branches to the",
             "// entry (decision 2). A name written by hand gets its stub as minato_host_<name>, for the function",
             "// written by hand to call.", "", "    .bss", "    .p2align 3", f"    .globl {table}",
             f"    .hidden {table}", f"    .type {table}, %object", f"{table}:", f"    .zero {8 * length}",
             f"    .size {table}, {8 * length}", "", "    .text"]
    for slot, entry in enumerate(front.table, 1):
        lines += stub(front, entry, slot)
    return "\n".join(lines) + "\n"


def number(value):
    """An answer as C source: enumerants of EGL and GL read better in hexadecimal."""
    return f"{value:#x}" if value > 0 else str(value)


def binding(front, note):
    """<lib>_bind.c: the front lib's first constructor binds its table (decision 3); without a driver, each
    slot answers what the list here says, through minato_no_driver (graphics/guest/minato_bind.h)."""
    lib, prefix = front.lib, f"MINATO_{front.lib.upper()}"
    lines = [f"/* {note}",
             f"   The front lib's first constructor binds its table through the runtime's syscall \"bind host "
             f"table\"", f"   (ADR-0030, decision 3). Without a driver, slot n answers minato_{lib}_no_driver[n]. */",
             '#include "minato_bind.h"', f'#include "{lib}_table.h"', "",
             f"extern void *minato_{lib}_table[{prefix}_TABLE_LENGTH] MINATO_HIDDEN;", "",
             "/* The slots not named here answer 0. */",
             f"MINATO_HIDDEN const int64_t minato_{lib}_no_driver[{prefix}_TABLE_LENGTH] = {{",
             "    [0] = 0,  /* the entry's slot, which gets minato_no_driver */"]
    lines += [f"    [{prefix}_SLOT_{entry['name']}] = {number(entry['answers'][1])}," for entry in front.table
              if entry["answers"][1]]
    lines += ["};", "", "/* 0 once the constructor bound the table, else the syscall's negative error. */",
              f"MINATO_HIDDEN int minato_{lib}_binding;", "",
              f"__attribute__((constructor(101))) static void bind_{lib}(void) {{",
              f"    minato_{lib}_binding = minato_bind_host_table(\"{lib}\", minato_{lib}_table, {prefix}_TABLE_LENGTH,",
              f"{' ' * (len(lib) + 45)}{prefix}_TABLE_HASH, minato_{lib}_no_driver);", "}", ""]
    return "\n".join(lines)


def write(front, out, inputs):
    os.makedirs(out, exist_ok=True)
    lib, prefix = front.lib, f"MINATO_{front.lib.upper()}"
    note = (f"Generated by tools/frontgen (ADR-0030, decision 7) from "
            f"{os.path.relpath(front.description.path, ROOT)}; do not edit.")
    header = [f"/* {note}", f"   Slot 0 holds the runtime's entry apkrt_host_call, slot n the host function of the "
              f"table's n-th name. */",
              f"#ifndef {prefix}_TABLE_H", f"#define {prefix}_TABLE_H", "",
              f"#define {prefix}_TABLE_LENGTH {len(front.table) + 1}u",
              f"#define {prefix}_TABLE_HASH 0x{front.hash}ull", "", f"enum {lib}_slot {{",
              f"    {prefix}_SLOT_ENTRY = 0,"]
    header += [f"    {prefix}_SLOT_{entry['name']} = {index}," for index, entry in enumerate(front.table, 1)]
    header += ["};", "", f"#endif", ""]
    names = [f"MINATO_NAME({entry['name']}, \"{entry['host']}\", {entry['result']}, {number(entry['answers'][0])})\n"
             for entry in front.table]
    files = {f"{lib}_table.h": "\n".join(header),
             f"{lib}_names.inc": (f"/* {note}\n   Slot 1 on: MINATO_NAME(the table's name, the host function's name, "
                                  f"its kind of result, its answer\n   when the host library lacks it). */\n"
                                  + "".join(names)),
             f"{lib}_stubs.S": stubs(front, note), f"{lib}_bind.c": binding(front, note)}
    # The version scripts. With private names every node needs a name (an anonymous one stands alone), so
    # the NDK's names then take the library's.
    private = sorted(front.description.lists["private"])
    for soname, names in front.exports.items():
        body = "".join(f"    {name};\n" for name in names)
        if private:
            text = (f"/* {note}\n   The names that the NDK's {soname} exports, and in version APKRT_PRIVATE the "
                    f"lib's own for Minato's other front libs. */\n{soname.replace('.so', '').upper()} {{\n"
                    f"  global:\n{body}  local:\n    *;\n}};\nAPKRT_PRIVATE {{\n  global:\n"
                    + "".join(f"    {name};\n" for name in private) + "};\n")
        else:
            text = (f"/* {note}\n   The names that the NDK's {soname} exports. */\n"
                    f"{{\n  global:\n{body}  local:\n    *;\n}};\n")
        files[soname.replace(".so", ".map")] = text
    if front.registry.dialect != "vulkan":
        procs = "".join(f"MINATO_PROC({entry['name']}, {entry['hand'] or entry['name']})\n" for entry in front.table)
        files[f"{lib}_procs.inc"] = (
            f"/* {note}\n   For eglGetProcAddress (ADR-0029, decision 14): the table's names in its order, "
            f"MINATO_PROC(name, the function\n   handed out, which is the one written by hand or else the stub). */\n"
            + procs)
        withheld = "".join(f"MINATO_WITHHELD({name})\n" for name in sorted(front.left_out))
        withheld += "".join(f"MINATO_WITHHELD_PREFIX({prefix})\n"
                            for prefix in sorted(front.description.lists["exclude-prefix"]))
        files[f"{lib}_withheld.inc"] = (
            f"/* {note}\n   The extensions the lib's strings leave out (ADR-0029, decision 14), sorted: those "
            f"the description\n   leaves out and those whose functions are not all in the table, then the "
            f"description's prefixes,\n   which also leave out names the host library reports beyond the "
            f"registry. */\n" + withheld)
    if front.procs is not None:
        version = ".".join(map(str, front.description.version))
        procs = "".join(f"MINATO_PROC({name}, {function}, {level}, "
                        f"{' | '.join(f'MINATO_OWNER({owner})' for owner in owners) or '0'})\n"
                        for name, function, level, owners in front.procs["procs"])
        files[f"{lib}_procs.inc"] = (
            f"/* {note}\n   For vkGetInstanceProcAddr and vkGetDeviceProcAddr (ADR-0030, decision 10): the table's "
            f"names up to Vulkan {version},\n   in its order: MINATO_PROC(name, the function handed out, which is the "
            f"one written by hand or\n   else the stub, the level it is found at, the own extensions that bring it "
            f"or 0). */\n" + procs)
        extensions = "".join(f"MINATO_EXTENSION({name}, {kind}, {f'MINATO_OWNER({name})' if own else '0'})\n"
                             for name, kind, own in front.procs["extensions"])
        files[f"{lib}_extensions.inc"] = (
            f"/* {note}\n   The extensions the lib may offer (ADR-0030, decision 11), sorted: "
            f"MINATO_EXTENSION(name, INSTANCE or\n   DEVICE, MINATO_OWNER(name) for one it implements itself, else 0). "
            f"It lists only those the host library\n   reports too. */\n" + extensions)
        structures = "".join(f"MINATO_STRUCT({name}, {kind})\n" for name, kind in front.procs["structures"])
        files[f"{lib}_structs.inc"] = (
            f"/* {note}\n   The structures that may extend {' or '.join(sorted(front.description.lists['chain']))}, "
            f"whose pNext chains the lib\n   copies: MINATO_STRUCT(type, its sType). */\n" + structures)
    kinds = {}
    for entry in front.table:
        kinds[entry["kind"]] = kinds.get(entry["kind"], 0) + 1
    exported_kinds, exported_abi = {}, {}
    for entry in front.table:
        if entry["name"] in front.exported:
            exported_kinds[entry["kind"]] = exported_kinds.get(entry["kind"], 0) + 1
            exported_abi[entry["abi"]] = exported_abi.get(entry["abi"], 0) + 1
    report = {
        "lib": lib, "description": os.path.relpath(front.description.path, ROOT), "inputs": inputs,
        "hash": front.hash, "table": front.table, "abi": front.abi, "signatures": front.signatures,
        "exports": front.exports,
        "extensions": {"offered": front.offered, "left out": front.left_out}, "structures": front.structures,
        "counts": {"table": len(front.table), "kinds": kinds, "kinds of the exports": exported_kinds,
                   "abi of the exports": exported_abi, "offered": len(front.offered),
                   "left out": len(front.left_out)},
    }
    files[f"{lib}_report.json"] = json.dumps(report, indent=1, sort_keys=True) + "\n"
    for name, text in files.items():
        with open(os.path.join(out, name), "w") as file:
            file.write(text)


def generate(lib, options):
    description = Description(os.path.join(ROOT, "graphics", lib, "frontgen.txt"))
    path, extension_path = registry_paths(lib, options)
    for needed in filter(None, (path, extension_path)):
        if not os.path.exists(needed):
            raise GeneratorError(f"{lib}: {needed} is missing; see tools/fetch-moltenvk.sh and tools/build-angle.sh")
    registry = Registry(path, description.dialect, extension_path)
    front = FrontLib(lib, description, registry, ndk_exports(options.ndk, description.libraries))
    inputs = {os.path.basename(item): digest(item) for item in filter(None, (path, extension_path))}
    inputs["ndk"] = ndk_revision(options.ndk)
    write(front, options.out, inputs)
    print(f"{lib}: {len(front.table)} names in the table, hash {front.hash}; {len(front.offered)} extensions "
          f"offered, {len(front.left_out)} left out")


def check(path, dialect):
    """Every command of one registry and every callback its commands pass: its class, or why it is refused."""
    registry = Registry(path, dialect)
    refused = False
    for name, command in sorted(registry.commands.items()):
        try:
            print(f"{name}: {abi.classify(command.result, command.params)['abi']}")
        except abi.Refused as why:
            print(f"{name}: refused: {why}")
            refused = True
    used = {value.callback for command in registry.commands.values()
            for value in [command.result] + command.params if value.kind == "callback"}
    for name in sorted(used):
        result, params = registry.callbacks[name]
        try:
            abi.check_callback(name, result, params)
            print(f"{name}: a callback that needs no conversion")
        except abi.Refused as why:
            print(f"{name}: refused: {why}")
            refused = True
    return 1 if refused else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", help="the folder for the generated files, e.g. build/graphics/gen")
    parser.add_argument("--lib", action="append", choices=LIBS, help="only this front lib (repeatable)")
    parser.add_argument("--vk-xml", default=os.path.join(ROOT, "build", "moltenvk", "vk.xml"))
    parser.add_argument("--angle", default=os.path.join(ROOT, "build", "angle", "angle"))
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK") or
                        os.path.expanduser("~/Library/Android/sdk/ndk/30.0.16248370"))
    parser.add_argument("--check", metavar="REGISTRY", help="only check one registry's commands and callbacks")
    parser.add_argument("--dialect", choices=("vulkan", "gl", "egl"), default="vulkan")
    options = parser.parse_args(argv)
    if options.check:
        return check(options.check, options.dialect)
    if not options.out:
        parser.error("needs --out or --check")
    try:
        for lib in options.lib or LIBS:
            generate(lib, options)
    except GeneratorError as error:
        print(f"frontgen: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
