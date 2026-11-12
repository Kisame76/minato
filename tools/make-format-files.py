#!/usr/bin/env python3
"""Builds one file of each import format from our own fixtures into one folder (default build/format-files), never from
a user APK: a plain APK, bundletool's .apks, a plain zip of APKs, an .xapk with two OBBs, an .apkm, a .apks with asset
packs, a loose set in a folder, and an APK with its OBBs to import apart. They are what a device pass picks with "+"
and Open with, and what FormatFilesTests imports on the Mac (MINATO_FORMAT_FILES=<folder>). Run from the repo root,
after what tools/format-files-check.sh builds first.
Usage: tools/make-format-files.py [out-folder]
"""
import json
import os
import shutil
import sys
import zipfile

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/format-files"
SET = "build/splits-test/set"  # tools/build-splits-test.sh: base.apk, split_config.*.apk, split_feature.apk
BUNDLETOOL_APKS = "build/splits-test/splits-test.apks"
PACKS_APKS = "build/packs-test/packs-test.apks"  # tools/build-packs-test.sh
PLAIN_APK = "build/java-test/java-test.apk"  # tools/build-java-test-app.sh java-test
OBB_APK = "build/obb-test/obb-test.apk"  # tools/build-java-test-app.sh obb-test
OBB_DIR = "fixtures/apps/obb-test"
OBB_PACKAGE = "io.github.kisame76.minato.test.obb"
SPLITS_PACKAGE = "io.github.kisame76.minato.test.splits"


def have(path):
    if not os.path.exists(path):
        print("missing %s (build it first)" % path)
    return os.path.exists(path)


def zip_of(name, entries):
    with zipfile.ZipFile(os.path.join(OUT, name), "w", zipfile.ZIP_DEFLATED) as archive:
        for entry, source in entries:
            if isinstance(source, bytes):
                archive.writestr(entry, source)
            else:
                archive.write(source, entry)


def copy(source, name):
    shutil.copy(source, os.path.join(OUT, name))


def obb_files():
    return sorted(name for name in os.listdir(OBB_DIR) if name.endswith(".obb"))


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
set_files = sorted(f for f in os.listdir(SET) if f.endswith(".apk")) if have(SET) else []
set_entries = [(name, os.path.join(SET, name)) for name in set_files]
made = []

if have(PLAIN_APK):
    copy(PLAIN_APK, "01-plain.apk")
    made.append("01-plain.apk")
if have(BUNDLETOOL_APKS):
    copy(BUNDLETOOL_APKS, "02-bundletool.apks")
    made.append("02-bundletool.apks")
if set_files:
    # A plain zip of APKs, the shape of SAI's and App Manager's .apks: no marker file, the APKs make a loose set.
    zip_of("03-zip-of-apks.apks", set_entries)
    made.append("03-zip-of-apks.apks")
    info = {"apkm_version": 4, "pname": SPLITS_PACKAGE, "app_name": "splits-test", "versioncode": "1"}
    zip_of("05-splits.apkm", [("info.json", json.dumps(info).encode())] + set_entries)
    made.append("05-splits.apkm")
    os.makedirs(os.path.join(OUT, "07-loose-set"))
    for name, source in set_entries:
        shutil.copy(source, os.path.join(OUT, "07-loose-set", name))
    made.append("07-loose-set/")
if have(OBB_APK):
    paths = ["Android/obb/%s/%s" % (OBB_PACKAGE, name) for name in obb_files()]
    manifest = {
        "xapk_version": 2, "package_name": OBB_PACKAGE, "version_code": "1",
        "split_apks": [{"file": OBB_PACKAGE + ".apk", "id": "base"}],
        "expansions": [{"file": p, "install_location": "EXTERNAL_STORAGE", "install_path": p} for p in paths],
    }
    entries = [("manifest.json", json.dumps(manifest, sort_keys=True).encode()), (OBB_PACKAGE + ".apk", OBB_APK)]
    entries += [(p, os.path.join(OBB_DIR, os.path.basename(p))) for p in paths]
    zip_of("04-with-obb.xapk", entries)
    made.append("04-with-obb.xapk")
    os.makedirs(os.path.join(OUT, "08-obb-later"))
    shutil.copy(OBB_APK, os.path.join(OUT, "08-obb-later", "obb-test.apk"))
    for name in obb_files():
        shutil.copy(os.path.join(OBB_DIR, name), os.path.join(OUT, "08-obb-later", name))
    made.append("08-obb-later/")
if have(PACKS_APKS):
    copy(PACKS_APKS, "06-asset-packs.apks")
    made.append("06-asset-packs.apks")

print("\n".join(made))
