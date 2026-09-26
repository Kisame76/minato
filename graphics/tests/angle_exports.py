#!/usr/bin/env python3
"""Test 3 of ADR-0029: every function that Android's libEGL.so, libGLESv2.so and libGLESv3.so export
(NDK r30 stubs, API 35) is exported by all three slices of ANGLE's libGLESv2, EGL ones as EGL_* and GLES
ones under their own names, or it is on the stub list, and nothing on the stub list is exported.
Prints "skipped:" while ANGLE's checkout or one of its three slices is missing, so that CTest stays green
for everyone else while ANGLE is being built."""
import argparse
import glob
import os
import subprocess
import sys

SLICES = {
    "ios": "out/ios/libGLESv2.framework/libGLESv2",
    "ios-simulator": "out/ios-simulator/libGLESv2.framework/libGLESv2",
    "macos": "out/macos/libGLESv2.dylib",
}
LIBRARIES = ["libEGL.so", "libGLESv2.so", "libGLESv3.so"]


def ndk_names(ndk):
    prebuilt = glob.glob(os.path.join(ndk, "toolchains", "llvm", "prebuilt", "*"))
    if not prebuilt:
        raise SystemExit(f"angle_exports: no NDK at {ndk}; set ANDROID_NDK or APK_ANDROID_NDK")
    nm = os.path.join(prebuilt[0], "bin", "llvm-nm")
    stubs = os.path.join(prebuilt[0], "sysroot", "usr", "lib", "aarch64-linux-android", "35")
    names = set()
    for library in LIBRARIES:
        result = subprocess.run([nm, "-D", "--defined-only", "--format=just-symbols", os.path.join(stubs, library)],
                                check=True, capture_output=True, text=True)
        names.update(line.strip() for line in result.stdout.splitlines() if line.strip())
    return names


def exported(binary):
    result = subprocess.run(["nm", "-gU", "-j", binary], check=True, capture_output=True, text=True)
    return {line.strip()[1:] for line in result.stdout.splitlines() if line.strip().startswith("_")}


def angle_name(name):
    return "EGL_" + name[3:] if name.startswith("egl") else name


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--angle", required=True)
    parser.add_argument("--ndk", required=True)
    parser.add_argument("--stubs", required=True)
    options = parser.parse_args()
    if not os.path.isdir(options.angle):
        print(f"skipped: no ANGLE checkout in {options.angle}, see tools/build-angle.sh")
        return 0

    unbuilt = [name for name, path in SLICES.items() if not os.path.exists(os.path.join(options.angle, path))]
    if unbuilt:
        print(f"skipped: slices not built yet: {', '.join(unbuilt)}, see tools/build-angle.sh")
        return 0

    names = ndk_names(options.ndk)
    stubbed = set()
    if os.path.exists(options.stubs):
        with open(options.stubs) as handle:
            for line in handle:
                name = line.split("#", 1)[0].strip()
                if name:
                    stubbed.add(name)
    problems = sorted("on the stub list but not exported by Android: " + name for name in stubbed - names)

    exports = {name: exported(os.path.join(options.angle, path)) for name, path in SLICES.items()}
    for name in sorted(names):
        missing = [slice_ for slice_, symbols in exports.items() if angle_name(name) not in symbols]
        if name in stubbed:
            if len(missing) < len(exports):
                problems.append(f"on the stub list but exported as {angle_name(name)}: {name}")
        elif missing:
            problems.append(f"not exported as {angle_name(name)} by {', '.join(missing)} and not on the stub list: {name}")
    egl = sum(1 for name in names if name.startswith("egl"))
    print(f"angle_exports: {egl} EGL and {len(names) - egl} GLES functions, {len(stubbed)} on the stub list, "
          f"slices checked: {', '.join(sorted(exports)) or 'none'}")
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
