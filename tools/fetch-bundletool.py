#!/usr/bin/env python3
"""Checks bundletool against its pin, the entry "bundletool" of third_party/sources.json (ADR-0049 a2), and fetches
it on request. bundletool is M5's Mac build tool, for tools/formats-check.sh and the .aab tool, never in the app.

Usage: tools/fetch-bundletool.py              check build/tools/bundletool/<jar> and print its path
       tools/fetch-bundletool.py --fetch      fetch the jar when it is missing (network), then check it
       tools/fetch-bundletool.py --self-test  check the script's guards (no network)

The jar must have the pinned size and SHA-256; a fetch writes a temporary file next to it and renames it only once it
matches, so no half-written or other jar stays. Exit 0 with the jar's path on stdout, 1 with what to do.
"""
import argparse
import hashlib
import json
import os
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(ROOT, "build", "tools", "bundletool")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")


def pin(manifest=MANIFEST):
    """The pinned asset: name, url, bytes and sha256."""
    with open(manifest) as handle:
        assets = json.load(handle)["bundletool"]["assets"]
    if len(assets) != 1:
        raise SystemExit("fetch-bundletool: the pin must name exactly one asset")
    return assets[0]


def problem(path, asset):
    """Why the file at `path` is not the pinned jar, or None when it is."""
    if not os.path.isfile(path):
        return f"{path} is missing: run tools/fetch-bundletool.py --fetch (it downloads {asset['bytes']} bytes)"
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


def fetch(path, asset):
    os.makedirs(os.path.dirname(path), exist_ok=True)
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
            raise SystemExit(f"fetch-bundletool: the download differs from its pin: {reason}")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def self_test():
    with tempfile.TemporaryDirectory() as folder:
        content = b"bundletool"
        asset = {"name": "x.jar", "url": "https://invalid.example/x.jar", "bytes": len(content),
                 "sha256": hashlib.sha256(content).hexdigest()}
        path = os.path.join(folder, "x.jar")
        assert "missing" in problem(path, asset)
        with open(path, "wb") as handle:
            handle.write(content)
        assert problem(path, asset) is None
        with open(path, "wb") as handle:
            handle.write(b"bundletoo!")
        assert "SHA-256" in problem(path, asset)
        with open(path, "wb") as handle:
            handle.write(content + b"x")
        assert "bytes" in problem(path, asset)
        manifest = os.path.join(folder, "sources.json")
        with open(manifest, "w") as handle:
            json.dump({"bundletool": {"assets": [asset, asset]}}, handle)
        try:
            pin(manifest)
            raise AssertionError("two assets passed")
        except SystemExit as error:
            assert "exactly one" in str(error)
    real = pin()
    assert real["name"].endswith(".jar") and real["url"].startswith("https://github.com/google/bundletool/")
    print("fetch-bundletool: self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        self_test()
        return
    asset = pin()
    path = os.path.join(FOLDER, asset["name"])
    if options.fetch and not os.path.exists(path):
        fetch(path, asset)
    reason = problem(path, asset)
    if reason:
        print(f"fetch-bundletool: {reason}", file=sys.stderr)
        sys.exit(1)
    print(path)


if __name__ == "__main__":
    main()
