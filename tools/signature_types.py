#!/usr/bin/env python3
"""tools/framework-check.sh's guard of member signatures (retest 8's batch B3): reads javap -p -s's output of
framework.jar's classes on stdin and finds each field type, parameter type and return type that names a class neither
framework.jar nor the boot jars hold, a compile-only stub or an absent type. ART resolves the signature of every
declared member when an app calls getDeclaredMethods, getDeclaredFields or getDeclaredConstructors, so one such type
makes the class unreadable by reflection (NoClassDefFoundError), and with it every library that reflects over a Context,
an Activity or a View (the Unity test APK's retest 9: 70 classes).

framework/signature-gaps.txt lists the intended ones: "<type> <- <class>, <class>..." with framework.jar's classes by
their top-level name, or "<type> <- *" for every class, each line with why after "#": the step that takes the type
("step b3: ..."), "stays: <why no app reflects over the class>", or "gap: <what>", a class that stays unreadable today
without a step yet. Prints "unlisted <class> -> <type> (<member>)" for a pair no line covers, "stale <class> -> <type>"
for a listed pair that no longer holds ("stale * -> <type>" when nothing names the type), and "unexplained <type> <-
<classes>" for a line without its why.

Usage: javap -p -s ... | signature_types.py <framework classes.txt> <boot classes.txt> <allowlist> [--lines]

With --lines it prints only the unlisted pairs, grouped by type as the allowlist's lines, "<type> <- <classes>  # FILL IN
WHY": replace that with the reason (step, stays or gap), which the tool reports as unexplained until then.
"""
import re
import sys

CLASS_LINE = re.compile(r"^(?:[a-z-]+ )*(?:class|interface) (\S+?)(?:<.*?>)?(?: extends .*?)?(?: implements .*)? \{$")
MEMBER_LINE = re.compile(r"^  (\S.*);$")
DESCRIPTOR = re.compile(r"^    descriptor: (\S+)$")
REASON = re.compile(r"^(step \S+|ADR-\d{4} \S+|M\d+ step \d+|stays|gap): *\S")


def member_name(line):
    """The name of a member from javap's declaration: a method's before its "(", a field's last word, a constructor's
    class name."""
    if "(" in line:
        return re.search(r"([\w$<>]+)\(", line).group(1).rsplit(".", 1)[-1].rsplit("$", 1)[-1]
    return line.split()[-1]


def names(lines, known):
    """{(top-level class, type): {member}} from javap -p -s's lines."""
    found = {}
    current = member = None
    for line in lines:
        line = line.rstrip("\n")
        match = CLASS_LINE.match(line)
        if match:
            current, member = match.group(1), None
            continue
        match = MEMBER_LINE.match(line)
        if match:
            member = member_name(match.group(1))
            continue
        match = DESCRIPTOR.match(line)
        if not match or current is None:
            continue
        for each in re.findall(r"L([^;]+);", match.group(1)):
            if each in known:
                continue
            found.setdefault((current.split("$")[0], each.replace("/", ".")), set()).add(member)
    return found


def main():
    lines_only = sys.argv[4:] == ["--lines"]
    if len(sys.argv) != 4 and not lines_only:
        raise SystemExit(__doc__)
    known = set(open(sys.argv[1]).read().split()) | set(open(sys.argv[2]).read().split())
    allowed, unexplained = {}, []
    for line in open(sys.argv[3]):
        entry, _, why = line.partition("#")
        if not entry.strip():
            continue
        missing, _, users = entry.partition("<-")
        missing = missing.strip()
        classes = {name.strip() for name in users.split(",") if name.strip()} or {"*"}
        allowed.setdefault(missing, set()).update(classes)
        if not REASON.match(why.strip()):
            unexplained.append(f"{missing} <- {', '.join(sorted(classes))}")
    found = names(sys.stdin, known)
    if lines_only:
        unlisted = {}
        for (top, missing) in found:
            if "*" not in allowed.get(missing, set()) and top not in allowed.get(missing, set()):
                unlisted.setdefault(missing, set()).add(top)
        for missing, users in sorted(unlisted.items()):
            print(f"{missing} <- {', '.join(sorted(users))}  # FILL IN WHY")
        return
    for (top, missing), members in sorted(found.items()):
        users = allowed.get(missing, set())
        if "*" not in users and top not in users:
            print(f"unlisted {top} -> {missing} ({', '.join(sorted(members))})")
    naming = {}
    for top, missing in found:
        naming.setdefault(missing, set()).add(top)
    for missing, users in sorted(allowed.items()):
        for user in sorted(users):
            if user == "*" and not naming.get(missing) or user != "*" and user not in naming.get(missing, ()):
                print(f"stale {user} -> {missing}")
    for entry in unexplained:
        print(f"unexplained {entry}")


if __name__ == "__main__":
    main()
