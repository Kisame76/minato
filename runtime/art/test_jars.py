#!/usr/bin/env python3
"""A Java test program for ART (ADR-0033 b): its sources compiled with javac against the classes of the boot jars,
never against an SDK's android.jar, then dexed with D8, which gets all classes of the boot jars as its library (as
boot_jars.py does). javac 17 compiles Java 8 here, the newest level that still takes a boot class path of its
own; D8 desugars what Java 8 adds over Android's dex.

Usage: test_jars.py <jdk-home> <r8.jar> <java-work-directory> <output.jar> <source>...
<java-work-directory> is where boot_jars.py left the classes of each boot jar (<jar>.classes.jar) and all of them
(all-classes.jar).
"""
import os
import shutil
import subprocess
import sys
import tempfile

BOOT_JARS = ["core-oj", "core-libart", "core-icu4j"]
D8_FLAGS = ["--release", "--min-api", "31"]


def main():
    if len(sys.argv) < 6:
        raise SystemExit(__doc__)
    jdk, r8, work, output, sources = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
    boot = ":".join(os.path.join(work, f"{jar}.classes.jar") for jar in BOOT_JARS)
    with tempfile.TemporaryDirectory() as scratch:
        classes = os.path.join(scratch, "classes")
        os.makedirs(classes)
        subprocess.run([os.path.join(jdk, "bin", "javac"), "-source", "8", "-target", "8", "-bootclasspath", boot,
                        "-encoding", "UTF-8", "-nowarn", "-Xlint:-options", "-g", "-d", classes] + sources, check=True)
        class_files = sorted(os.path.join(folder, name) for folder, _, names in os.walk(classes)
                             for name in names if name.endswith(".class"))
        dexed = os.path.join(scratch, "dexed.jar")
        subprocess.run([os.path.join(jdk, "bin", "java"), "-cp", r8, "com.android.tools.r8.D8"] + D8_FLAGS +
                       ["--lib", os.path.join(work, "all-classes.jar"), "--output", dexed] + class_files, check=True)
        os.makedirs(os.path.dirname(output), exist_ok=True)
        shutil.move(dexed, output)


if __name__ == "__main__":
    main()
