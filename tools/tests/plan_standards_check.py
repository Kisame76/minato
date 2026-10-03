#!/usr/bin/env python3
"""Every C and C++ compile of a configured guest build takes the standard that its plan names (ADR-0043 a2): the last
-std of the unit's CFLAGS and CONLYFLAGS, or CFLAGS and CPPFLAGS, which tools/soong_plan.py puts after the module's
cflags, where Soong puts it. It reads the build's plan.cmake and the compile commands CMake wrote into build.ninja, so
it also catches a standard of the CMake files' own, or one that CMake drops as a repeated option.
A build that is not configured is not checked, nor one with an input of its configuration newer than its build.ninja
(ninja's query, which only reads): its next build plans again. With none checked, the check is skipped.
Usage: plan_standards_check.py <build directory>...  (graphics/hwui's and runtime/art's builds)"""
import os
import re
import subprocess
import sys

COMPILE = re.compile(r"^build [^:]+: (C|CXX)_COMPILER__(\S+)_unscanned_\w+ (\S+)")
LANGUAGES = {".c": ("C", "CONLYFLAGS"), ".cpp": ("CXX", "CPPFLAGS"), ".cc": ("CXX", "CPPFLAGS")}


def values(text):
    """A field's values, unquoted, without a SHELL: prefix."""
    result = []
    for value in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
        value = value.replace('\\"', '"').replace("\\;", ";").replace("\\\\", "\\")
        result.append(value[len("SHELL:"):] if value.startswith("SHELL:") else value)
    return result


def standard(flags):
    return next((flag[len("-std="):] for flag in reversed(flags) if flag.startswith("-std=")), None)


def cached(build, name):
    """A variable's value in the build's CMakeCache.txt, or None."""
    with open(os.path.join(build, "CMakeCache.txt")) as handle:
        for line in handle:
            if line.startswith(name + ":"):
                return line.split("=", 1)[1].rstrip("\n")
    return None


def planned(build):
    """{(target, source): (language, the standard the plan names)} for every C and C++ source of the plan's units.
    A generated source is named as build.ninja names it: in the generated folder of the build directory that CMake
    configured, however the build is given here."""
    generated = os.path.join(cached(build, "CMAKE_CACHEFILE_DIR") or os.path.abspath(build), "generated")
    with open(os.path.join(build, "plan.cmake")) as handle:
        text = handle.read()
    result = {}
    for match in re.finditer(r"^\w+_unit\((\S+)\n(.*?)\)$", text, re.M | re.S):
        fields = {}
        for line in match.group(2).splitlines():
            key, _, rest = line.strip().partition(" ")
            fields[key] = values(rest)
        for source in fields.get("SOURCES", []):
            source = re.sub(r"\$\{APK_\w+_GENERATED\}", lambda _: generated, source)
            language = LANGUAGES.get(os.path.splitext(source)[1])
            if language:
                result[(match.group(1), source)] = (language[0], standard(fields.get("CFLAGS", [])
                                                                          + fields.get(language[1], [])))
    return result


def compiled(build):
    """{(target, source): (language, the last -std of its compile command)} from build.ninja."""
    result, current = {}, None
    with open(os.path.join(build, "build.ninja")) as handle:
        for line in handle:
            match = COMPILE.match(line)
            if match:
                current = (match.group(2), match.group(3))
                result[current] = (match.group(1), None)
            elif current and line.startswith("  FLAGS = "):
                result[current] = (result[current][0], standard(line.split()))
            elif not line.startswith("  "):
                current = None
    return result


def newer_input(build):
    """An input of build.ninja's own edge, CMake's regeneration, outside the build and newer than build.ninja, or
    None."""
    ninja = cached(build, "CMAKE_MAKE_PROGRAM") or "ninja"
    result = subprocess.run([ninja, "-n", "-C", build, "-t", "query", "build.ninja"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"{build}: ninja -t query failed: {result.stderr.strip()}")
    manifest, listing = os.path.getmtime(os.path.join(build, "build.ninja")), False
    for line in result.stdout.splitlines():
        entry = line.strip()
        if entry.startswith("input:"):
            listing = True
        elif entry.startswith("outputs:"):
            listing = False
        elif listing and entry:
            path = os.path.realpath(os.path.join(build, entry.lstrip("|").strip()))
            if (not path.startswith(os.path.realpath(build) + os.sep) and os.path.exists(path)
                    and os.path.getmtime(path) > manifest):
                return path
    return None


def check(build):
    """Problems of one build, one per unit and kind with its first file, and the compiles: checked and wrong."""
    plan, ninja = planned(build), compiled(build)
    groups = {}
    for key, (language, wanted) in sorted(plan.items()):
        target, source = key
        if wanted is None:
            problem = f"the plan names no {language} standard"
        elif key not in ninja:
            problem = "build.ninja does not compile them"
        elif ninja[key] != (language, wanted):
            problem = f"they compile as {ninja[key][0]} {ninja[key][1]}, the plan names {language} {wanted}"
        else:
            continue
        groups.setdefault((target, problem), []).append(os.path.basename(source))
    problems = [f"{target}: {len(files)} files, {problem} ({files[0]}{', ...' if len(files) > 1 else ''})"
                for (target, problem), files in groups.items()]
    return problems, len(plan), sum(len(files) for files in groups.values())


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    notes, failures, checked = [], 0, 0
    for build in sys.argv[1:]:
        if not all(os.path.isfile(os.path.join(build, name)) for name in ("plan.cmake", "build.ninja",
                                                                          "CMakeCache.txt")):
            notes.append(f"{build} is not configured")
            continue
        newer = newer_input(build)
        if newer:
            notes.append(f"{build} is out of date: {newer} is newer than its build.ninja")
            continue
        problems, count, wrong = check(build)
        for problem in problems:
            print(f"FAIL {problem}")
        failures += wrong
        checked += 1
        print(f"plan-standards: {build}: {count - wrong} of {count} compiles take the plan's standard")
    if not checked:
        print("skipped: " + "; ".join(notes))
        return 0
    for note in notes:
        print(f"not checked: {note}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
