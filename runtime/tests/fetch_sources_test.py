#!/usr/bin/env python3
"""tools/fetch-sources.py's lists (ADR-0021, ADR-0033 a): a plain run fetches and pins only "sources", the guest
libc's; --set art only "art"; an archive with a "url" is checked against its SHA-256 before anything of it is
extracted into its "directory", and an archive of one commit without a SHA-256, whose url names the commit by all
40 hex digits, by its content digest alone.
Against a host in memory, in a scratch directory. And its fetch: a host that answers
429 (Too Many Requests) is asked again after its Retry-After, at most 120 s, or after a backoff, 5 tries in all."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import sys
import tarfile
import tempfile
import urllib.error

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


def linked(members):
    """A .tar.gz of (name, kind, value) in their order: a "file" with its content, a "dir", or a "link" to a target."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
        for name, kind, value in members:
            info = tarfile.TarInfo(name)
            if kind == "link":
                info.type, info.linkname = tarfile.SYMTYPE, value
                tar.addfile(info)
            elif kind == "dir":
                info.type, info.mode = tarfile.DIRTYPE, 0o755
                tar.addfile(info)
            else:
                info.size, info.mode = len(value), 0o644
                tar.addfile(info, io.BytesIO(value))
    return buffer.getvalue()


LIBC = archive({"libc/NOTICE": b"libc\n"})
ART = archive({"runtime/runtime.cc": b"// art\n"})
JDK = archive({"jdk-17/Contents/Home/bin/java": b"#!/bin/sh\n"})
VOLK_COMMIT = "3333" * 10
VOLK = archive({"volk-3333/volk.h": b"// volk\n"})
VOLK_URL = f"https://github.example/zeux/volk/archive/{VOLK_COMMIT}.tar.gz"
SERVED = {
    f"{HOST}/platform/bionic/+archive/1111.tar.gz": LIBC,
    f"{HOST}/platform/art/+archive/2222.tar.gz": ART,
    "https://jdk.example/jdk.tar.gz": JDK,
    VOLK_URL: VOLK,
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


class Response:
    def __init__(self, data):
        self.data = data

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def read(self):
        return self.data


def fetch_with(module, answers):
    """module.fetch against a host that gives `answers` in turn, each bytes or an HTTP status with its headers:
    what it returned or its error, and the seconds it waited."""
    answers = list(answers)
    waits = []

    def urlopen(url, timeout):
        answer = answers.pop(0)
        if isinstance(answer, bytes):
            return Response(answer)
        status, headers = answer
        raise urllib.error.HTTPError(url, status, "refused", headers, None)

    module.urlopen = urlopen
    module.sleep = waits.append
    try:
        return module.fetch(f"{HOST}/a.tar.gz"), waits
    except (SystemExit, urllib.error.HTTPError) as error:
        return error, waits


def check_retries(module):
    result, waits = fetch_with(module, [(429, {"Retry-After": "7"}), b"data"])
    check(result == b"data" and waits == [7], f"a 429 is asked again after its Retry-After: {result!r} {waits}")
    result, waits = fetch_with(module, [(429, {"Retry-After": "600"}), b"data"])
    check(result == b"data" and waits == [120], f"a Retry-After waits at most 120 s: {result!r} {waits}")
    result, waits = fetch_with(module, [(429, {})] * 5)
    check(isinstance(result, SystemExit) and "429" in str(result) and "5 tries" in str(result),
          f"5 answers of 429 give up with a message: {result!r}")
    check(waits == [5, 10, 20, 40], f"without Retry-After it backs off: {waits}")
    result, waits = fetch_with(module, [(404, {}), b"data"])
    check(isinstance(result, urllib.error.HTTPError) and result.code == 404 and waits == [],
          f"another status is not asked again: {result!r} {waits}")


def main():
    module = load()
    check_retries(module)
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

    # An archive of one commit that its host makes on request (MoltenVK's from GitHub, ADR-0044 b): no SHA-256 of
    # its bytes, only the content digest, as android.googlesource.com's sources have.
    with tempfile.TemporaryDirectory() as root:
        listed = dict(manifest(), volk=[{"name": "volk", "url": VOLK_URL, "commit": VOLK_COMMIT,
                                         "directory": "build/sources/moltenvk/Volk", "digest": ""}])
        fetched, error, after = run(module, root, listed, ["--set", "volk", "--pin"])
        check(error is None and fetched == [VOLK_URL], f"an archive of a commit without a SHA-256: {fetched} {error}")
        check(after["volk"][0]["digest"] != "", "--pin pins the commit archive's content digest")
        check(os.path.isfile(os.path.join(root, "build/sources/moltenvk/Volk/volk-3333/volk.h")),
              "the commit archive lies in its directory")
    with tempfile.TemporaryDirectory() as root:
        SERVED[VOLK_URL] = archive({"volk-3333/volk.h": b"// another volk\n"})
        fetched, error, _ = run(module, root, after, ["--set", "volk"])
        SERVED[VOLK_URL] = VOLK
        check(error is not None and "digest" in error, f"other content under a pinned commit is refused: {error}")
        check(not os.path.exists(os.path.join(root, "build/sources/moltenvk/Volk")),
              "nothing of the refused commit archive is extracted")
    with tempfile.TemporaryDirectory() as root:
        listed = dict(manifest(), volk=[{"name": "volk", "url": VOLK_URL, "digest": ""}])
        fetched, error, _ = run(module, root, listed, ["--set", "volk", "--pin"])
        check(error is not None and fetched == [],
              f"an archive with neither a SHA-256 nor a commit is refused: {error}")
    # The url names the commit the archive is of, by all 40 hex digits: not a branch's archive listed with a commit,
    # nor an abbreviated commit.
    branch = "https://github.example/zeux/volk/archive/refs/heads/main.tar.gz"
    SERVED[branch] = VOLK
    for what, url, commit in (("a branch's archive", branch, VOLK_COMMIT),
                              ("an abbreviated commit", VOLK_URL, VOLK_COMMIT[:7])):
        with tempfile.TemporaryDirectory() as root:
            listed = dict(manifest(), volk=[{"name": "volk", "url": url, "commit": commit,
                                             "directory": "build/sources/moltenvk/Volk", "digest": ""}])
            fetched, error, _ = run(module, root, listed, ["--set", "volk", "--pin"])
            check(error is not None and "commit" in error and fetched == [],
                  f"{what} as the archive of a commit is refused before it is fetched: {fetched} {error}")
    del SERVED[branch]

    # Links that lead out only together: "b -> ." and "a -> b/.." each look harmless by name, but "a" is the folder's
    # parent, so "a/x" would land beside the folder. No member is written through a link, and a link that leaves the
    # folder through other links is left out once all is extracted; android.googlesource.com's archives too. Links keep
    # their targets as written, which Python 3.14's tarfile would write as their normpath ("b/.." as ".").
    chain_commit = "4444" * 10
    chain = f"https://github.example/chain/archive/{chain_commit}.tar.gz"
    listed = dict(manifest(), chain=[{"name": "chain", "url": chain, "commit": chain_commit,
                                      "directory": "build/sources/moltenvk/Chain", "digest": ""}])
    for what, members in (
            ("through a chain of links that leads out",
             [("b", "link", "."), ("a", "link", "b/.."), ("a/x", "file", b"out\n")]),
            ("through a link to a folder inside", [("d", "dir", None), ("l", "link", "d"), ("l/y", "file", b"y\n")]),
            ("in place of a link", [("m", "link", "n"), ("m", "file", b"m\n")])):
        with tempfile.TemporaryDirectory() as root:
            SERVED[chain] = linked(members)
            fetched, error, _ = run(module, root, listed, ["--set", "chain", "--pin"])
            written = [os.path.relpath(os.path.join(folder, name), root) for folder, _, names in os.walk(root)
                       for name in names if name in ("x", "y", "m", "n")]
            check(error is not None and "link" in error, f"a member written {what} is refused: {error}")
            check(written == [], f"nothing is written {what}: {written}")
    with tempfile.TemporaryDirectory() as root:
        SERVED[chain] = linked([("b", "link", "."), ("a", "link", "b/.."), ("kept", "file", b"kept\n"),
                                ("same", "link", "./kept")])
        fetched, error, _ = run(module, root, listed, ["--set", "chain", "--pin"])
        folder = os.path.join(root, "build/sources/moltenvk/Chain")
        check(error is None, f"an archive whose links only lead out does not fail: {error}")
        check(os.path.isfile(os.path.join(folder, "kept")) and os.path.islink(os.path.join(folder, "b")) and
              not os.path.lexists(os.path.join(folder, "a")),
              "a link that leaves the folder through another link is left out, the others are kept")
        same = os.path.join(folder, "same")
        check(os.path.islink(same) and os.readlink(same) == "./kept",
              f"a link keeps its target as written, so the digest does not depend on Python's version: "
              f"{os.readlink(same) if os.path.islink(same) else None}")
    del SERVED[chain]

    with tempfile.TemporaryDirectory() as root:
        fetched, error, _ = run(module, root, manifest(), ["--set", "nothing"])
        check(error is not None and fetched == [], f"a list that does not exist is an error: {error}")
        fetched, error, _ = run(module, root, manifest(), ["--set", "host"])
        check(error is not None and fetched == [], f"an entry that is no list is an error: {error}")

    print("fetch-sources: ok" if failures == 0 else f"fetch-sources: {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
