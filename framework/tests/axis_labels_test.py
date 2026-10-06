#!/usr/bin/env python3
"""The axis labels of libandroid_runtime's stand-in for libinput's InputEventLabels.cpp
(framework/native/android_runtime/input_event_labels.cpp, ADR-0045 step 7b1) against MotionEvent.java at
android-15.0.0_r1: kAxisLabels names every AXIS_ constant without its prefix at the index of its value, from AXIS_X to
the highest, and the values no constant has are libinput's RESERVED_<value>, so that axisToString and axisFromString
answer as Android's libinput table does. Skipped while the fetched sources are absent. tools/framework-host-check.sh
runs it.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NATIVE = os.path.join(ROOT, "framework", "native", "android_runtime", "input_event_labels.cpp")
MOTION_EVENT = os.path.join(ROOT, "build", "sources", "frameworks_base", "core", "java", "android", "view",
                            "MotionEvent.java")


def java_axes(text):
    """MotionEvent.java's AXIS_ constants as {value: name without the prefix}."""
    axes = {}
    for name, value in re.findall(r"public static final int AXIS_(\w+)\s*=\s*(\d+);", text):
        if int(value) in axes:
            raise SystemExit(f"MotionEvent.java: AXIS_{name} repeats the value {value}")
        axes[int(value)] = name
    return axes


def native_labels(text):
    """kAxisLabels' strings, in order."""
    table = re.search(r"constexpr const char \*kAxisLabels\[\] = \{(.*?)\};", text, re.S)
    if not table:
        raise SystemExit(f"{NATIVE}: no kAxisLabels table")
    return re.findall(r'"([^"]*)"', table.group(1))


def main():
    if not os.path.exists(MOTION_EVENT):
        print(f"axis_labels_test: skipped, {MOTION_EVENT} is not fetched")
        return
    with open(MOTION_EVENT, encoding="utf-8") as handle:
        axes = java_axes(handle.read())
    with open(NATIVE, encoding="utf-8") as handle:
        labels = native_labels(handle.read())
    expected = [axes.get(value, f"RESERVED_{value}") for value in range(max(axes) + 1)]
    failures = []
    if labels != expected:
        first = next((i for i, (a, b) in enumerate(zip(labels, expected)) if a != b), min(len(labels), len(expected)))
        failures.append(f"kAxisLabels has {len(labels)} labels, MotionEvent.java {len(expected)}; first difference "
                        f"at {first}")
    if [value for value in range(len(expected)) if value not in axes] != [29, 30, 31]:
        failures.append("the values without an AXIS_ constant are not libinput's reserved 29 to 31")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        sys.exit(1)
    print(f"axis_labels_test: fine, {len(labels)} labels")


if __name__ == "__main__":
    main()
