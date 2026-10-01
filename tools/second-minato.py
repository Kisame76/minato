#!/usr/bin/env python3
"""Prints the bundle IDs of other installs of Minato in `devicectl device info apps --json-output`'s file.

Usage: tools/second-minato.py <apps.json> <Minato's bundle ID>
An app counts when it is named Minato under another bundle ID, such as SideStore's install beside Xcode's;
both answer minato:// (tools/session-check.sh). Prints nothing when there is none or the file has no apps.
"""
import json
import sys


def others(apps, own):
    return sorted(app.get("bundleIdentifier", "") for app in apps
                  if app.get("name") == "Minato" and app.get("bundleIdentifier") != own)


def main():
    path, own = sys.argv[1], sys.argv[2]
    try:
        with open(path) as file:
            apps = json.load(file).get("result", {}).get("apps", [])
    except (OSError, ValueError):
        apps = []
    print(", ".join(others(apps, own)))


if __name__ == "__main__":
    main()
