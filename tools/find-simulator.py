#!/usr/bin/env python3
"""Prints "<udid> <name>" of the newest available iPhone simulator, or of the one whose name or
UDID is given, and nothing when there is none.
Usage: tools/find-simulator.py [simulator name or UDID]"""
import json
import subprocess
import sys


def main():
    wanted = sys.argv[1] if len(sys.argv) > 1 else ""
    listing = subprocess.run(["xcrun", "simctl", "list", "devices", "available", "--json"],
                             check=True, capture_output=True, text=True).stdout
    devices = json.loads(listing)["devices"]
    for runtime in sorted(devices, reverse=True):
        if "iOS" not in runtime:
            continue
        for device in devices[runtime]:
            if wanted and wanted not in (device["name"], device["udid"]):
                continue
            if not wanted and not device["name"].startswith("iPhone"):
                continue
            print(device["udid"], device["name"])
            return


if __name__ == "__main__":
    main()
