#!/usr/bin/env python3
"""The guest builds plan their trees when CMake configures them: runtime/bionic with tools/bionic-plan.py (ADR-0021),
runtime/art with tools/art-plan.py (ADR-0033 a). A change to what a plan comes from configures the build again:
  - the planner and every module of tools/ it imports, directly or through another, are inputs of build.ninja's
    regeneration;
  - each build globs the build's overlay folder again (CMakeFiles/VerifyGlobs.cmake), so that a file that comes or
    goes there configures it again.
Each build is read as it was configured, against its own source tree (CMakeCache.txt), with ninja's query, which only
reads. A build with a source input newer than its build.ninja is not checked: its next build configures it again.
Prints "skipped: ..." when no build is checked.

Usage: plan_inputs_test.py <build>...
"""
import ast
import os
import re
import subprocess
import sys

# The planner of each source folder whose build plans its tree.
PLANNERS = {"bionic": "bionic-plan.py", "art": "art-plan.py"}


def cached(build, name):
    """The value of `name` in the CMakeCache.txt of `build`."""
    with open(os.path.join(build, "CMakeCache.txt")) as file:
        for line in file:
            if line.startswith(name + ":"):
                return line.split("=", 1)[1].rstrip("\n")
    raise SystemExit(f"{build}: CMakeCache.txt has no {name}")


def modules(path, tools, found):
    """Adds `path` and the modules of `tools` it imports, directly or through another, to `found`."""
    if path in found:
        return
    found.add(path)
    with open(path) as file:
        tree = ast.parse(file.read(), path)
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names = [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names = [node.module]
        else:
            continue
        for name in names:
            module = os.path.join(tools, name.split(".")[0] + ".py")
            if os.path.isfile(module):
                modules(module, tools, found)


def regeneration_inputs(build):
    """The inputs of build.ninja's own edge, CMake's regeneration, as real paths. -n keeps ninja from writing its
    logs, as it would to compact them."""
    ninja = cached(build, "CMAKE_MAKE_PROGRAM")
    result = subprocess.run([ninja, "-n", "-C", build, "-t", "query", "build.ninja"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"{build}: ninja -t query failed: {result.stderr.strip()}")
    inputs, listing = set(), False
    for line in result.stdout.splitlines():
        entry = line.strip()
        if entry.startswith("input:"):
            listing = True
        elif entry.startswith("outputs:"):
            listing = False
        elif listing and entry:
            inputs.add(os.path.realpath(os.path.join(build, entry.lstrip("|").strip())))
    return inputs


def globbed(build, inputs):
    """The patterns that each build of `build` globs again, as real paths: those of CMakeFiles/VerifyGlobs.cmake while
    the regeneration depends on it. CMake leaves the file behind when a configuration no longer globs."""
    path = os.path.join(build, "CMakeFiles", "VerifyGlobs.cmake")
    if path not in inputs:
        return set()
    with open(path) as file:
        patterns = re.findall(r'^file\(GLOB(?:_RECURSE)? NEW_GLOB .*"([^"]*)"\)$', file.read(), re.MULTILINE)
    return {os.path.realpath(pattern) for pattern in patterns}


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    checked, notes, failures = [], [], []
    for build in (os.path.realpath(argument) for argument in sys.argv[1:]):
        manifest = os.path.join(build, "build.ninja")
        if not os.path.exists(manifest):
            notes.append(f"{build} is not configured")
            continue
        source = os.path.realpath(cached(build, "CMAKE_HOME_DIRECTORY"))
        name = os.path.basename(source)
        if name not in PLANNERS:
            raise SystemExit(f"{build}: {source} has no planner")
        root = os.path.dirname(os.path.dirname(source))
        inputs = regeneration_inputs(build)
        newer = sorted(path for path in inputs if not path.startswith(build + os.sep) and os.path.exists(path)
                       and os.path.getmtime(path) > os.path.getmtime(manifest))
        if newer:
            notes.append(f"{build} is out of date: {os.path.relpath(newer[0], root)} is newer than its build.ninja")
            continue
        tools = os.path.join(root, "tools")
        found = set()
        modules(os.path.join(tools, PLANNERS[name]), tools, found)
        for module in sorted(found - inputs):
            failures.append(f"{name}: {os.path.relpath(module, root)} is no input of its configuration, "
                            "so a change to it does not plan again")
        overlay = os.path.join(source, "overlay")
        if os.path.isdir(overlay) and os.path.join(overlay, "*") not in globbed(build, inputs):
            failures.append(f"{name}: {os.path.relpath(overlay, root)} is not globbed, "
                            "so a file that comes or goes there does not plan again")
        checked.append(name)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        for note in notes:
            print(f"not checked: {note}")
        sys.exit(1)
    if not checked:
        print("skipped: " + "; ".join(notes))
        return
    print(f"{' and '.join(checked)} plan again when the planner or a module it imports changes, or a file comes into "
          "the overlay folder or leaves it" + "".join(f"; not checked: {note}" for note in notes))


if __name__ == "__main__":
    main()
