#!/usr/bin/env python3
"""Exits 0, printing nothing, when the paired physical iOS device, the first one or the one whose identifier, UDID or
name is given, is reachable by cable or over the local network, and 1 with a message otherwise. The iPhone takes
installs and runs over USB or the local network (the user's rule of 2026-10-28, which lifted that of 2026-10-23, by
cable only), still each sitting only after the device's owner agrees, so each script that reaches it calls this first.
It fails closed: a devicectl that fails, JSON it cannot read, a device that is not listed, one that is not paired and a
transport other than "wired" or "localNetwork" all stop the script. devicectl's JSON v5 has the transport and the
pairing in "properties", the deprecated fields have them too, and each one that is there must say so. The tunnel's state
is not asked: devicectl reports it "disconnected" between its calls and connects it at the next. Usage:
tools/require-device.py [device name or identifier]"""
import json
import subprocess
import sys
import tempfile

TRANSPORTS = ("wired", "localNetwork")


def stop(message):
    print(f"Stopped, the iPhone is not reachable: {message} Connect it by cable or to this Mac's network, paired, and "
          "run this again.", file=sys.stderr)
    sys.exit(1)


def main():
    wanted = sys.argv[1] if len(sys.argv) > 1 else ""
    try:
        with tempfile.NamedTemporaryFile(suffix=".json") as output:
            listing = subprocess.run(["xcrun", "devicectl", "list", "devices", "--json-output", output.name],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            if listing.returncode != 0:
                stop(f"devicectl could not list the devices ({listing.stderr.strip() or listing.returncode}).")
            devices = json.load(open(output.name))["result"]["devices"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        stop(f"devicectl's list of the devices could not be read ({error}).")
    for device in devices:
        props = device.get("properties") or {}
        hardware = props.get("hardware") or device.get("hardwareProperties") or {}
        name = (props.get("state") or {}).get("name") or (device.get("deviceProperties") or {}).get("name", "")
        if hardware.get("reality") != "physical" or hardware.get("platform") != "iOS":
            continue
        if wanted and wanted not in (device.get("identifier"), hardware.get("udid"), name):
            continue
        sections = [section for section in (props.get("connection"), device.get("connectionProperties"))
                    if isinstance(section, dict)]
        transports = [section["transportType"] for section in sections if "transportType" in section]
        if not transports or any(transport not in TRANSPORTS for transport in transports):
            over = ", ".join(sorted(set(map(str, transports)))) or "nothing"
            stop(f"{name or device.get('identifier')} is connected over {over}.")
        pairings = [section["pairingState"] for section in sections if "pairingState" in section]
        if not pairings or any(pairing != "paired" for pairing in pairings):
            states = ", ".join(sorted(set(map(str, pairings)))) or "unknown"
            stop(f"{name or device.get('identifier')} is not paired ({states}).")
        return
    stop(f"no paired iPhone{' ' + repr(wanted) if wanted else ''} is listed.")


if __name__ == "__main__":
    main()
