#!/usr/bin/env python3
"""tools/framework-check.sh's guard of run-time references (ADR-0039 step c2a): reads javap -c -p's output of
framework.jar's classes on stdin and finds each reference from a class's code to a class that is neither in
framework.jar nor in the boot jars, a compile-only stub, which ART cannot resolve at run time. A reference is a
`new`, a call, a field, a checkcast or instanceof (both throw NoClassDefFoundError even for null, c2's measurement), a
class constant, an array of it, a catch type, or the interface a lambda implements.

framework/stub-references.txt lists the intended ones: "<stub> <- <class>, <class>..." with framework.jar's classes by
their top-level name, or "<stub> <- *" for every class, each line with why after "#": the step that takes or answers
the stub ("step c2c: ...", "ADR-0041 a): ...", "M4 step 10: ..."), "stays: <why its code never runs on Minato's path>",
or "gap: <what>", an app's path that fails today without a step yet. Prints "unlisted <class> -> <stub>
(<method>: <instructions>)" for a reference no line covers, "stale <class> -> <stub>" for a listed pair that no longer
holds ("stale * -> <stub>" when nothing refers to the stub), and "unexplained <stub> <- <classes>" for a line without
its why.

Usage: javap -c -p ... | stub_references.py <framework classes.txt> <boot classes.txt> <allowlist>
"""
import re
import sys

CLASS_LINE = re.compile(r"^(?:[a-z-]+ )*(?:class|interface) (\S+?)(?:<.*?>)?(?: extends .*?)?(?: implements .*)? \{$")
METHOD_LINE = re.compile(r"^  (?:\S.*\(.*\)(?: throws [^;]*)?|static \{\});$")
INSTRUCTION = re.compile(r"^\s+\d+: (\w+)\s.*?// (class|Method|InterfaceMethod|Field|InvokeDynamic) (.*)$")
CATCH = re.compile(r"^\s+\d+\s+\d+\s+\d+\s+Class (\S+)$")
REASON = re.compile(r"^(step \S+|ADR-\d{4} \S+|M\d+ step \d+|stays|gap): *\S")


def element(name):
    """The class of a type name or descriptor, None for a primitive or a primitive array."""
    name = name.strip('"').lstrip("[")
    if name.startswith("L") and name.endswith(";"):
        return name[1:-1]
    return None if len(name) == 1 else name


def referenced(kind, rest):
    """The class an instruction's constant names, None for the class's own members, arrays and primitives."""
    if kind == "class":
        return element(rest.split()[0])
    if kind == "InvokeDynamic":
        descriptor = rest.split(":", 2)[-1]
        returned = descriptor[descriptor.rindex(")") + 1:]
        return element(returned) if returned.startswith("L") else None
    head = rest.split(":")[0]
    # javap leaves the owner out for the class's own members.
    if "." not in head.replace('"<init>"', "").replace('"<clinit>"', ""):
        return None
    owner = head.rsplit(".", 1)[0]
    # A call on an array type ("[Ljava/lang/Object;".clone) has no class of its own.
    return None if owner.startswith('"[') else owner


def references(lines, known):
    """{(top-level class, stub): {"<method>: <instructions>"}} from javap -c -p's lines."""
    found = {}
    current = method = None
    for line in lines:
        line = line.rstrip("\n")
        match = CLASS_LINE.match(line)
        if match:
            current, method = match.group(1), None
            continue
        if METHOD_LINE.match(line):
            name = re.search(r"([\w$<>]+)\(", line)
            method = name.group(1) if name else "<clinit>"
            continue
        stub = None
        match = INSTRUCTION.match(line)
        if match:
            instruction, kind, rest = match.groups()
            stub = referenced(kind, rest)
        else:
            match = CATCH.match(line)
            if match:
                instruction, stub = "catch", match.group(1)
        if stub is None or current is None or stub in known:
            continue
        top = current.split("$")[0]
        sites = found.setdefault((top, stub.replace("/", ".")), {})
        sites.setdefault(f"{current.rsplit('.', 1)[-1]}.{method}", set()).add(instruction)
    return found


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    known = set(open(sys.argv[1]).read().split()) | set(open(sys.argv[2]).read().split())
    allowed, unexplained = {}, []
    for line in open(sys.argv[3]):
        entry, _, why = line.partition("#")
        if not entry.strip():
            continue
        stub, _, users = entry.partition("<-")
        stub = stub.strip()
        names = {name.strip() for name in users.split(",") if name.strip()} or {"*"}
        allowed.setdefault(stub, set()).update(names)
        if not REASON.match(why.strip()):
            unexplained.append(f"{stub} <- {', '.join(sorted(names))}")
    found = references(sys.stdin, known)
    for (top, stub), sites in sorted(found.items()):
        users = allowed.get(stub, set())
        if "*" not in users and top not in users:
            where = "; ".join(f"{site}: {','.join(sorted(ops))}" for site, ops in sorted(sites.items()))
            print(f"unlisted {top} -> {stub} ({where})")
    referring = {}
    for top, stub in found:
        referring.setdefault(stub, set()).add(top)
    for stub, users in sorted(allowed.items()):
        for user in sorted(users):
            if user == "*" and not referring.get(stub) or user != "*" and user not in referring.get(stub, ()):
                print(f"stale {user} -> {stub}")
    for entry in unexplained:
        print(f"unexplained {entry}")


if __name__ == "__main__":
    main()
