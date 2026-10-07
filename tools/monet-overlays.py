#!/usr/bin/env python3
"""Writes Monet's overlays (ADR-0043 f)) into a folder that Minato mounts as /data/resource-cache, as Android 15's
SystemUI and idmap2 write them there on the reference AVD: for each overlay of framework/monet-overlays.txt, its
fabricated overlay (.frro) and the idmap through which libandroidfw loads it, the values inline.
Usage: tools/monet-overlays.py <framework-res> <out>, with <framework-res> tools/build-framework-res.sh's
build/framework-res (its APK for the target's CRC, its R classes for the resource IDs). Python 3.9.

The formats are those at android-15.0.0_r1:
  - .frro (cmds/idmap2/libidmap2/FabricatedOverlay.cpp, ToBinaryStream): magic "FRRO", version 3, the CRC-32 of the
    version and the protobuf, the size of the binary data (none), an empty UTF-8 string pool (libandroidfw's
    StringPool::FlattenUtf8), then fabricated_v1.proto's FabricatedOverlay, its entries in the order of package, type
    and name. The AVD's three are these files byte for byte.
  - idmap (libs/androidfw/Idmap.cpp, LoadedIdmap::Load): version 9; the target's CRC as idmap2's GetPackageCrc makes
    it, resources.arsc's XOR AndroidManifest.xml's; the overlay's CRC, the .frro's; the policies and enforcement that
    SystemUI's overlays get (0x13: public, system and signature; enforced); the paths, the name and no debug info; then
    each entry inline for the one default configuration, in the order of the target's IDs, and the .frro's string
    pool. The target path is framework-res's folder, /system/framework/framework-res/ (ADR-0039 decision 8), by
    which AssetManager2 finds the overlay's target; the AVD's names /system/framework/framework-res.apk, and that and
    the target's CRC are all that differ from the AVD's idmaps.
"""
import os
import re
import struct
import sys
import zipfile
import zlib

VALUES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "framework", "monet-overlays.txt")
# AssetManager2 finds an overlay's target by its ApkAssets' path, which a folder's gives with a separator at its end
# (DirectoryAssetsProvider::Create).
TARGET_PATH = "/system/framework/framework-res/"
CACHE = "/data/resource-cache"
PACKAGE = "com.android.systemui"
FRRO_MAGIC, FRRO_VERSION = 0x4F525246, 3
IDMAP_MAGIC, IDMAP_VERSION = 0x504D4449, 9
POLICIES, ENFORCE_OVERLAYABLE = 0x13, 1
TYPE_INT_COLOR_ARGB8 = 0x1C
CONFIG_SIZE = 64  # ResTable_config at the tag
# StringPool::FlattenUtf8 of no strings: the header alone, RES_STRING_POOL_TYPE, UTF8_FLAG, strings at its end.
EMPTY_STRING_POOL = struct.pack("<HHI5I", 0x0001, 28, 28, 0, 0, 0x100, 28, 0)


def overlays(path):
    """[(name, file, [(color, ARGB)])] in the file's order."""
    out = []
    for number, line in enumerate(open(path, encoding="utf-8"), 1):
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if fields[0] == "overlay" and len(fields) == 3:
            out.append((fields[1], fields[2], []))
        elif len(fields) == 2 and out and re.fullmatch(r"#[0-9A-F]{8}", fields[1]):
            out[-1][2].append((fields[0], int(fields[1][1:], 16)))
        else:
            raise SystemExit(f"monet-overlays: {path}:{number}: not an overlay or a color: {line.rstrip()}")
    return out


def color_ids(framework_res):
    """Every color's ID in framework-res, from the R classes of its link (android.R and the private ones)."""
    ids = {}
    for rel in ("java/android/R.java", "java/com/android/internal/R.java"):
        text = open(os.path.join(framework_res, rel), encoding="utf-8").read()
        block = re.search(r"\n  public static final class color \{\n(.*?)\n  \}\n", text, re.S)
        for name, value in re.findall(r"public static final int (\w+)=(0x[0-9a-f]{8});", block.group(1)):
            if ids.setdefault(name, int(value, 16)) != int(value, 16):
                raise SystemExit(f"monet-overlays: {name} has two IDs")
    return ids


