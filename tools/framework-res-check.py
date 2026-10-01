#!/usr/bin/env python3
"""Checks our framework-res (ADR-0039 b2, decision 8) as tools/build-framework-res.sh leaves it in build/framework-res:

  public IDs   every resource of core/res's public-final.xml with its ID, and every name of public-staging.xml's
               open staging groups at its group's first ID plus its place, is in our table under that ID and
               public; every package of the table (the second holds the staging groups) is "android" with ID 01
               (the tag's sources, no device)
  the folder   /system/framework/framework-res/ as ApkAssets.loadFromDir opens it: AndroidManifest.xml and
               resources.arsc, with the APK's bytes
  reference    with --reference <framework-res.apk pulled from the AVD>: every resource of the AVD's table is in
               ours under the same name and ID, with the same value in every configuration, and ours has no other;
               a staged resource appears under its staged and its final ID, so a resource is its name and ID; and
               the two manifests' trees are the same but for source line numbers, and resources.arsc is the
               same, byte for byte

Tables are read with `aapt2 dump resources` of build/aapt2/aapt2.

Usage: tools/framework-res-check.py [--reference <apk>]
"""
import argparse
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AAPT2 = os.path.join(ROOT, "build", "aapt2", "aapt2")
OUT = os.path.join(ROOT, "build", "framework-res")
APK = os.path.join(OUT, "framework-res.apk")
FOLDER = os.path.join(OUT, "framework-res")
VALUES = os.path.join(ROOT, "build", "sources", "frameworks_base", "core", "res", "res", "values")
RESOURCE = re.compile(r"^resource (0x[0-9a-f]{8}) ([^/ ]+)/(\S+)(.*)$")
PACKAGE = re.compile(r"^Package name=(\S+)(?: id=([0-9a-f]{2}))?$")


def dump(apk):
    """The table of an APK: its package lines, and per ('type/name', ID) its flags (PUBLIC, STAGED, …) and the lines
    of its values in every configuration, as `aapt2 dump resources` prints them."""
    output = subprocess.run([AAPT2, "dump", "resources", apk], capture_output=True, text=True, check=True).stdout
    packages, resources, current = [], {}, None
    for raw in output.splitlines():
        line = raw.strip()
        match = PACKAGE.match(line)
        if match:
            packages.append((match.group(1), match.group(2)))
            current = None
            continue
        match = RESOURCE.match(line)
        if match:
            current = (f"{match.group(2)}/{match.group(3)}", int(match.group(1), 16))
            resources[current] = {"flags": match.group(4).split(), "values": []}
            continue
        if current and not line.startswith("type "):
            resources[current]["values"].append(line)
    return packages, resources


def public_ids():
    """'type/name' to its ID from public-final.xml and public-staging.xml."""
    ids = {}
    final = ElementTree.parse(os.path.join(VALUES, "public-final.xml")).getroot()
    for element in final.iter("public"):
        if element.get("id"):
            ids[f"{element.get('type')}/{element.get('name')}"] = int(element.get("id"), 16)
    staging = ElementTree.parse(os.path.join(VALUES, "public-staging.xml")).getroot()
    for group in staging.iter("staging-public-group"):
        first = int(group.get("first-id"), 16)
        for place, element in enumerate(group.iter("public")):
            ids[f"{group.get('type')}/{element.get('name')}"] = first + place
    return ids


def main():
    parser = argparse.ArgumentParser(description="Checks our framework-res (ADR-0039 b2).")
    parser.add_argument("--reference", help="framework-res.apk pulled from the AVD")
    arguments = parser.parse_args()
    for path, how in ((AAPT2, "tools/build-aapt2.sh"), (APK, "tools/build-framework-res.sh")):
        if not os.path.exists(path):
            print(f"No {os.path.relpath(path, ROOT)}: run {how} first.", file=sys.stderr)
            return 1

    failed = 0
    packages, ours = dump(APK)
    if not packages or any(package != ("android", "01") for package in packages):
        print(f"FAILED   package: {packages}, not android with ID 01")
        failed += 1

    ids = public_ids()
    wrong = [f"{name} 0x{expected:08x}" for name, expected in sorted(ids.items())
             if (name, expected) not in ours or "PUBLIC" not in ours[(name, expected)]["flags"]]
    if wrong:
        print(f"FAILED   public IDs: {len(wrong)} of {len(ids)} wrong, the first: {wrong[:5]}")
        failed += 1
    else:
        print(f"ok       public IDs: {len(ids)} resources at their IDs, public")

    with zipfile.ZipFile(APK) as apk:
        for name in ("AndroidManifest.xml", "resources.arsc"):
            path = os.path.join(FOLDER, name)
            if not os.path.isfile(path) or open(path, "rb").read() != apk.read(name):
                print(f"FAILED   folder: {os.path.relpath(path, ROOT)} is not the APK's {name}")
                failed += 1
    if not failed:
        print("ok       folder: framework-res/ holds the APK's manifest and table")

    if arguments.reference:
        their_packages, theirs = dump(arguments.reference)
        if their_packages != packages:
            print(f"FAILED   reference, packages: {their_packages} -> {packages}")
            failed += 1
        def name(key):
            return f"{key[0]} 0x{key[1]:08x}"
        missing = [name(key) for key in theirs if key not in ours]
        flagged = [f"{name(key)}: {theirs[key]['flags']} -> {ours[key]['flags']}" for key in theirs
                   if key in ours and theirs[key]["flags"] != ours[key]["flags"]]
        changed = [f"{name(key)}: {theirs[key]['values'][:3]} -> {ours[key]['values'][:3]}" for key in theirs
                   if key in ours and theirs[key]["values"] != ours[key]["values"]]
        extra = [name(key) for key in ours if key not in theirs]
        for what, items in (("missing", missing), ("other flags", flagged), ("other values", changed),
                            ("extra", extra)):
            if items:
                print(f"FAILED   reference, {what}: {len(items)}, the first: {items[:3]}")
                failed += 1
        with zipfile.ZipFile(arguments.reference) as reference, zipfile.ZipFile(APK) as apk:
            their_table, our_table = reference.read("resources.arsc"), apk.read("resources.arsc")
        if their_table != our_table:
            print(f"FAILED   reference, resources.arsc: {len(our_table)} bytes, the AVD's {len(their_table)}, "
                  "not the same bytes")
            failed += 1
        else:
            print(f"ok       reference: resources.arsc byte for byte ({len(our_table)} bytes)")
        def tree(apk):
            output = subprocess.run([AAPT2, "dump", "xmltree", "--file", "AndroidManifest.xml", apk],
                                    capture_output=True, text=True, check=True).stdout
            return [re.sub(r" \(line=\d+\)", "", line) for line in output.splitlines()]
        their_tree, our_tree = tree(arguments.reference), tree(APK)
        if their_tree != our_tree:
            only_theirs = [line.strip() for line in their_tree if line not in our_tree]
            only_ours = [line.strip() for line in our_tree if line not in their_tree]
            print(f"FAILED   reference, manifest: only the AVD's {only_theirs[:3]}, only ours {only_ours[:3]}")
            failed += 1
        else:
            print("ok       reference: the same manifest")
        if not (missing or flagged or changed or extra):
            print(f"ok       reference: all {len(theirs)} resources of the AVD's framework-res, same IDs, flags and "
                  "values in every configuration")

    print("framework-res check: fine" if not failed else f"framework-res check: {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
