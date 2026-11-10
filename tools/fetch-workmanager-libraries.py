#!/usr/bin/env python3
"""Checks the libraries of the WorkManager test app against their pins, the entry "workmanager-libraries" of
third_party/sources.json, and fetches them on request: androidx.work:work-runtime 2.12.0 with its runtime closure
(Kotlin's standard library and coroutines, androidx.*) from Google's Maven and Maven Central. They only build a test
app on this Mac, after the user's OK; never committed and never in the app (Non-Goals); they stay a black box: javac,
D8 and aapt2 take them, and nothing reads their classes.

Usage: tools/fetch-workmanager-libraries.py [--folder <folder>]          check the files and print their folder
       tools/fetch-workmanager-libraries.py [--folder <folder>] --fetch  fetch the missing ones (network), then check
       tools/fetch-workmanager-libraries.py [--folder <folder>] --list   print the files' paths, one per line
       tools/fetch-workmanager-libraries.py --self-test                  check the script's guards (no network)
  --folder  where the files lie, default ~/.android/minato-workmanager-libraries, outside the repository

It shares the checks of tools/fetch-play-libraries.py: each file must have the pinned size and SHA-256, a fetch
writes a temporary file and renames it only once it matches, and the folder is private (0700).
"""
import argparse
import importlib.util
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(os.path.expanduser("~"), ".android", "minato-workmanager-libraries")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
HOSTS = ("https://dl.google.com/dl/android/maven2/", "https://repo.maven.apache.org/maven2/")

_spec = importlib.util.spec_from_file_location("play", os.path.join(ROOT, "tools", "fetch-play-libraries.py"))
play = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(play)


def pins(manifest=MANIFEST):
    """The pinned files: name, url, bytes and sha256 each."""
    with open(manifest) as handle:
        assets = json.load(handle)["workmanager-libraries"]["assets"]
    names = [asset["name"] for asset in assets]
    if not assets or len(set(names)) != len(names):
        raise SystemExit("fetch-workmanager-libraries: the pins must name each file once")
    for asset in assets:
        name = asset["name"]
        if os.path.basename(name) != name or not name.endswith((".aar", ".jar")) or ".." in asset["url"] \
                or not asset["url"].startswith(HOSTS) or not asset["url"].endswith("/" + name):
            raise SystemExit(f"fetch-workmanager-libraries: the pin of {name} is not a file of Google's Maven "
                             "or Maven Central")
    return assets


def self_test():
    manifest = os.path.join(os.path.dirname(MANIFEST), "sources.json")
    real = pins(manifest)
    names = [asset["name"] for asset in real]
    assert "work-runtime-2.12.0.aar" in names and "kotlin-stdlib-2.1.20.jar" in names, names
    assert all(len(asset["sha256"]) == 64 and asset["bytes"] > 0 for asset in real)
    import tempfile
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "p.json")
        asset = {"name": "x.aar", "url": HOSTS[0] + "a/x/1/x.aar", "bytes": 1, "sha256": "0" * 64}
        for assets, reason in (([asset, asset], "once"), ([], "once"),
                               ([dict(asset, url="https://example.com/x.aar")], "Google's Maven"),
                               ([dict(asset, name="x.zip", url=HOSTS[0] + "a/x/1/x.zip")], "Google's Maven")):
            with open(path, "w") as handle:
                json.dump({"workmanager-libraries": {"assets": assets}}, handle)
            try:
                pins(path)
                raise AssertionError(f"the pins {assets} passed")
            except SystemExit as error:
                assert reason in str(error), error
    assert play.outside(FOLDER) and not play.outside(os.path.join(ROOT, "build", "wm"))
    print("fetch-workmanager-libraries: self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--folder", default=FOLDER)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        self_test()
        return
    folder = os.path.abspath(options.folder)
    if not play.outside(folder):
        print(f"fetch-workmanager-libraries: {folder} lies in the repository; the libraries must stay outside it",
              file=sys.stderr)
        sys.exit(1)
    assets = pins()
    if options.fetch:
        play.private(folder)
        for asset in assets:
            path = os.path.join(folder, asset["name"])
            if not os.path.exists(path):
                play.fetch(path, asset)
    reasons = [play.problem(os.path.join(folder, asset["name"]), asset) for asset in assets]
    reasons = [reason for reason in reasons if reason]
    for reason in reasons:
        print(f"fetch-workmanager-libraries: {reason}", file=sys.stderr)
    if reasons:
        sys.exit(1)
    if options.list:
        print("\n".join(os.path.join(folder, asset["name"]) for asset in assets))
    else:
        print(folder)


if __name__ == "__main__":
    main()
