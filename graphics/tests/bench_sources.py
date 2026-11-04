#!/usr/bin/env python3
"""The digest of the sources the bench's APKs are built from: graphics/bench without native/, the host build's own
files. tools/build-bench.sh writes it next to the APKs as sources.sha256, and bench_test.py refuses APKs whose digest
is not their tree's, which would test another scene. Usage: bench_sources.py <graphics/bench>"""
import hashlib
import pathlib
import sys


def digest(bench):
    """The SHA-256 of each file's path under `bench` and its content, in path order."""
    bench = pathlib.Path(bench)
    total = hashlib.sha256()
    files = sorted(path for path in bench.rglob("*") if path.is_file() and path.relative_to(bench).parts[0] != "native"
                   and "__pycache__" not in path.parts and path.name != ".DS_Store")
    for path in files:
        total.update(path.relative_to(bench).as_posix().encode() + b"\0")
        total.update(hashlib.sha256(path.read_bytes()).digest())
    return total.hexdigest()


if __name__ == "__main__":
    print(digest(sys.argv[1]))
