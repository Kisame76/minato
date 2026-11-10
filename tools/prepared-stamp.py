#!/usr/bin/env python3
"""Lets a build step return at once when it was built already and nothing it reads has changed (MINATO_PREPARED=1).
The checks (tools/*-check.sh) call tools/build-framework.sh before every run, which takes more than a minute even when
nothing changed; a lead check that runs many of them builds once and exports MINATO_PREPARED=1 for the rest.

  prepared-stamp.py compute <input>... [--stat <input>...]
      prints the SHA-256 of the inputs: for every file below a folder (or the file itself) its path and bytes, hidden
      files left out; after --stat only each file's size and modification time, for inputs too big to read each time
      (the boot jars, aidl).
  prepared-stamp.py check <name> <marker> <input>... [--stat <input>...]
      exit 0 (skip the build) only when MINATO_PREPARED is 1, build/prepared/<name>.stamp exists, <marker> exists and
      the digest of the inputs equals the stamp; exit 1 otherwise, and the step builds as it always did.
  prepared-stamp.py write <name> <digest>
      stores the digest as build/prepared/<name>.stamp. A step computes the digest BEFORE it builds and writes it after
      the build succeeded, so an input changed meanwhile leaves a stamp that does not match.

Run from the repository root. Python 3.9."""
import hashlib
import os
import sys

STAMPS = os.path.join("build", "prepared")


def files_below(path):
    if os.path.isfile(path):
        yield path
        return
    for folder, folders, names in os.walk(path):
        folders[:] = sorted(name for name in folders if not name.startswith("."))
        for name in sorted(names):
            if not name.startswith("."):
                yield os.path.join(folder, name)


def digest(content_inputs, stat_inputs):
    h = hashlib.sha256()
    for path in list(content_inputs) + list(stat_inputs):
        if not os.path.exists(path):
            h.update(b"A\0" + path.encode() + b"\0")  # absent: a typo or a missing build output must show, not vanish
    for path in content_inputs:
        for file in files_below(path):
            h.update(b"C\0" + file.encode() + b"\0")
            try:
                with open(file, "rb") as f:
                    for block in iter(lambda: f.read(1 << 20), b""):
                        h.update(block)
            except OSError:
                h.update(b"unreadable")
    for path in stat_inputs:
        for file in files_below(path):
            try:
                st = os.stat(file)
                h.update(b"S\0" + file.encode() + b"\0" + f"{st.st_size}\0{st.st_mtime_ns}".encode())
            except OSError:
                h.update(b"S\0" + file.encode() + b"\0missing")
    return h.hexdigest()


def split_inputs(args):
    if "--stat" in args:
        at = args.index("--stat")
        return args[:at], args[at + 1:]
    return args, []


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__.split("Run from")[0])
    command = args[0]
    if command == "compute":
        print(digest(*split_inputs(args[1:])))
    elif command == "check" and len(args) >= 3:
        name, marker = args[1:3]
        if os.environ.get("MINATO_PREPARED") != "1" or not os.path.exists(marker):
            sys.exit(1)
        try:
            with open(os.path.join(STAMPS, name + ".stamp")) as f:
                stamp = f.read().strip()
        except OSError:
            sys.exit(1)
        sys.exit(0 if stamp == digest(*split_inputs(args[3:])) else 1)
    elif command == "write" and len(args) == 3:
        os.makedirs(STAMPS, exist_ok=True)
        with open(os.path.join(STAMPS, args[1] + ".stamp"), "w") as f:
            f.write(args[2] + "\n")
    else:
        raise SystemExit("usage: prepared-stamp.py compute|check|write, see the file's header")


main()