def target_crc(framework_res):
    with zipfile.ZipFile(os.path.join(framework_res, "framework-res.apk")) as apk:
        return apk.getinfo("resources.arsc").CRC ^ apk.getinfo("AndroidManifest.xml").CRC


def varint(value):
    out = bytearray()
    while value >= 0x80:
        out.append(value & 0x7F | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def field(number, data):
    """A length-delimited field; proto3 leaves out an empty string."""
    return varint(number << 3 | 2) + varint(len(data)) + data if data else b""


def number_field(number, value):
    """A varint field; proto3 leaves out a zero."""
    return varint(number << 3) + varint(value) if value else b""


def frro(name, colors):
    entries = b"".join(field(2, field(1, color.encode()) + field(2, number_field(1, TYPE_INT_COLOR_ARGB8)
                                                                    + number_field(2, argb)))
                       for color, argb in sorted(colors))
    package = field(1, b"android") + field(2, field(1, b"color") + entries)
    proto = field(1, package) + field(2, name.encode()) + field(3, PACKAGE.encode()) + field(4, b"android")
    crc = zlib.crc32(proto, zlib.crc32(struct.pack("<I", FRRO_VERSION)))
    data = struct.pack("<4I", FRRO_MAGIC, FRRO_VERSION, crc, 0)
    return data + struct.pack("<I", len(EMPTY_STRING_POOL)) + EMPTY_STRING_POOL + proto, crc


def string(text):
    data = text.encode()
    return struct.pack("<I", len(data)) + data + b"\0" * (-len(data) % 4)


def idmap(target_path, target, overlay_path, name, overlay, values):
    """`values`: [(target ID, ARGB)]."""
    values = sorted(values)
    data = struct.pack("<6I", IDMAP_MAGIC, IDMAP_VERSION, target, overlay, POLICIES, ENFORCE_OVERLAYABLE)
    data += string(target_path) + string(overlay_path) + string(name) + string("")
    data += struct.pack("<6I", 0, len(values), len(values), 1, 0, 0)
    data += b"".join(struct.pack("<3I", id_, index, 1) for index, (id_, _) in enumerate(values))
    data += b"".join(struct.pack("<IHBBI", 0, 8, 0, TYPE_INT_COLOR_ARGB8, argb) for _, argb in values)
    data += struct.pack("<I", CONFIG_SIZE) + b"\0" * (CONFIG_SIZE - 4)
    return data + struct.pack("<I", len(EMPTY_STRING_POOL)) + EMPTY_STRING_POOL


def idmap_name(overlay_path):
    """idmap2's name for an overlay's idmap in /data/resource-cache (ResourcesManager.overlayPathToIdmapPath)."""
    return overlay_path[1:].replace("/", "@") + "@idmap"


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: tools/monet-overlays.py <framework-res> <out>")
    framework_res, out = sys.argv[1], sys.argv[2]
    ids = color_ids(framework_res)
    crc = target_crc(framework_res)
    os.makedirs(out, exist_ok=True)
    for name, file, colors in overlays(VALUES):
        missing = [color for color, _ in colors if color not in ids]
        if missing:
            raise SystemExit(f"monet-overlays: framework-res has no color {', '.join(missing)}")
        data, overlay_crc = frro(name, colors)
        overlay_path = f"{CACHE}/{file}"
        with open(os.path.join(out, file), "wb") as frro_file:
            frro_file.write(data)
        with open(os.path.join(out, idmap_name(overlay_path)), "wb") as idmap_file:
            idmap_file.write(idmap(TARGET_PATH, crc, overlay_path, name, overlay_crc,
                                   [(ids[color], argb) for color, argb in colors]))
        print(f"monet-overlays: {name}, {len(colors)} colors: {file}")


if __name__ == "__main__":
    main()
