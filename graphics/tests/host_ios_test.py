#!/usr/bin/env python3
"""ADR-0037 b): graphics/host's sources compile for the iPhone and the Simulator. The app gets graphics/host from
tools/build-graphics-host.sh, which builds it with the presets ios and ios-simulator; CTest runs on the Mac, and the
Simulator's SDK has less Metal than the device's (its MTLDrawable has no presented handlers). Each compile command of
graphics/host in this build's compile_commands.json runs again with -fsyntax-only against both iOS SDKs, at the
presets' deployment target, without the Mac's version, output and sanitizer flags."""
import argparse
import json
import pathlib
import shlex
import subprocess
import sys

# The Mac build's flags that name the Mac or an output, with the number of arguments each takes.
DROPPED = {"-c": 0, "-MD": 0, "-o": 1, "-MT": 1, "-MF": 1, "-isysroot": 1}
DROPPED_PREFIXES = ("-mmacosx-version-min=", "-fsanitize", "-fno-sanitize")
# Each SDK with its presets and its flag for the deployment target.
SDKS = [("iphoneos", "ios", "-mios-version-min="),
        ("iphonesimulator", "ios-simulator", "-mios-simulator-version-min=")]


def deployment_target(presets, name):
    """CMAKE_OSX_DEPLOYMENT_TARGET of a configure preset, through the presets it inherits."""
    preset = next(p for p in presets["configurePresets"] if p["name"] == name)
    value = preset.get("cacheVariables", {}).get("CMAKE_OSX_DEPLOYMENT_TARGET")
    if value is not None:
        return value["value"] if isinstance(value, dict) else value
    inherits = preset.get("inherits", [])
    for parent in [inherits] if isinstance(inherits, str) else inherits:
        found = deployment_target(presets, parent)
        if found is not None:
            return found
    return None


def ios_command(command, sdk_path, version_flag):
    arguments = shlex.split(command)
    kept = [arguments[0]]
    skip = 0
    for argument in arguments[1:]:
        if skip:
            skip -= 1
        elif argument in DROPPED:
            skip = DROPPED[argument]
        elif not argument.startswith(DROPPED_PREFIXES):
            kept.append(argument)
    return kept + ["-fsyntax-only", "-isysroot", sdk_path, version_flag]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--commands", required=True, type=pathlib.Path, help="the Mac build's compile_commands.json")
    parser.add_argument("--sources", required=True, type=pathlib.Path, help="graphics/host")
    parser.add_argument("--presets", required=True, type=pathlib.Path, help="CMakePresets.json")
    args = parser.parse_args()

    sources = args.sources.resolve()
    entries = [entry for entry in json.loads(args.commands.read_text())
               if pathlib.Path(entry["directory"], entry["file"]).resolve().parent == sources]
    expected = {path.name for path in sources.iterdir() if path.suffix in (".c", ".m")}
    missing = expected - {pathlib.Path(entry["file"]).name for entry in entries}
    if missing:
        print(f"FAIL: no compile command for {', '.join(sorted(missing))} in {args.commands}")
        return 1

    presets = json.loads(args.presets.read_text())
    failures = []
    for sdk, preset, flag in SDKS:
        sdk_path = subprocess.run(["xcrun", "--sdk", sdk, "--show-sdk-path"], stdout=subprocess.PIPE, text=True,
                                  check=True).stdout.strip()
        target = deployment_target(presets, preset)
        for entry in entries:
            result = subprocess.run(ios_command(entry["command"], sdk_path, flag + target), cwd=entry["directory"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if result.returncode != 0:
                errors = [line for line in result.stdout.splitlines() if "error:" in line]
                failures.append(f"{pathlib.Path(entry['file']).name} for {sdk} {target}: " + "; ".join(errors[:4]))
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        return 1
    sdks = " and ".join(sdk for sdk, _, _ in SDKS)
    print(f"PASS: graphics.host_ios: {len(entries)} sources of graphics/host for {sdks}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
