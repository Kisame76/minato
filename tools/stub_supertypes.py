#!/usr/bin/env python3
"""tools/framework-check.sh's guard (ADR-0039 b3.4): reads javap's class lines of framework.jar's classes on stdin and
prints each class whose superclass or interface is neither in framework.jar nor in the boot jars, as
"<class> -> <supertype>", against the allowlist framework/stub-supertypes.txt: "unloadable ..." for a pair not on it,
"stale ..." for an entry that no longer holds. A stub supertype compiles but is missing at run time, so the class
cannot load.

Usage: javap ... | stub_supertypes.py <framework classes.txt> <boot classes.txt> <allowlist>
"""
import re
import sys


def known_name(name, known):
    """javap prints a nested type as Outer.Inner, the jar holds Outer$Inner: try each split."""
    parts = name.split(".")
    for split in range(len(parts), 0, -1):
        candidate = "/".join(parts[:split]) + "".join("$" + part for part in parts[split:])
        if candidate in known:
            return True
    return False


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    known = set(open(sys.argv[1]).read().split()) | set(open(sys.argv[2]).read().split())
    allowed = set()
    for line in open(sys.argv[3]):
        line = line.split("#", 1)[0].strip()
        if line:
            allowed.add(" ".join(line.split()))
    found = set()
    for line in sys.stdin:
        match = re.match(r"^[a-z ]*(?:class|interface) (\S+)(?: extends ([^{]*?))?(?: implements ([^{]*))? \{", line)
        if not match:
            continue
        supertypes = ",".join(filter(None, match.group(2, 3)))
        while "<" in supertypes:
            supertypes = re.sub(r"<[^<>]*>", "", supertypes)
        for supertype in filter(None, (part.strip() for part in supertypes.split(","))):
            if not known_name(supertype, known):
                found.add(f"{match.group(1)} -> {supertype}")
    for pair in sorted(found - allowed):
        print(f"unloadable {pair}")
    for pair in sorted(allowed - found):
        print(f"stale {pair}")


if __name__ == "__main__":
    main()
