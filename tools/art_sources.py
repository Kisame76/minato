#!/usr/bin/env python3
"""The stamp of the sources ART is built from: runtime/art without tests/, whose patches and own files decide what
build/art holds. tools/build-art.sh writes it next to the libraries as sources.sha256; the checks that run on that
build (tools/compiled-apps-check.sh and others) stop when it is not the tree's, which would test an older ART.
Usage: art_sources.py <runtime/art>, which prints the digest, or
       art_sources.py --check <runtime/art> <build/art>, which exits 1, saying why, when the build's stamp is not the
       tree's"""
import hashlib
import pathlib
import sys


def digest(sources):
    """The SHA-256 of each file's path under `sources` and its content, in path order."""
    sources = pathlib.Path(sources)
    total = hashlib.sha256()
    files = sorted(path for path in sources.rglob("*")
                   if path.is_file() and path.relative_to(sources).parts[0] != "tests"
                   and "__pycache__" not in path.parts and path.name != ".DS_Store")
    for path in files:
        total.update(path.relative_to(sources).as_posix().encode() + b"\0")
        total.update(hashlib.sha256(path.read_bytes()).digest())
    return total.hexdigest()


if __name__ == "__main__":
    if sys.argv[1] == "--check":
        stamp = pathlib.Path(sys.argv[3]) / "sources.sha256"
        built = stamp.read_text().strip() if stamp.is_file() else None
        if built != digest(sys.argv[2]):
            print(f"stopped: {sys.argv[3]} was not built from the current {sys.argv[2]} "
                  f"({'no stamp' if built is None else 'its stamp differs'}); run tools/build-art.sh")
            sys.exit(1)
    else:
        print(digest(sys.argv[1]))
