#!/usr/bin/env python3
"""Lists the places in ART's arm64 compiler and assembly stubs where a heap reference becomes an address, for the
reference codec (ADR-0053, step c2.0): references are 32-bit offsets into the heap window, so every such place needs
the decode (B | reference, with B in x20), or is one that does not (a raw address, a stack slot, a thread field).

The scan is textual, over the ART sources in <sources>/art, and finds candidates, not types: the checked list
runtime/art/codec-sites.txt says what each candidate is. A line of it is
    <kind> <file>::<function>#<n> <verdict> <the line's text, white space squeezed>
with <n> the ordinal of that kind in the function, so a line number moving does not rename a site. Kinds:
    H   a call of HeapOperand or HeapOperandFrom (its base is a heap reference, W register, by its DCHECK)
    M   a MemOperand or SVEMemOperand whose base is anything but sp, tr (the thread register) or xzr
    A   an address computation that adds to the X form of a register (Add(..., base.X(), ...))
    F   any other Add, Sub or ComputeAddress of the generators: where a raw address is formed from a base that may be a
        reference (data_offset + array), the other half of the M sites whose base is a "ptr" or "addr" register
    E   an assembly stub's `mov x0, x3|x4|\\xTemp0`: where a raw object address becomes the reference it returns
        (encode keeps the low 32 bits, which a W register already is: nop, unless a 64-bit slot keeps the reference)
    S   a load or store of an assembly stub (.S) whose base register is not sp, x19 (tr), x29 or x30
Verdicts: decode (the base is a reference), encode (a raw address kept as a 64-bit reference; the 32-bit case is
nop), dead (code only a read barrier emits), raw (a native address, the thread, a method, ...), derived (a base formed
from a reference at a decode site listed as F or A), nop (not an access or an address), todo (not yet checked).
The script prints the candidates (default), or with --check compares them with the list and fails for a site the
list lacks, a site it lists that is gone, or a verdict "todo". --write adds the new sites to the list as "todo" and
keeps the verdicts of the others. The list is what step c2.5's mutation check counts: every decode site, not a sample.

Usage: tools/art-codec-sites.py [--check | --write] [--list <file>] <sources-directory>
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIST = os.path.join(ROOT, "runtime", "art", "codec-sites.txt")
CXX = ["compiler/optimizing/code_generator_arm64.cc", "compiler/optimizing/intrinsics_arm64.cc",
       "compiler/optimizing/code_generator_vector_arm64_neon.cc",
       "compiler/optimizing/code_generator_vector_arm64_sve.cc",
       "compiler/optimizing/jit_patches_arm64.cc", "compiler/optimizing/instruction_simplifier_arm64.cc"]
ASM = ["runtime/arch/arm64/quick_entrypoints_arm64.S", "runtime/arch/arm64/jni_entrypoints_arm64.S",
       "runtime/arch/arm64/asm_support_arm64.S"]
SAFE_BASES = {"sp", "tr", "xzr", "vixl::aarch64::sp", "vixl::aarch64::xzr", "x19", "x29", "x30", "xSELF", "xFP",
              "xLR"}
FUNCTION = re.compile(r"^[A-Za-z_][^;=]*?([A-Za-z_~][\w:~<>]*)\s*\(")
NOT_FUNCTIONS = {"if", "for", "while", "switch", "return", "else", "namespace", "using", "template", "static_assert",
                 "DCHECK", "CHECK", "ALWAYS_INLINE", "DISALLOW_COPY_AND_ASSIGN", "UNREACHABLE", "LOG"}
ASM_FUNCTION = re.compile(r"^\s*(?:ENTRY|ENTRY_NO_GP|ONE_ARG_DOWNCALL|TWO_ARG_DOWNCALL|THREE_ARG_DOWNCALL|"
                          r"FOUR_ARG_DOWNCALL|ONE_ARG_REF_DOWNCALL|MACRO\d|\.macro|GENERATE_\w+)\s+([\w\.]+)")
MEM = re.compile(r"\b(?:SVE)?MemOperand\(\s*([^,()]+(?:\([^()]*\))?[^,()]*)")
ACCESS = re.compile(r"^\s*(?:ldr|ldp|ldar|ldxr|ldaxr|ldur|str|stp|stlr|stxr|stlxr|stur|ldrb|ldrh|ldrsw|strb|strh|"
                    r"cas\w*|swp\w*|ldadd\w*|ld\w+|st\w+)\s+[^\[]*\[\s*(\\?[a-z]\w*)", re.IGNORECASE)


def squeeze(text):
    return re.sub(r"\s+", " ", text.strip())


def cxx_sites(path, text):
    sites, counts, function = [], {}, "<file>"
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//")[0]
        match = FUNCTION.match(code) if code[:1].isalpha() and not code.rstrip().endswith(";") else None
        if match and match.group(1) not in NOT_FUNCTIONS and code.split()[0] not in NOT_FUNCTIONS:
            function = match.group(1)
        kinds = []
        if re.search(r"\bHeapOperand(?:From)?\(", code) and "inline " not in code:
            kinds.append("H")
        for mem in MEM.finditer(code):
            if mem.group(1).strip() not in SAFE_BASES:
                kinds.append("M")
        if re.search(r"\bAdd\([^;]*\.X\(\)", code) and "MemOperand" not in code:
            kinds.append("A")
        elif re.search(r"__ (?:Add|Sub|ComputeAddress)\(", code) and not re.search(r"__ (?:Add|Sub)\(\s*sp\b", code):
            kinds.append("F")
        for kind in kinds:
            index = counts.get((function, kind), 0)
            counts[(function, kind)] = index + 1
            sites.append((kind, f"{path}::{function}#{index + 1}", number, squeeze(code)))
    return sites


def asm_sites(path, text):
    sites, counts, function = [], {}, "<file>"
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//")[0]
        match = ASM_FUNCTION.match(code)
        if match:
            function = match.group(1)
        kinds = []
        access = ACCESS.match(code)
        if access and access.group(1) not in SAFE_BASES:
            kinds.append("S")
        if re.match(r"^\s*add\s+x\w+,\s*[\\\w]+,\s*#?MIRROR_", code):
            kinds.append("F")
        if re.match(r"^\s*mov\s+x0,\s*(?:x[34]|\\xTemp0)\b", code):
            kinds.append("E")
        for kind in kinds:
            index = counts.get((function, kind), 0)
            counts[(function, kind)] = index + 1
            sites.append((kind, f"{path}::{function}#{index + 1}", number, squeeze(code)))
    return sites


def scan(sources):
    sites = []
    for path in CXX + ASM:
        full = os.path.join(sources, "art", path)
        if not os.path.exists(full):
            raise SystemExit(f"art-codec-sites.py: {full} is missing")
        text = open(full, encoding="utf-8").read()
        sites += (asm_sites if path.endswith(".S") else cxx_sites)(path, text)
    return sites


def read_list(path):
    entries = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8").read().splitlines():
            if line and not line.startswith("#"):
                kind, key, verdict, text = (line.split(" ", 3) + [""])[:4]
                entries[(kind, key)] = (verdict, text)
    return entries


def main():
    args = sys.argv[1:]
    mode, listing = "print", LIST
    while args[:1] in (["--check"], ["--write"], ["--list"]):
        if args[0] == "--list":
            listing, args = args[1], args[2:]
        else:
            mode, args = args[0][2:], args[1:]
    if len(args) != 1:
        raise SystemExit(__doc__)
    sites = scan(args[0])
    if mode == "print":
        for kind, key, number, text in sites:
            print(f"{kind} {key} L{number} {text}")
        counts = {}
        for kind, *_ in sites:
            counts[kind] = counts.get(kind, 0) + 1
        print("# " + ", ".join(f"{kind} {count}" for kind, count in sorted(counts.items())), file=sys.stderr)
        return 0
    known = read_list(listing)
    scanned = {(kind, key): text for kind, key, _, text in sites}
    if mode == "write":
        lines = []
        for kind, key, _, text in sites:
            verdict = known.get((kind, key), ("todo", ""))[0]
            lines.append(f"{kind} {key} {verdict} {text}")
        header = ["# ART's arm64 compiler and stubs: where a heap reference becomes an address "
                  "(tools/art-codec-sites.py)."]
        open(listing, "w", encoding="utf-8").write("\n".join(header + lines) + "\n")
        return 0
    failures = [f"not in the list: {kind} {key}" for (kind, key) in scanned if (kind, key) not in known]
    failures += [f"gone from the sources: {kind} {key}" for (kind, key) in known if (kind, key) not in scanned]
    failures += [f"changed text: {kind} {key}" for (kind, key), text in scanned.items()
                 if (kind, key) in known and known[(kind, key)][1] != text]
    failures += [f"verdict todo: {kind} {key}" for (kind, key), (verdict, _) in known.items() if verdict == "todo"]
    for failure in failures[:50]:
        print(failure)
    if len(failures) > 50:
        print(f"... and {len(failures) - 50} more")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
