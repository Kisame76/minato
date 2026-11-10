#!/usr/bin/env python3
"""tools/prepared-stamp.py, which lets a build step return at once when MINATO_PREPARED=1 and nothing it reads has
changed since the stamp was written. The step is skipped only when all of these hold: the variable is 1, the stamp for
its name exists, its marker output exists, and the digest of the inputs equals the stamp's. A changed, added or removed
input file, a missing marker, a changed stat-only input (size or time) and a stamp of another name each make the step
build as it always did. .DS_Store and other hidden files do not count as inputs. Python 3.9."""
import os
import re
import subprocess
import sys
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "prepared-stamp.py")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        print("FAIL", what)
        failures += 1


def run(root, *args, prepared="1"):
    env = dict(os.environ)
    env.pop("MINATO_PREPARED", None)
    if prepared is not None:
        env["MINATO_PREPARED"] = prepared
    return subprocess.run([sys.executable, TOOL, *args], cwd=root, env=env, capture_output=True, text=True)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


with tempfile.TemporaryDirectory() as root:
    write(os.path.join(root, "src/a.txt"), "one")
    write(os.path.join(root, "src/sub/b.txt"), "two")
    write(os.path.join(root, "single.sh"), "echo")
    write(os.path.join(root, "big.jar"), "jar")
    write(os.path.join(root, "out/marker"), "built")
    inputs = ["src", "single.sh", "--stat", "big.jar"]

    def digest():
        r = run(root, "compute", *inputs)
        check(r.returncode == 0 and len(r.stdout.strip()) == 64, "compute prints a SHA-256: " + r.stdout + r.stderr)
        return r.stdout.strip()

    def skips(name="fw", marker="out/marker", prepared="1"):
        return run(root, "check", name, marker, *inputs, prepared=prepared).returncode == 0

    d = digest()
    check(not skips(), "no stamp yet: build")
    check(run(root, "write", "fw", d).returncode == 0, "write stores the stamp")
    check(skips(), "stamp, marker and inputs unchanged: skip")
    check(not skips(prepared=None), "MINATO_PREPARED unset: build")
    check(not skips(prepared="0"), "MINATO_PREPARED=0: build")
    check(not skips(name="other"), "a stamp of another name: build")
    check(not skips(marker="out/missing"), "the marker output is missing: build")

    write(os.path.join(root, "src/.DS_Store"), "junk")
    write(os.path.join(root, "src/sub/.hidden"), "junk")
    check(skips(), "hidden files are no inputs: skip")

    write(os.path.join(root, "src/a.txt"), "one!")
    check(not skips(), "a changed input file: build")
    write(os.path.join(root, "src/a.txt"), "one")
    check(skips(), "the file back as it was: skip (content, not time)")

    write(os.path.join(root, "src/c.txt"), "new")
    check(not skips(), "an added input file: build")
    os.remove(os.path.join(root, "src/c.txt"))
    os.remove(os.path.join(root, "src/sub/b.txt"))
    check(not skips(), "a removed input file: build")
    write(os.path.join(root, "src/sub/b.txt"), "two")
    check(skips(), "restored: skip")

    write(os.path.join(root, "single.sh"), "echo changed")
    check(not skips(), "a changed single-file input: build")
    write(os.path.join(root, "single.sh"), "echo")

    os.utime(os.path.join(root, "big.jar"), (1, 1))
    check(not skips(), "a stat-only input with another time: build")
    check(run(root, "write", "fw", digest()).returncode == 0, "write again")
    check(skips(), "stamp renewed: skip")
    write(os.path.join(root, "big.jar"), "jar, longer")
    check(not skips(), "a stat-only input with another size: build")

    check(run(root, "check", "fw").returncode != 0, "a check without a marker is a usage error")
    check(run(root, "bogus").returncode != 0, "an unknown command is a usage error")

# The input lists of the real build steps: every declared input changes the digest, and the lists name what exists.
REPO = os.path.abspath(os.path.join(os.path.dirname(TOOL), ".."))
for script in ("build-framework.sh", "build-framework-native.sh"):
    env = dict(os.environ, MINATO_PRINT_INPUTS="1")
    printed = subprocess.run(["bash", os.path.join(REPO, "tools", script)], env=env, capture_output=True, text=True)
    declared = printed.stdout.split("\n")[:-1]
    check(printed.returncode == 0 and "--stat" in declared and len(declared) > 10, script + " prints its input list")
    at = declared.index("--stat") if "--stat" in declared else len(declared)
    check(len(set(declared)) == len(declared), script + ": an input is listed twice")
    for entry in declared[:at]:  # our own files: they must exist in the tree
        check(os.path.exists(os.path.join(REPO, entry)), script + ": content input " + entry + " does not exist")
    for entry in declared:
        if entry == "--stat":
            continue
        is_stat = declared.index(entry) > at
        with tempfile.TemporaryDirectory() as box:
            target = os.path.join(box, "x", entry.lstrip("/").replace("..", "_"))
            write(os.path.join(box, "keep.txt"), "k")
            write(os.path.join(target, "file") if entry.endswith("/") or "." not in os.path.basename(entry)
                  else target, "before")
            local = os.path.relpath(target, box)
            args = ["keep.txt", "--stat", local] if is_stat else [local, "keep.txt"]
            before = run(box, "compute", *args).stdout
            leaf = target if os.path.isfile(target) else os.path.join(target, "file")
            write(leaf, "after, longer")
            changed = run(box, "compute", *args).stdout
            check(before != changed, script + ": changing a file of " + entry + " keeps the digest")
            os.remove(leaf)
            gone = run(box, "compute", *args).stdout
            check(gone not in (before, changed), script + ": removing " + entry + " keeps the digest")

# Completeness: every folder of build/sources that a step names, and that framework/sources.txt takes from, is declared.
declared_sources = {}
for script in ("build-framework.sh", "build-framework-native.sh"):
    env = dict(os.environ, MINATO_PRINT_INPUTS="1")
    out = subprocess.run(["bash", os.path.join(REPO, "tools", script)], env=env, capture_output=True, text=True).stdout
    declared_sources[script] = {line.split("/")[2] for line in out.split("\n") if line.startswith("build/sources/")}
with open(os.path.join(REPO, "tools", "build-framework.sh")) as f:
    text = f.read()
named = set(re.findall(r"\$SOURCES/([A-Za-z0-9_]+)", text)) | set(re.findall(r"build/sources/([A-Za-z0-9_]+)", text))
with open(os.path.join(REPO, "framework", "sources.txt")) as f:
    for line in f:
        line = line.split("#")[0].strip()
        if line:
            named.add(line.split("/")[0])
for folder in sorted(named - declared_sources["build-framework.sh"] - {"build", "bin"}):
    check(False, "build-framework.sh reads build/sources/" + folder + " but does not declare it")

if failures:
    sys.exit(1)
print("prepared_stamp_test: ok")
