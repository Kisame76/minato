#!/usr/bin/env python3
"""tools/check-guest-fresh.py: the guest outputs a release packs (build/bionic, art, framework, framework-native, hwui,
graphics/guest) must not be older than the sources they are built from, or the release would ship stale libraries.
On a small tree it writes: all fresh passes; a source newer than its output, a missing output and a missing marker
file fail and name the pair; a missing source folder and an old file in a folder the build does not read are ignored.
Python 3.9."""
import os
import subprocess
import sys
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "check-guest-fresh.py")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        print("FAIL", what)
        failures += 1


def touch(root, path, time):
    full = os.path.join(root, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "a"):
        pass
    os.utime(full, (time, time))


def run(root):
    result = subprocess.run([sys.executable, TOOL, root], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def make(root, output_time, source_time):
    """Every output at output_time, every source at source_time."""
    for path in ["build/bionic/lib/libc.so", "build/art/lib/libart.so", "build/framework/framework.jar",
                 "build/framework-native/libandroid_runtime.so", "build/hwui/lib/libhwui.so",
                 "build/graphics/guest/bin/libEGL.so"]:
        touch(root, path, output_time)
    for path in ["runtime/bionic/CMakeLists.txt", "runtime/art/CMakeLists.txt", "framework/java/android/app/A.java",
                 "framework/native/android_runtime/B.cpp", "graphics/hwui/CMakeLists.txt",
                 "graphics/guest/CMakeLists.txt", "tools/build-bionic.sh"]:
        touch(root, path, source_time)


with tempfile.TemporaryDirectory() as root:
    make(root, 2000, 1000)
    code, text = run(root)
    check(code == 0, "fresh outputs pass: " + text)

    touch(root, "framework/java/android/app/Newer.java", 3000)
    code, text = run(root)
    check(code == 1 and "build/framework/framework.jar" in text and "framework/java/android/app/Newer.java" in text,
          "a source newer than framework.jar fails and names both: " + text)
    check("build/bionic" not in text, "the fresh ones are not named: " + text)
    touch(root, "framework/java/android/app/Newer.java", 1500)

    touch(root, "docs/NOTES.md", 9000)
    touch(root, "runtime/tests/audio_test.cpp", 9000)
    code, text = run(root)
    check(code == 0, "a file in a folder the build does not read is ignored: " + text)

    os.remove(os.path.join(root, "build/hwui/lib/libhwui.so"))
    code, text = run(root)
    check(code == 1 and "build/hwui/lib/libhwui.so" in text and "missing" in text, "a missing output fails: " + text)
    touch(root, "build/hwui/lib/libhwui.so", 2000)

    os.remove(os.path.join(root, "graphics/guest/CMakeLists.txt"))
    os.rmdir(os.path.join(root, "graphics/guest"))
    code, text = run(root)
    check(code == 0, "a source folder that is not there is ignored: " + text)

if failures:
    sys.exit(1)
print("check_guest_fresh_test: fine")
