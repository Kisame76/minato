#!/usr/bin/env python3
"""The checks of test 2's APKs, script and twin (ADR-0042 a and c), for CTest's godot.export, godot.script and godot.twin.
Usage: tools/godot-check.py export   checks build/godot/godot-bench-{vulkan,gles}.apk from tools/build-godot.sh
       tools/godot-check.py script   runs the project headless on the Mac for 3 s (tools/build-godot.sh --run)
       tools/godot-check.py twin     checks build/MinatoGodot.xcframework from tools/build-godot.sh --twin
Each prints "skipped:" while tools/fetch-godot.py --fetch has not unpacked the editor. Runs on /usr/bin/python3 (3.9).
export, for each APK:
  - apksigner of tools/fetch-godot.py's build-tools verifies it, with the v2 scheme;
  - its libraries are arm64-v8a only, and libgodot_android.so's LOAD segments are aligned to 16 KB;
  - its dex files name no class of Google Play services, Play's libraries or Firebase;
  - APKKit reads its package, its launcher activity and arm64-v8a (apk-inspect --json);
  - assets/_cl_ names the driver it must draw with (--bench-driver), and the GLES APK's also the Compatibility
    renderer (--rendering-method gl_compatibility);
  - its launcher and splash icons are ours: none equals the template's, which are Godot's logo.
script: a bench.conf of 3 s at 60 Hz gives the script's lines, a scene.csv row per frame and the done file, and exit 0.
twin: the framework exports only the wrapper's entries of twin/exports.txt and holds no _main; its Vulkan calls bind to
  MoltenVK.framework, of which it defines nothing; it links no Swift library; it is an iOS device binary from iOS 26.0.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "godot")
EDITOR = os.path.join(BUILD, "editor", "Godot.app", "Contents", "MacOS", "Godot")
TEMPLATE = os.path.join(BUILD, "editor", "editor_data", "export_templates", "4.7.2.stable", "android_release.apk")
APKSIGNER = os.path.join(BUILD, "sdk", "build-tools", "35.0.1", "apksigner")
JAVA_HOME = os.path.join(ROOT, "build", "tools", "jdk-17", "jdk-17.0.20.1+1", "Contents", "Home")
TWIN = os.path.join(ROOT, "build", "MinatoGodot.xcframework", "ios-arm64", "MinatoGodot.framework")
MOLTENVK = os.path.join(ROOT, "build", "MoltenVK.xcframework", "ios-arm64", "MoltenVK.framework")
APKKIT = os.path.join(ROOT, "importer", "APKKit")
INSPECT_BUILD = os.path.join(BUILD, "apkkit")
APKS = {
    "vulkan": {"package": "io.github.kisame76.minato.godotbench.vulkan", "flags": [b"--bench-driver=vulkan"],
               "not": [b"gl_compatibility"]},
    "gles": {"package": "io.github.kisame76.minato.godotbench.gles",
             "flags": [b"--rendering-method", b"gl_compatibility", b"--bench-driver=opengl3"], "not": []},
}
FORBIDDEN = (b"Lcom/google/android/gms/", b"Lcom/google/android/play/", b"Lcom/android/billingclient/",
             b"Lcom/google/firebase/")
ICON = re.compile(r"^res/(mipmap[^/]*/icon[^/]*|drawable[^/]*/splash_icon)\.(webp|png)$")
PAGE = 16384
failures = []


def check(condition, what):
    print(("ok      " if condition else "FAILED  ") + what, flush=True)
    if not condition:
        failures.append(what)


def load_alignments(elf):
    """The p_align of each LOAD segment of a little-endian ELF64 file."""
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        return None
    phoff, = struct.unpack_from("<Q", elf, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x36)
    alignments = []
    for index in range(phnum):
        kind, = struct.unpack_from("<I", elf, phoff + index * phentsize)
        if kind == 1:
            alignment, = struct.unpack_from("<Q", elf, phoff + index * phentsize + 48)
            alignments.append(alignment)
    return alignments


def apk_inspect():
    """APKKit's apk-inspect, built once into build/godot/apkkit/, apart from the App chat's build."""
    subprocess.run(["swift", "build", "--package-path", APKKIT, "--scratch-path", INSPECT_BUILD, "-c", "release",
                    "--product", "apk-inspect", "-q"], check=True)
    return os.path.join(INSPECT_BUILD, "release", "apk-inspect")


def check_export():
    with zipfile.ZipFile(TEMPLATE) as template:
        template_icons = {name: template.read(name) for name in template.namelist() if ICON.match(name)}
    check(len(template_icons) >= 6, f"the template has its icons ({len(template_icons)} files)")
    inspect = apk_inspect()
    environment = dict(os.environ, JAVA_HOME=JAVA_HOME, PATH=os.path.join(JAVA_HOME, "bin") + os.pathsep + os.environ["PATH"])
    for api, expected in APKS.items():
        apk = os.path.join(BUILD, f"godot-bench-{api}.apk")
        name = os.path.basename(apk)
        if not os.path.isfile(apk):
            check(False, f"{name} exists: run tools/build-godot.sh")
            continue
        verified = subprocess.run([APKSIGNER, "verify", "--verbose", apk], capture_output=True, text=True, env=environment)
        check(verified.returncode == 0 and "Verified using v2 scheme (APK Signature Scheme v2): true" in verified.stdout,
              f"{name}: apksigner verifies it with the v2 scheme")
        with zipfile.ZipFile(apk) as zipped:
            names = zipped.namelist()
            abis = {entry.split("/")[1] for entry in names if entry.startswith("lib/") and entry.count("/") == 2}
            check(abis == {"arm64-v8a"}, f"{name}: libraries for arm64-v8a only ({', '.join(sorted(abis)) or 'none'})")
            library = "lib/arm64-v8a/libgodot_android.so"
            alignments = load_alignments(zipped.read(library)) if library in names else None
            check(bool(alignments) and all(alignment % PAGE == 0 for alignment in alignments),
                  f"{name}: libgodot_android.so's LOAD segments are aligned to 16 KB ({alignments})")
            dex = [entry for entry in names if re.match(r"^classes[0-9]*\.dex$", entry)]
            found = sorted({prefix.decode() for entry in dex for prefix in FORBIDDEN if prefix in zipped.read(entry)})
            check(bool(dex) and not found, f"{name}: no Play services, Play libraries or Firebase in its dex ({found or 'none'})")
            flags = zipped.read("assets/_cl_") if "assets/_cl_" in names else b""
            check(all(flag in flags for flag in expected["flags"]) and not any(flag in flags for flag in expected["not"]),
                  f"{name}: assets/_cl_ names {b' '.join(expected['flags']).decode()}")
            icons = {entry: zipped.read(entry) for entry in names if ICON.match(entry)}
            same = sorted(entry for entry, data in icons.items() if template_icons.get(entry) == data)
            check(set(icons) == set(template_icons) and not same, f"{name}: every launcher and splash icon is ours ({same or 'none'} equal the template's)")
        report = subprocess.run([inspect, "--json", apk], capture_output=True, text=True)
        try:
            fields = json.loads(report.stdout)
        except ValueError:
            fields = {}
        check(fields.get("packageName") == expected["package"] and bool(fields.get("launcherActivity")) and
              fields.get("selectedABI") == "arm64-v8a",
              f"{name}: APKKit reads {expected['package']}, a launcher activity and arm64-v8a "
              f"({fields.get('packageName')}, {fields.get('launcherActivity')}, {fields.get('selectedABI')})")


def check_script():
    os.makedirs(BUILD, exist_ok=True)
    folder = tempfile.mkdtemp(prefix="godot-script-", dir=BUILD)
    conf = os.path.join(folder, "bench.conf")
    with open(conf, "w") as handle:
        handle.write("run=godot-script\nrate=60\nload=light\nseconds=3\n")
    script = os.path.join(ROOT, "tools", "build-godot.sh")
    if not os.access(script, os.X_OK):
        check(False, "tools/build-godot.sh exists")
        return
    start = int(time.time())
    run = subprocess.run([script, "--run", conf], capture_output=True, text=True, timeout=300)
    # Godot's user:// on macOS follows $HOME alone: the run's log must land in build/godot/home, not in the user's.
    log = os.path.join("Library", "Application Support", "Godot", "app_userdata", "godot-bench", "logs", "godot.log")
    inside = os.path.join(BUILD, "home", log)
    outside = os.path.join(os.path.expanduser("~"), log)
    check(os.path.isfile(inside) and os.path.getmtime(inside) >= start, "user:// lies in build/godot/home (the run's log)")
    check(not os.path.isfile(outside) or os.path.getmtime(outside) < start, "the run writes nothing to ~/Library's user://")
    lines = run.stdout.splitlines()
    check(run.returncode == 0, f"the project ends with exit status 0 ({run.returncode})")
    check("godot-bench: start godot-script rate 60 load light seconds 3 meshes 512" in lines, "the start line names bench.conf's run")
    check(any(line.startswith("godot-bench: rendering ") for line in lines), "a line names the renderer and driver")
    seconds = [line for line in lines if re.match(r"^godot-bench-device: second [0-9]+: [0-9]+ frames$", line)]
    check(len(seconds) >= 2, f"a line a second ({len(seconds)})")
    ends = [re.match(r"^run godot-script: ([1-9][0-9]*) frames, 0 failures$", line) for line in lines]
    ends = [match for match in ends if match]
    frames = int(ends[0].group(1)) if len(ends) == 1 else 0
    check(len(ends) == 1 and 150 <= frames <= 200, f"one run line, with about 3 s of 60 frames ({frames})")
    scene = os.path.join(folder, "scene.csv")
    rows = open(scene).read().splitlines() if os.path.isfile(scene) else []
    indices = [int(row.split(",")[0]) for row in rows[1:]] if rows else []
    ticks = [int(row.split(",")[1]) for row in rows[1:]] if rows else []
    check(rows[:1] == ["index,tick_ns,cpu_ns"] and indices == list(range(frames)) and ticks == sorted(ticks),
          f"scene.csv has a row per frame, in order ({len(indices)} rows)")
    done = os.path.join(folder, "done")
    check(os.path.isfile(done) and open(done).read() == f"{frames} frames\n", "the done file follows scene.csv")
    if failures:
        print(run.stdout[-3000:] + run.stderr[-2000:], flush=True)
    else:
        shutil.rmtree(folder)


def tool(*command):
    return subprocess.run(command, capture_output=True, text=True, check=True).stdout


def check_twin():
    """The twin's framework (ADR-0042 decision 3, c) 1): Godot's iOS template library with our wrapper, linked
    against our MoltenVK 1.4.2 instead of the template's own."""
    binary = os.path.join(TWIN, "MinatoGodot")
    if not os.path.isfile(binary):
        check(False, "build/MinatoGodot.xcframework exists: run tools/build-godot.sh --twin")
        return
    with open(os.path.join(ROOT, "fixtures", "apps", "godot-bench", "twin", "exports.txt")) as handle:
        entries = {line.strip() for line in handle if line.strip() and not line.startswith("#")}
    exported = {line.split()[-1] for line in tool("nm", "-gU", binary).splitlines() if line.strip()}
    check(bool(entries) and exported == entries, f"it exports only the wrapper's entries ({', '.join(sorted(exported))})")
    names = {line.split()[-1] for line in tool("nm", binary).splitlines() if line.strip()}
    check("_main" not in names, "it holds no _main, so app.swift's @main stays out")
    undefined = [line for line in tool("nm", "-m", "-u", binary).splitlines() if " _vk" in line]
    elsewhere = [line.split()[2] for line in undefined if "(from MoltenVK)" not in line]
    check(len(undefined) >= 100 and not elsewhere, f"its {len(undefined)} Vulkan calls bind to MoltenVK.framework ({elsewhere or 'all'})")
    moltenvk = {line.split()[-1] for line in tool("nm", "-gU", os.path.join(MOLTENVK, "MoltenVK")).splitlines() if line.strip()}
    defined = {line.split()[-1] for line in tool("nm", "-U", binary).splitlines() if line.strip()}
    inside = sorted(defined & moltenvk)
    check(len(moltenvk) > 100 and not inside, f"it defines none of MoltenVK's symbols, so no second MoltenVK ({inside[:5] or 'none'})")
    libraries = tool("otool", "-L", binary)
    check("@rpath/MoltenVK.framework/MoltenVK" in libraries and "libswift" not in libraries and "SwiftUI" not in libraries,
          "it links @rpath/MoltenVK.framework and no Swift library")
    commands = tool("otool", "-l", binary)
    check(re.search(r"cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform 2\s+minos 26\.0", commands) is not None,
          "it is built for iOS devices, from iOS 26.0")
    plist = os.path.join(TWIN, "Info.plist")
    check(os.path.isfile(plist) and tool("plutil", "-extract", "CFBundleExecutable", "raw", plist).strip() == "MinatoGodot",
          "its Info.plist names MinatoGodot")


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("export", "script", "twin"):
        raise SystemExit("usage: tools/godot-check.py export|script|twin")
    if not os.access(EDITOR, os.X_OK):
        print("skipped: no Godot editor in build/godot: run tools/fetch-godot.py --fetch", flush=True)
        return
    if sys.argv[1] == "export":
        check_export()
    elif sys.argv[1] == "script":
        check_script()
    else:
        check_twin()
    if failures:
        raise SystemExit(f"godot-check: {len(failures)} failed")
    print("godot-check: passed", flush=True)


if __name__ == "__main__":
    main()
