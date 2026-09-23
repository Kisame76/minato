#!/usr/bin/env python3
"""Prints "<identifier> <udid> <developer mode> <connection state>" of the first paired physical iOS
device, or of the one whose identifier, UDID or name is given, and nothing when there is none.
Reads the devicectl JSON v5 "properties" layout and falls back to the deprecated fields.
Usage: tools/find-device.py [device name or identifier]"""
import json
import subprocess
import sys
import tempfile


def main():
    wanted = sys.argv[1] if len(sys.argv) > 1 else ""
    with tempfile.NamedTemporaryFile(suffix=".json") as output:
        subprocess.run(["xcrun", "devicectl", "list", "devices", "--json-output", output.name],
                       check=True, stdout=subprocess.DEVNULL)
        devices = json.load(open(output.name))["result"]["devices"]
    for device in devices:
        props = device.get("properties", {})
        hardware = props.get("hardware") or device.get("hardwareProperties", {})
        state = props.get("state", {})
        name = state.get("name") or device.get("deviceProperties", {}).get("name", "")
        if hardware.get("reality") != "physical" or hardware.get("platform") != "iOS":
            continue
        if wanted and wanted not in (device["identifier"], hardware.get("udid"), name):
            continue
        dev_mode = state.get("developerModeStatus") or device.get("deviceProperties", {}).get("developerModeStatus")
        if isinstance(dev_mode, dict):
            dev_mode = next(iter(dev_mode), "unknown")
        connection = (props.get("connection", {}).get("state")
                      or device.get("connectionProperties", {}).get("tunnelState", "unknown"))
        print(device["identifier"], hardware.get("udid", ""), dev_mode or "unknown", connection)
        return


if __name__ == "__main__":
    main()
