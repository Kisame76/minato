#!/usr/bin/env python3
"""The release guards of tools/check-guest-fresh.py and tools/release.sh, CTest's release.guest_fresh, on a fake tree:
  - guest outputs newer than their sources pass;
  - a source file newer than its output fails, and so does a missing output;
  - a stray file that no build reads (.DS_Store, a README, an editor's backup) newer than the output does not fail the
    guard for good, since ninja would have nothing to rebuild;
  - release.sh runs the guard before it makes the release commit, so that a failing guard leaves no commit behind that a
    rerun refuses ("main is not origin/main").
Runs on /usr/bin/python3 (3.9)."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUARD = os.path.join(ROOT, "tools", "check-guest-fresh.py")
failures = []


def check(condition, what):
    print(("ok      " if condition else "FAILED  ") + what, flush=True)
    if not condition:
        failures.append(what)


def touch(path, time):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not os.path.exists(path):
        open(path, "w").close()
    os.utime(path, (time, time))


def guard(tree):
    return subprocess.run([sys.executable, "-I", GUARD, tree], capture_output=True, text=True).returncode


def fake_tree(base):
    """Every marker at time 1000 and every source file at 500: all fresh."""
    spec = subprocess.run([sys.executable, "-I", "-c", """
import runpy, sys, os
src = open(sys.argv[1]).read().replace("\\nmain()\\n", "\\n")
ns = {}
exec(compile(src, sys.argv[1], "exec"), ns)
for marker, script, sources in ns["OUTPUTS"]:
    print(marker + "\\t" + "\\t".join(sources))
""", GUARD], capture_output=True, text=True).stdout.splitlines()
    sources = {}
    for line in spec:
        marker, *paths = line.split("\t")
        touch(os.path.join(base, marker), 1000)
        for path in paths:
            full = os.path.join(base, path)
            if os.path.splitext(path)[1]:
                touch(full, 500)
            else:
                touch(os.path.join(full, "source.cpp"), 500)
        sources[marker] = paths
    return sources


with tempfile.TemporaryDirectory() as base:
    sources = fake_tree(base)
    check(guard(base) == 0, "outputs newer than their sources pass")
    folder = next(path for paths in sources.values() for path in paths if not os.path.splitext(path)[1])
    for junk in (".DS_Store", "README.md", "notes.md", "source.cpp~", ".source.cpp.swp", "source.cpp.orig"):
        touch(os.path.join(base, folder, junk), 2000)
        check(guard(base) == 0, f"a stray {junk} newer than the outputs does not fail the guard")
        os.remove(os.path.join(base, folder, junk))
    touch(os.path.join(base, folder, "source.cpp"), 2000)
    check(guard(base) == 1, "a source file newer than an output fails")
    touch(os.path.join(base, folder, "source.cpp"), 500)
    for suffix in (".c", ".cc", ".h", ".hpp", ".java", ".aidl", ".txt", ".patch", ".S", ".inc", ".json", ".xml", ".py"):
        name = "input" + suffix
        touch(os.path.join(base, folder, name), 2000)
        check(guard(base) == 1, f"a {suffix} file newer than an output fails")
        os.remove(os.path.join(base, folder, name))
    check(guard(base) == 0, "fresh again")
    os.remove(os.path.join(base, "build/art/lib/libart.so"))
    check(guard(base) == 1, "a missing output fails")

release = open(os.path.join(ROOT, "tools", "release.sh")).read()
guard_at = release.index("check-guest-fresh.py")
commit_at = release.index('git commit -q -m "chore: release')
check(guard_at < commit_at, "release.sh runs the guest guard before the release commit")

print("guest-fresh-test: " + ("all fine" if not failures else f"{len(failures)} failed"))
sys.exit(1 if failures else 0)
