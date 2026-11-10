#!/usr/bin/env python3
"""Checks Google's Play libraries for ADR-0050's pad-test (test 3) against their pins, the entry "play-libraries" of
third_party/sources.json, and fetches them on request: asset-delivery 2.2.2 and feature-delivery 2.1.0 with the
libraries their POMs name, without AndroidX, as AARs from Google's Maven. They only build pad-test on this Mac, after
the libraries' terms are accepted (ADR-0050 c)); never committed and never in the app (Non-Goals). They stay a black
box: javac, D8 and aapt2 take them, and nothing reads their classes.

Usage: tools/fetch-play-libraries.py [--folder <folder>]          check the AARs and print their folder
       tools/fetch-play-libraries.py [--folder <folder>] --fetch  fetch the missing ones (network), then check them
       tools/fetch-play-libraries.py --self-test                  check the script's guards (no network)
  --set core  the set of play:core 1.10.3, which older apps ship in place of asset-delivery and feature-delivery
              (pad-test-core); the sets never share a classpath, so --list prints one set's AARs, one path per line
  --folder  where the AARs lie, default ~/.android/minato-play-libraries, a private folder outside the repository

Each AAR must have the pinned size and the SHA-256 that Google's Maven states beside it; a fetch writes a temporary file
next to it and renames it only once it matches, so no half-written or other AAR stays, and keeps the folder private
(0700). Exit 0 with the folder on stdout, 1 with what to do.
"""
import argparse
import hashlib
import json
import os
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(os.path.expanduser("~"), ".android", "minato-play-libraries")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
MAVEN = "https://dl.google.com/dl/android/maven2/"


def pins(manifest=MANIFEST, library_set=None):
    """The pinned AARs of one set: name, url, bytes and sha256 each. The default set, pad-test's asset-delivery and
    feature-delivery, is the entries without a "set"; the set "core" is the monolithic Play Core of older apps."""
    with open(manifest) as handle:
        assets = [asset for asset in json.load(handle)["play-libraries"]["assets"]
                  if asset.get("set") == library_set]
    names = [asset["name"] for asset in assets]
    if not assets or len(set(names)) != len(names):
        raise SystemExit("fetch-play-libraries: the pins must name each AAR once")
    for asset in assets:
        name = asset["name"]
        if os.path.basename(name) != name or not name.endswith(".aar") or ".." in asset["url"] \
                or not asset["url"].startswith(MAVEN) or not asset["url"].endswith("/" + name):
            raise SystemExit(f"fetch-play-libraries: the pin of {name} is not an AAR of Google's Maven")
    return assets


def outside(folder):
    """Whether `folder` lies outside the repository, as the libraries must."""
    folder, root = os.path.realpath(folder), os.path.realpath(ROOT)
    return os.path.commonpath([folder, root]) != root


def problem(path, asset):
    """Why the file at `path` is not the pinned AAR, or None when it is."""
    if not os.path.isfile(path):
        return f"{path} is missing: run tools/fetch-play-libraries.py --fetch (it downloads {asset['bytes']} bytes)"
    size = os.path.getsize(path)
    if size != asset["bytes"]:
        return f"{path} has {size} bytes, the pin {asset['bytes']}"
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    if digest.hexdigest() != asset["sha256"]:
        return f"{path} has the SHA-256 {digest.hexdigest()}, the pin {asset['sha256']}"
    return None


def private(folder):
    """Makes `folder` if it is missing, readable by its owner only."""
    os.makedirs(folder, mode=0o700, exist_ok=True)
    os.chmod(folder, 0o700)


