#!/usr/bin/env python3
"""Fails when a guest output that a release packs is older than the sources it is built from (or is missing), so that
tools/release.sh never ships libraries left over from an older tree. Each output is a marker file the build writes; its
sources are the folders and files the build script reads. The marker's time must be at least that of the newest source
file. In a fresh clone every source is as new as the checkout, so a clone needs the full build of docs/BUILDING.md
before a release, which is what this asks for. A source folder that is not there is skipped; tests and docs are not
sources, and neither are files that no build reads (.DS_Store, README and *.md, editors' backups). Python 3.9.

Usage: tools/check-guest-fresh.py [<repo root>]"""
import os
import sys

# marker -> (what it is built by, the sources: folders are walked, files are taken as they are)
OUTPUTS = [
    ("build/bionic/lib/libc.so", "tools/build-bionic.sh",
     ["runtime/bionic", "runtime/android", "runtime/include", "tools/build-bionic.sh", "tools/prefix-map-flags.sh"]),
    ("build/art/lib/libart.so", "tools/build-art.sh",
     ["runtime/art", "tools/build-art.sh", "tools/prefix-map-flags.sh"]),
    ("build/framework/framework.jar", "tools/build-framework.sh",
     ["framework/java", "framework/managers", "framework/patches", "framework/stubs", "framework/sources.txt",
      "framework/stub-references.txt", "framework/stub-supertypes.txt", "framework/signature-gaps.txt",
      "tools/build-framework.sh"]),
    ("build/framework-native/libandroid_runtime.so", "tools/build-framework-native.sh",
     ["framework/native", "framework/CMakeLists.txt", "runtime/include", "tools/build-framework-native.sh",
      "tools/prefix-map-flags.sh"]),
    ("build/hwui/lib/libhwui.so", "tools/build-hwui.sh",
     ["graphics/hwui", "tools/build-hwui.sh", "tools/prefix-map-flags.sh"]),
    ("build/graphics/guest/bin/libEGL.so", "tools/build-front-libs.sh",
     ["graphics/guest", "graphics/egl", "graphics/gles", "graphics/vulkan", "runtime/include",
      "tools/build-front-libs.sh", "tools/prefix-map-flags.sh"]),
]


# Files that no build reads: the system's and editors' leftovers, notes and docs. One of them newer than the marker
# would fail the guard for good, since the build has nothing to redo. (A list of the suffixes the builds read would
# silently drop a kind of source nobody thought of; a list of what is no source can only err toward a false alarm.)
IGNORED_SUFFIXES = (".md", ".orig", ".rej", ".swp", ".tmp", ".bak", "~")


def is_source(name):
    return not name.startswith(".") and not name.startswith("README") and not name.endswith(IGNORED_SUFFIXES)


def newest(root, sources):
    """(time, path) of the newest source file under `root`."""
    best = (0.0, "")
    for source in sources:
        path = os.path.join(root, source)
        if os.path.isfile(path):
            candidates = [path]
        elif os.path.isdir(path):
            candidates = [os.path.join(folder, name) for folder, _, names in os.walk(path) for name in names
                          if is_source(name)]
        else:
            continue
        for candidate in candidates:
            try:
                time = os.stat(candidate).st_mtime
            except OSError:
                continue
            if time > best[0]:
                best = (time, os.path.relpath(candidate, root))
    return best


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else here
    problems = []
    for marker, script, sources in OUTPUTS:
        path = os.path.join(root, marker)
        if not os.path.exists(path):
            problems.append(f"{marker} is missing; {script} builds it")
            continue
        time, source = newest(root, sources)
        if os.stat(path).st_mtime < time:
            problems.append(f"{marker} is older than {source}; {script} builds it again")
    if problems:
        print("check-guest-fresh: the guest outputs are not those of this tree:", file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        sys.exit(1)
    print("check-guest-fresh: the guest outputs are newer than their sources")


main()
