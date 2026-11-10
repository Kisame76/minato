#!/usr/bin/env python3
"""Packs obb-large-test's APK and a main OBB of 2 GiB and 128 MiB into an .xapk, as APKPure packs one, for the import of a
big expansion file on a device (tools/java-start-check.sh with LARGE_XAPK=1). The OBB is the one tools/make-sparse-obb.py
makes (zeros with eight-byte tags at the offsets obb-large-test reads), compressed on the way in, so the .xapk is a few
MiB big and the import has to inflate the whole OBB into the app, which is what a store's .xapk of a game makes it do.
Nothing but the .xapk is written (streamed, 16 MiB at a time); it prints the .xapk's path and sizes.

Usage: tools/make-large-xapk.py <obb-large-test.apk> <output .xapk>
"""
import json
import sys
import zipfile

PACKAGE = "io.github.kisame76.minato.test.obblarge"
GIB2 = 1 << 31
SIZE = GIB2 + (128 << 20)
TAGS = [(0, b"FIRST-08"), (GIB2 - 4, b"SPANNING"), (GIB2 + 4096, b"ABOVE2G!"), (SIZE - 8, b"THE-END!")]
CHUNK = 16 << 20


def obb_chunks():
    """The OBB's bytes in parts: zeros, with each tag at its offset (a tag may cross a part's edge)."""
    tags = sorted(TAGS)
    offset = 0
    while offset < SIZE:
        step = min(CHUNK, SIZE - offset)
        part = bytearray(step)
        for at, tag in tags:
            lo, hi = max(at, offset), min(at + len(tag), offset + step)
            if lo < hi:
                part[lo - offset:hi - offset] = tag[lo - at:hi - at]
        yield bytes(part)
        offset += step


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    apk, output = sys.argv[1:]
    obb = f"Android/obb/{PACKAGE}/main.1.{PACKAGE}.obb"
    manifest = {"xapk_version": 2, "package_name": PACKAGE, "name": "OBB Large Test", "version_code": "1",
                "version_name": "1.0", "split_apks": [{"file": f"{PACKAGE}.apk", "id": "base"}],
                "expansions": [{"file": obb, "install_location": "EXTERNAL_STORAGE", "install_path": obb}]}
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as out:
        out.writestr("manifest.json", json.dumps(manifest))
        out.write(apk, f"{PACKAGE}.apk")
        with out.open(obb, "w", force_zip64=True) as entry:
            for part in obb_chunks():
                entry.write(part)
    with zipfile.ZipFile(output) as check:
        packed = sum(info.compress_size for info in check.infolist())
    print(f"{output}: {packed} bytes packed, the OBB {SIZE} bytes")


if __name__ == "__main__":
    main()
