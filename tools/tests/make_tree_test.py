#!/usr/bin/env python3
"""tools/soong_plan.py's make_tree keeps builds current when what a tree path holds goes back to its plain original.
ninja follows the tree's links, so a link back to an original that is older than the objects built from the path's
last content would leave those objects as they are (found by Runtime, 2026-10-02, while building ART's mutants).

On a made-up set of sources whose files are older than the build:
- a file whose patch goes, one whose host side goes and one whose overlay file goes each become a real copy of the
  original, newer than the build;
- a file that was never patched stays a link, also when its project moves to another folder, so the tree does not grow;
- a second run touches nothing;
- a kept copy follows its original when that changes, and an overlay file that comes again replaces the copy.
The host-side case needs /usr/bin/unifdef and is left out without it.
Usage: make_tree_test.py"""
import os
import shutil
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import soong_plan  # noqa: E402

failures = 0

OLD = 1_767_225_600 * 10**9  # 2026-01-01, the sources' time
HOSTED = "#ifdef __ANDROID__\nint device;\n#else\nint host;\n#endif\n"
PATCH = """--- a/p/patched.c
+++ b/p/patched.c
@@ -1 +1 @@
-int patched = 1;
+int patched = 2;
"""


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def write(path, text, when=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(text)
    if when is not None:
        os.utime(path, ns=(when, when))


def read(path):
    with open(path) as handle:
        return handle.read()


def main():
    unifdef = os.path.exists("/usr/bin/unifdef")
    if not unifdef:
        print("note: /usr/bin/unifdef is missing, so the host-side case is left out")
    scratch = tempfile.mkdtemp()
    try:
        sources = os.path.join(scratch, "sources", "p")
        for name, text in (("kept.c", "int kept = 1;\n"), ("patched.c", "int patched = 1;\n"),
                           ("overlaid.c", "int overlaid = 1;\n"), ("hosted.c", HOSTED)):
            write(os.path.join(sources, name), text, OLD)
        overlay = os.path.join(scratch, "overlay")
        write(os.path.join(overlay, "p", "overlaid.c"), "int overlaid = 2;\n")
        patches = os.path.join(scratch, "patches")
        write(os.path.join(patches, "0001-patched.patch"), PATCH)
        tree = os.path.join(scratch, "tree")
        projects = {"p": sources}
        host_side = ["p"] if unifdef else []
        names = ["kept.c", "patched.c", "overlaid.c"] + (["hosted.c"] if unifdef else [])

        def path(name):
            return os.path.join(tree, "p", name)

        def plan(projects=projects, host_side=host_side):
            problems = soong_plan.make_tree(tree, projects, overlay, patches, host_side)
            check(not problems, f"no problems: {problems}")

        def is_copy(name, text):
            return not os.path.islink(path(name)) and read(path(name)) == text

        plan()
        check(os.path.islink(path("kept.c")) and os.readlink(path("kept.c")) == os.path.join(sources, "kept.c"),
              "kept.c is a link to its source")
        check(is_copy("patched.c", "int patched = 2;\n"), "patched.c is a copy with its patch")
        check(os.path.islink(path("overlaid.c")) and read(path("overlaid.c")) == "int overlaid = 2;\n",
              "overlaid.c is a link to its overlay file")
        if unifdef:
            check(is_copy("hosted.c", "int host;\n"), "hosted.c is a copy with its host side")

        # The build: its objects are newer than the tree's copies, which are newer than the sources.
        built = time.time_ns() - 10**9
        for name in names:
            if not os.path.islink(path(name)):
                os.utime(path(name), ns=(built - 10**9, built - 10**9))

        os.remove(os.path.join(patches, "0001-patched.patch"))
        os.remove(os.path.join(overlay, "p", "overlaid.c"))
        plan(host_side=[])
        for name, text in (("patched.c", "int patched = 1;\n"), ("overlaid.c", "int overlaid = 1;\n"),
                           ("hosted.c", HOSTED)):
            if name not in names:
                continue
            check(is_copy(name, text), f"{name} becomes a real copy of its original")
            check(os.stat(path(name)).st_mtime_ns > built, f"{name} is newer than the build, so ninja builds it again")
        check(os.path.islink(path("kept.c")), "kept.c stays a link")

        before = {name: (os.lstat(path(name)).st_ino, os.lstat(path(name)).st_mtime_ns) for name in names}
        plan(host_side=[])
        after = {name: (os.lstat(path(name)).st_ino, os.lstat(path(name)).st_mtime_ns) for name in names}
        check(before == after, "a second run touches nothing")

        write(os.path.join(sources, "patched.c"), "int patched = 3;\n", OLD)
        plan(host_side=[])
        check(is_copy("patched.c", "int patched = 3;\n"), "a kept copy follows its original")
        write(os.path.join(overlay, "p", "overlaid.c"), "int overlaid = 4;\n")
        plan(host_side=[])
        check(read(path("overlaid.c")) == "int overlaid = 4;\n", "an overlay file that comes again replaces the copy")

        moved = os.path.join(scratch, "moved", "p")
        shutil.copytree(sources, moved)
        plan(projects={"p": moved}, host_side=[])
        check(os.path.islink(path("kept.c")) and os.readlink(path("kept.c")) == os.path.join(moved, "kept.c"),
              "kept.c stays a link when its project moves to another folder")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print(f"make-tree: {'all checks passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