def fetch(path, asset):
    handle, temporary = tempfile.mkstemp(prefix=".fetch-", dir=os.path.dirname(path))
    try:
        with os.fdopen(handle, "wb") as out, urllib.request.urlopen(asset["url"], timeout=60) as response:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                out.write(chunk)
        reason = problem(temporary, asset)
        if reason:
            raise SystemExit(f"fetch-play-libraries: the download of {asset['name']} differs from its pin: {reason}")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def self_test():
    with tempfile.TemporaryDirectory() as folder:
        content = b"an aar"
        asset = {"name": "x.aar", "url": MAVEN + "com/example/x/1/x.aar", "bytes": len(content),
                 "sha256": hashlib.sha256(content).hexdigest()}
        path = os.path.join(folder, "x.aar")
        assert "missing" in problem(path, asset)
        with open(path, "wb") as handle:
            handle.write(content)
        assert problem(path, asset) is None
        with open(path, "wb") as handle:
            handle.write(b"an aaR")
        assert "SHA-256" in problem(path, asset)
        with open(path, "wb") as handle:
            handle.write(content + b"x")
        assert "bytes" in problem(path, asset)

        # A fetch puts the file in place only once it matches, and leaves nothing else behind.
        source = os.path.join(folder, "source.aar")
        with open(source, "wb") as handle:
            handle.write(content)
        libraries = os.path.join(folder, "libraries")
        private(libraries)
        target = os.path.join(libraries, "x.aar")
        fetch(target, dict(asset, url="file://" + source))
        assert problem(target, asset) is None and os.listdir(libraries) == ["x.aar"]
        os.remove(target)
        try:
            fetch(target, dict(asset, url="file://" + source, sha256="0" * 64))
            raise AssertionError("a download of another digest was kept")
        except SystemExit as error:
            assert "differs from its pin" in str(error)
        assert os.listdir(libraries) == []

        # The folder is private, also when it was there before with other permissions.
        assert os.stat(libraries).st_mode & 0o777 == 0o700
        os.chmod(libraries, 0o755)
        private(libraries)
        assert os.stat(libraries).st_mode & 0o777 == 0o700

        assert outside(folder) and outside(FOLDER)
        assert not outside(ROOT) and not outside(os.path.join(ROOT, "build", "play-libraries"))

        manifest = os.path.join(folder, "sources.json")
        for assets, reason in (([asset, asset], "once"), ([], "once"),
                               ([dict(asset, url="https://example.com/x.aar")], "Google's Maven"),
                               ([dict(asset, name="../x.aar", url=MAVEN + "x/../x.aar")], "Google's Maven"),
                               ([dict(asset, name="x.jar", url=MAVEN + "x/1/x.jar")], "Google's Maven")):
            with open(manifest, "w") as handle:
                json.dump({"play-libraries": {"assets": assets}}, handle)
            try:
                pins(manifest)
                raise AssertionError(f"the pins {assets} passed")
            except SystemExit as error:
                assert reason in str(error), error
    real = pins()
    assert sorted(asset["name"] for asset in real) == [
        "asset-delivery-2.2.2.aar", "core-common-2.0.3.aar", "feature-delivery-2.1.0.aar",
        "play-services-basement-18.3.0.aar", "play-services-tasks-18.1.0.aar"]
    assert [asset["name"] for asset in pins(library_set="core")] == ["core-1.10.3.aar"]
    print("fetch-play-libraries: self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--folder", default=FOLDER)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--set", dest="library_set", choices=["core"], default=None)
    parser.add_argument("--list", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        self_test()
        return
    folder = os.path.abspath(options.folder)
    if not outside(folder):
        print(f"fetch-play-libraries: {folder} lies in the repository; the libraries must stay outside it",
              file=sys.stderr)
        sys.exit(1)
    assets = pins(library_set=options.library_set)
    if options.fetch:
        private(folder)
        for asset in assets:
            path = os.path.join(folder, asset["name"])
            if not os.path.exists(path):
                fetch(path, asset)
    reasons = [problem(os.path.join(folder, asset["name"]), asset) for asset in assets]
    reasons = [reason for reason in reasons if reason]
    for reason in reasons:
        print(f"fetch-play-libraries: {reason}", file=sys.stderr)
    if reasons:
        sys.exit(1)
    if options.list:
        print("\n".join(os.path.join(folder, asset["name"]) for asset in assets))
    else:
        print(folder)


if __name__ == "__main__":
    main()
