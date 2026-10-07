#!/usr/bin/env python3
"""The key labels of KeyEvent's natives (ADR-0039 step c2c3), which libinput's InputEventLabels holds since ADR-0045
step 7c1 (framework/native/android_runtime/input_event_labels.cpp), against KeyEvent.java at android-15.0.0_r1:
kKeyLabels names every KEYCODE_ constant without its prefix at the index of its value,
from KEYCODE_UNKNOWN to LAST_KEYCODE, with no gap, so that keyCodeToString and keyCodeFromString answer as Android's
libinput table does. Skipped while the fetched sources are absent. tools/framework-host-check.sh runs it.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NATIVE = os.path.join(ROOT, "framework", "native", "android_runtime", "input_event_labels.cpp")
KEY_EVENT = os.path.join(ROOT, "build", "sources", "frameworks_base", "core", "java", "android", "view",
                         "KeyEvent.java")


def java_keycodes(text):
    """KeyEvent.java's KEYCODE_ constants as {value: name without the prefix}, and LAST_KEYCODE's name."""
    codes = {}
    for name, value in re.findall(r"public static final int KEYCODE_(\w+)\s*=\s*(\d+);", text):
        if int(value) in codes:
            raise SystemExit(f"KeyEvent.java: KEYCODE_{name} repeats the value {value}")
        codes[int(value)] = name
    last = re.search(r"public static final int LAST_KEYCODE\s*=\s*KEYCODE_(\w+);", text)
    return codes, last.group(1) if last else None


def native_labels(text):
    """kKeyLabels' strings, in order."""
    table = re.search(r"constexpr const char \*kKeyLabels\[\] = \{(.*?)\};", text, re.S)
    if not table:
        raise SystemExit(f"{NATIVE}: no kKeyLabels table")
    return re.findall(r'"([^"]*)"', table.group(1))


def main():
    if not os.path.exists(KEY_EVENT):
        print(f"keycode_labels_test: skipped, {KEY_EVENT} is not fetched")
        return
    with open(KEY_EVENT, encoding="utf-8") as handle:
        codes, last = java_keycodes(handle.read())
    with open(NATIVE, encoding="utf-8") as handle:
        labels = native_labels(handle.read())
    failures = []
    if sorted(codes) != list(range(len(codes))):
        failures.append("KeyEvent.java's key codes are not 0 to n without a gap")
    expected = [codes[value] for value in sorted(codes)]
    if labels != expected:
        first = next((i for i, (a, b) in enumerate(zip(labels, expected)) if a != b), min(len(labels), len(expected)))
        failures.append(f"kKeyLabels has {len(labels)} labels, KeyEvent.java {len(expected)}; first difference at "
                        f"{first}")
    if expected and last != expected[-1]:
        failures.append(f"LAST_KEYCODE is KEYCODE_{last}, the table ends with {expected[-1]}")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        sys.exit(1)
    print(f"keycode_labels_test: fine, {len(labels)} labels")


if __name__ == "__main__":
    main()
