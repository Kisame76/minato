#!/usr/bin/env python3
"""Taps or long-presses the element of the emulator's screen whose text or description is exactly TEXT.

Usage: tools/ui-pick.py <adb> <serial> tap|hold <text> [seconds [index]]
       tools/ui-pick.py <adb> <serial> download
Dumps the screen with uiautomator every half second for up to `seconds` (default 20) until the element shows, then taps
its middle (hold: presses it for a second); with an index, the index-th element (from 0, top to bottom, left to right)
when several have the text, as the photo picker's thumbnails do. Elements are chosen by their text, never by a place on the screen, so that
the Android reference (tools/android-reference.sh) drives the system pickers whatever the AVD's display. Exits 1 when
the element does not show. A text that begins with "~" matches the start of a text or description.
"download" brings DocumentsUI to the internal storage's Download folder, where it may already be, as it opens where it
was last.
"""
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


def dump(adb, serial):
    run = lambda *args: subprocess.run([adb, "-s", serial, *args], capture_output=True, text=True, stdin=subprocess.DEVNULL)
    run("shell", "uiautomator", "dump", "/sdcard/ui-pick.xml")
    text = run("shell", "cat", "/sdcard/ui-pick.xml").stdout
    try:
        return ET.fromstring(text)
    except ET.ParseError:
        return None


def matches(node, text):
    """Whether the node's text or description is `text`, or starts with it when it begins with "~"."""
    values = (node.get("text") or "", node.get("content-desc") or "")
    return any(v.startswith(text[1:]) for v in values) if text.startswith("~") else text in values


def shows(adb, serial, text):
    root = dump(adb, serial)
    return root is not None and any(matches(n, text) for n in root.iter("node"))


def main():
    if len(sys.argv) == 4 and sys.argv[3] == "download":
        adb, serial = sys.argv[1:3]
        for _ in range(40):
            if shows(adb, serial, "Files in Download"):
                return
            if shows(adb, serial, "Show roots"):
                break
            time.sleep(0.5)
        # Where DocumentsUI was last, its path shows above the list: Download is in it, or below Download.
        if shows(adb, serial, "Download"):
            subprocess.run([sys.executable, __file__, adb, serial, "tap", "Download"], check=True)
            return
        for step in ("Show roots", "Android SDK built for arm64", "Download"):
            subprocess.run([sys.executable, __file__, adb, serial, "tap", step], check=True)
        return
    if len(sys.argv) not in (5, 6, 7) or sys.argv[3] not in ("tap", "hold"):
        raise SystemExit(__doc__)
    adb, serial, action, text = sys.argv[1:5]
    deadline = time.time() + (float(sys.argv[5]) if len(sys.argv) >= 6 else 20)
    index = int(sys.argv[6]) if len(sys.argv) == 7 else 0
    while True:
        root = dump(adb, serial)
        found = [node for node in (root.iter("node") if root is not None else ()) if matches(node, text)]
        found.sort(key=lambda n: [int(v) for v in re.findall(r"\d+", n.get("bounds"))][1::-1])
        if len(found) > index:
            left, top, right, bottom = map(int, re.findall(r"\d+", found[index].get("bounds")))
            x, y = (left + right) // 2, (top + bottom) // 2
            press = ["shell", "input", "swipe", str(x), str(y), str(x), str(y), "1000"] if action == "hold" \
                else ["shell", "input", "tap", str(x), str(y)]
            subprocess.run([adb, "-s", serial, *press], stdin=subprocess.DEVNULL, check=True)
            return
        if time.time() > deadline:
            print(f"ui-pick: no '{text}' on the screen", file=sys.stderr)
            raise SystemExit(1)
        time.sleep(0.5)


main()
