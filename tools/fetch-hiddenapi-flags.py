#!/usr/bin/env python3
"""Checks Android 15's hidden API lists against their pin, the entry "hiddenapi-flags" of third_party/sources.json, and
fetches them on request: hiddenapi-flags.csv, which Google publishes on developer.android.com (about 71 MB), the build
input that tools/hiddenapi-encode.py turns into the flags in framework.jar's and the boot jars' dex files. It stays
outside the repository and never reaches the app, only its encoded flags do; fetched once, after the user's OK.

Usage: tools/fetch-hiddenapi-flags.py [--folder <folder>]          check the file and print its path
       tools/fetch-hiddenapi-flags.py [--folder <folder>] --fetch  fetch it if it is missing (network), then check
       tools/fetch-hiddenapi-flags.py --self-test                  check the script's guards (no network)
  --folder  where the file lies, default ~/.android/minato-hiddenapi, outside the repository

It shares the checks of tools/fetch-play-libraries.py: the file must have the pinned size and SHA-256, a fetch writes a
temporary file and renames it only once it matches, and the folder is private (0700).
"""
import argparse
import importlib.util
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(os.path.expanduser("~"), ".android", "minato-hiddenapi")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
NAME = "hiddenapi-flags.csv"
URL = "https://dl.google.com/developers/android/vic/non-sdk/hiddenapi-flags.csv"

_spec = importlib.util.spec_from_file_location("play", os.path.join(ROOT, "tools", "fetch-play-libraries.py"))
play = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(play)


def pin(manifest=MANIFEST):
    """The pinned file: name, url, bytes and sha256."""
    with open(manifest) as handle:
        entry = json.load(handle)["hiddenapi-flags"]
    if entry["url"] != URL or len(entry["sha256"]) != 64 or entry["bytes"] <= 0:
        raise SystemExit(f"fetch-hiddenapi-flags: the pin is not {URL} with a size and a SHA-256")
    return {"name": NAME, "url": entry["url"], "bytes": entry["bytes"], "sha256": entry["sha256"]}


def self_test():
    real = pin()
    assert real["bytes"] == 71184887 and real["sha256"].startswith("40134e20"), real
    import tempfile
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "p.json")
        for entry in ({"url": "https://example.com/x.csv", "sha256": "0" * 64, "bytes": 1},
                      {"url": URL, "sha256": "0" * 63, "bytes": 1}, {"url": URL, "sha256": "0" * 64, "bytes": 0}):
            with open(path, "w") as handle:
                json.dump({"hiddenapi-flags": entry}, handle)
            try:
                pin(path)
                raise AssertionError(f"the pin {entry} passed")
            except SystemExit as error:
                assert "is not" in str(error), error
        data = os.path.join(folder, NAME)
        with open(data, "w") as handle:
            handle.write("x")
        assert play.problem(data, real) and "bytes" in play.problem(data, real)
    assert play.outside(FOLDER) and not play.outside(os.path.join(ROOT, "build", "hiddenapi"))
    print("fetch-hiddenapi-flags: self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--folder", default=FOLDER)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        self_test()
        return
    folder = os.path.abspath(options.folder)
    if not play.outside(folder):
        print(f"fetch-hiddenapi-flags: {folder} lies in the repository; the file must stay outside it", file=sys.stderr)
        sys.exit(1)
    asset = pin()
    path = os.path.join(folder, NAME)
    if options.fetch:
        play.private(folder)
        if not os.path.exists(path):
            play.fetch(path, asset)
    reason = play.problem(path, asset)
    if reason:
        print(f"fetch-hiddenapi-flags: {reason.replace('fetch-play-libraries.py', 'fetch-hiddenapi-flags.py')}",
              file=sys.stderr)
        sys.exit(1)
    print(path)


if __name__ == "__main__":
    main()
