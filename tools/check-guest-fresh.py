#!/usr/bin/env python3
"""Fails when a guest output that a release packs is older than the sources it is built from (or is missing), so that
tools/release.sh never ships libraries left over from an older tree. Each output is a marker file the build writes; its
sources are the folders and files the build script reads. The marker's time must be at least that of the newest source
file. In a fresh clone every source is as new as the checkout, so a clone needs the full build of docs/BUILDING.md
before a release, which is what this asks for. A source folder that is not there is skipped; tests and docs are not
sources, and neither are files that no build reads (.DS_Store, README and *.md, editors' backups). Python 3.9.

Usage: tools/check-guest-fresh.py [--only <folder>]... [<repo root>]
  --only <folder>  checks only the outputs under that folder of the build (build/bionic, say), which a test of the guest
                   libraries uses to stop with the command that builds them again; several may be given, and a folder
                   that holds no output is an error (exit 2). APK_SKIP_GUEST_FRESH=1 makes the check pass, for a tree
                   whose sources are fresh copies (their times say nothing about the outputs it links to)."""
import argparse
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
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("root", nargs="?", default=here)
    arguments = parser.parse_args()
    root = os.path.abspath(arguments.root)
    if os.environ.get("APK_SKIP_GUEST_FRESH") == "1":
        print("check-guest-fresh: skipped (APK_SKIP_GUEST_FRESH=1)")
        return
    outputs = OUTPUTS
    if arguments.only:
        folders = [folder.rstrip("/") + "/" for folder in arguments.only]
        outputs = [output for output in OUTPUTS if output[0].startswith(tuple(folders))]
        missing = [folder for folder in folders if not any(output[0].startswith(folder) for output in OUTPUTS)]
        if missing:
            print("check-guest-fresh: no guest output lives in " + ", ".join(missing), file=sys.stderr)
            sys.exit(2)
    problems = []
    for marker, script, sources in outputs:
        path = os.path.join(root, marker)
        if not os.path.exists(path):
            problems.append(f"{marker} is missing; run {script}")
            continue
        time, source = newest(root, sources)
        if os.stat(path).st_mtime < time:
            problems.append(f"{marker} is older than {source}; run {script}")
    if problems:
        print("check-guest-fresh: the guest outputs are not those of this tree:", file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        sys.exit(1)
    print("check-guest-fresh: the guest outputs are newer than their sources")


main()
