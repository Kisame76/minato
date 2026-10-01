#!/usr/bin/env python3
"""tools/fetch-sources.py's lists (ADR-0021, ADR-0033 a): a plain run fetches and pins only "sources", the guest
libc's; --set art only "art"; an archive with a "url" is checked against its SHA-256 before anything of it is
extracted into its "directory". Against a host in memory, in a scratch directory."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import sys
import tarfile
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "fetch-sources.py")
HOST = "https://source.example"
failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def load():
    spec = importlib.util.spec_from_file_location("fetch_sources", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def archive(files):
    """A .tar.gz of {path: content}."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
        for path, content in files.items():
            info = tarfile.TarInfo(path)
            info.size = len(content)
            info.mode = 0o755
            tar.addfile(info, io.BytesIO(content))
    return buffer.getvalue()


LIBC = archive({"libc/NOTICE": b"libc\n"})
ART = archive({"runtime/runtime.cc": b"// art\n"})
JDK = archive({"jdk-17/Contents/Home/bin/java": b"#!/bin/sh\n"})
SERVED = {
    f"{HOST}/platform/bionic/+archive/1111.tar.gz": LIBC,
    f"{HOST}/platform/art/+archive/2222.tar.gz": ART,
    "https://jdk.example/jdk.tar.gz": JDK,
}


def manifest(jdk_sha256=None):
    return {
        "host": HOST,
        "sources": [{"name": "bionic", "project": "platform/bionic", "commit": "1111", "digest": ""}],
        "art": [
            {"name": "art", "project": "platform/art", "commit": "2222", "digest": ""},
            {"name": "jdk", "url": "https://jdk.example/jdk.tar.gz", "directory": "build/tools/jdk",
             "sha256": jdk_sha256 or hashlib.sha256(JDK).hexdigest(), "digest": ""},
        ],
    }


def run(module, root, listed, arguments):
    """main() on `listed` as root/third_party/sources.json: the URLs it fetched, its error, the list after it."""
    path = os.path.join(root, "third_party", "sources.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        json.dump(listed, handle, indent=2)
    module.ROOT = root
    module.MANIFEST = path
    module.DESTINATION = os.path.join(root, "build", "sources")
    fetched = []

    def fetch(url):
        fetched.append(url)
        return SERVED[url]

    module.fetch = fetch
    sys.argv = ["fetch-sources.py"] + arguments
    error = None
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            module.main()
    except SystemExit as stop:
        error = str(stop)
    with open(path) as handle:
        return fetched, error, json.load(handle)


def main():
    module = load()

    with tempfile.TemporaryDirectory() as root:
        fetched, error, after = run(module, root, manifest(), ["--pin"])
        check(error is None, f"a plain run with --pin: {error}")
        check(fetched == [f"{HOST}/platform/bionic/+archive/1111.tar.gz"], f"a plain run fetches only Bionic's list: {fetched}")
        check(after["sources"][0]["digest"] != "", "a plain run pins Bionic's list")
        check(all(entry["digest"] == "" for entry in after["art"]), "a plain run leaves the art list alone")
        check(os.path.isfile(os.path.join(root, "build/sources/bionic/libc/NOTICE")), "Bionic lies in build/sources")

    with tempfile.TemporaryDirectory() as root:
        fetched, error, after = run(module, root, manifest(), ["--set", "art", "--pin"])
        check(error is None, f"--set art with --pin: {error}")
        check(fetched == [f"{HOST}/platform/art/+archive/2222.tar.gz", "https://jdk.example/jdk.tar.gz"],
              f"--set art fetches only the art list: {fetched}")
        check(all(entry["digest"] != "" for entry in after["art"]), "--set art pins the art list")
        check(after["sources"][0]["digest"] == "", "--set art leaves Bionic's list alone")
        check(os.path.isfile(os.path.join(root, "build/sources/art/runtime/runtime.cc")), "ART lies in build/sources")
        check(os.access(os.path.join(root, "build/tools/jdk/jdk-17/Contents/Home/bin/java"), os.X_OK),
              "the JDK lies in its directory, executable")
        # Pinned and present: a second run fetches nothing.
        fetched, error, _ = run(module, root, after, ["--set", "art"])
        check(error is None and fetched == [], f"a pinned list that is present is not fetched again: {fetched} {error}")

    with tempfile.TemporaryDirectory() as root:
        fetched, error, _ = run(module, root, manifest(jdk_sha256="0" * 64), ["--set", "art", "--pin", "jdk"])
        check(error is not None and "SHA-256" in error, f"an archive with another SHA-256 is refused: {error}")
        check(not os.path.exists(os.path.join(root, "build/tools/jdk")), "nothing of a refused archive is extracted")

    with tempfile.TemporaryDirectory() as root:
        fetched, error, _ = run(module, root, manifest(), ["--set", "nothing"])
        check(error is not None and fetched == [], f"a list that does not exist is an error: {error}")
        fetched, error, _ = run(module, root, manifest(), ["--set", "host"])
        check(error is not None and fetched == [], f"an entry that is no list is an error: {error}")

    print("fetch-sources: ok" if failures == 0 else f"fetch-sources: {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
