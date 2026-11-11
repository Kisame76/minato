#!/usr/bin/env python3
"""Packs the time zones of the Mac into bionic's tzdata file (ADR-0021, M2 step 5c).

Bionic reads time zones from one file, /system/usr/share/zoneinfo/tzdata or the tzdata APEX
(libc/tzcode/bionic.cpp): a header, an index of zone names and the TZif data of every zone. The Mac
has the same IANA tz database as TZif files in /usr/share/zoneinfo; the data is in the public domain.

  tools/make-tzdata.py <output> [<zoneinfo directory>]
  tools/make-tzdata.py <output> --pin-from <third_party/sources.json>

With --pin-from, the release packed is the one icu-data's time zone files are pinned for in the list (so that
bionic, libcore and ICU answer from one IANA release, ADR-0033 decision 9), whatever release the Mac holds today. The
folder is, in this order: the Mac's folder of that release, /var/db/timezone/tz/<release>.*/zoneinfo (the highest
suffix; macOS keeps the previous release beside the active one after an update), else /usr/share/zoneinfo when its
+VERSION is the release; otherwise the tool stops and names the two ways out. A folder counts only with Etc/UTC and
the +VERSION of the release. MINATO_TZ_ROOT puts a fake root in front of both paths (the test).

Layout, big-endian as bionic reads it:
  char version[12]      "tzdata" + IANA version, NUL-terminated, e.g. "tzdata2026c"
  int32 index_offset    24, the index follows the header
  int32 data_offset     the TZif data follows the index
  int32 final_offset    the end of the data
  index entries         char name[40] (NUL-padded), int32 start (from data_offset), int32 length, int32 0
"""
import glob
import json
import os
import re
import struct
import sys

NAME_LENGTH = 40
# Files in the zoneinfo directory that are not zones, and zones Android's tzdata does not have.
SKIPPED = {"posixrules", "localtime", "Factory"}


def zones(root):
    """(name, TZif bytes) for every zone below root, sorted by name."""
    found = []
    for directory, _, files in os.walk(root, followlinks=True):
        for file in files:
            path = os.path.join(directory, file)
            name = os.path.relpath(path, root)
            if name in SKIPPED or len(name) > NAME_LENGTH:
                continue
            with open(path, "rb") as stream:
                data = stream.read()
            if data.startswith(b"TZif"):
                found.append((name, data))
    return sorted(found)


def pack(version, entries):
    header_size = 24
    index_size = len(entries) * (NAME_LENGTH + 12)
    data_offset = header_size + index_size
    index = bytearray()
    data = bytearray()
    for name, tzif in entries:
        index += name.encode().ljust(NAME_LENGTH, b"\0")
        index += struct.pack(">iii", len(data), len(tzif), 0)
        data += tzif
    tag = ("tzdata" + version).encode()
    if len(tag) != 11:
        raise SystemExit(f"unexpected tz version {version!r}")
    header = tag + b"\0" + struct.pack(">iii", header_size, data_offset, data_offset + len(data))
    return header + bytes(index) + bytes(data)


def pinned_release(listing):
    """The one IANA release icu-data's files are pinned for (icu_tzdata's paths .../icunew/<release>/44/...)."""
    with open(listing) as stream:
        data = json.load(stream)
    releases = set()
    found = False
    for entries in data.values():
        if not isinstance(entries, list):
            continue
        for entry in entries:
            if isinstance(entry, dict) and entry.get("name") == "icu_tzdata":
                found = True
                for path in entry.get("files", []):
                    match = re.search(r"/icunew/(\d{4}[a-z])/", path)
                    if match:
                        releases.add(match.group(1))
    if not found or len(releases) != 1:
        raise SystemExit(f"{listing}: icu_tzdata must name the files of exactly one release, found "
                         f"{sorted(releases) or 'none'}")
    return releases.pop()


def release_of(folder):
    """The +VERSION of a zoneinfo folder, or None."""
    try:
        with open(os.path.join(folder, "+VERSION")) as stream:
            return stream.read().strip()
    except OSError:
        return None


def suffix_key(name, release):
    """Sorts 2026e.1.10 above 2026e.1.9: the numbers after the release, as numbers."""
    return tuple(int(part) if part.isdigit() else -1 for part in name[len(release) + 1:].split("."))


def pinned_source(release, prefix=""):
    """(folder, a line saying what is packed and why) for the pinned release, or SystemExit naming the ways out."""
    mac = prefix + "/var/db/timezone/tz"
    active_folder = prefix + "/usr/share/zoneinfo"
    active = release_of(active_folder) or "unknown"
    tried = []
    names = sorted((os.path.basename(os.path.dirname(path)) for path in glob.glob(f"{mac}/{release}.*/zoneinfo")),
                   key=lambda name: suffix_key(name, release), reverse=True)
    candidates = [(os.path.join(mac, name, "zoneinfo"), name) for name in names]
    candidates.append((active_folder, "/usr/share/zoneinfo"))
    for folder, label in candidates:
        found = release_of(folder)
        if found != release:
            tried.append(f"{label} (+VERSION {found})")
        elif not os.path.isfile(os.path.join(folder, "Etc", "UTC")):
            tried.append(f"{label} (no Etc/UTC)")
        else:
            return folder, (f"make-tzdata: the Mac's active release is {active}, "
                            f"packing the pinned {release} from {folder}")
    raise SystemExit(
        f"make-tzdata: the pinned tz release {release} is not on this Mac (the active release is {active}; "
        f"looked at {', '.join(tried) or 'nothing'}). Bionic's tzdata must be of the release icu-data's files are "
        f"pinned for: pin icu-data's files of {active} in third_party/sources.json (tools/fetch-sources.py fetches "
        f"them, a download that needs the user's OK) or restore {mac}/{release}.*/zoneinfo.")


def main():
    args = sys.argv[1:]
    pin_from = None
    if len(args) == 3 and args[1] == "--pin-from":
        pin_from = args[2]
        args = args[:1]
    if len(args) not in (1, 2):
        raise SystemExit(__doc__)
    output = args[0]
    if pin_from:
        release = pinned_release(pin_from)
        folder, line = pinned_source(release, os.environ.get("MINATO_TZ_ROOT", ""))
        print(line)
        root = os.path.realpath(folder)
    else:
        root = os.path.realpath(args[1] if len(args) == 2 else "/usr/share/zoneinfo")
    version = release_of(root)
    if version is None:
        raise SystemExit(f"{root} has no +VERSION")
    entries = zones(root)
    if not any(name == "Etc/UTC" for name, _ in entries):
        raise SystemExit(f"{root} has no Etc/UTC")
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "wb") as stream:
        stream.write(pack(version, entries))
    print(f"{output}: tzdata{version}, {len(entries)} zones")


if __name__ == "__main__":
    main()
