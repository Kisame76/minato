#!/usr/bin/env python3
"""Puts Android's hidden API flags into the dex files of boot jars, as ART's hiddenapi tool does (art/tools/hiddenapi/
hiddenapi.cc, `hiddenapi encode`): a hiddenapi class data section (dex map item type 0xF000) holds, per class, the
uleb128 flag of each field and method in class data order (static fields, instance fields, direct methods, virtual
methods), and ART's hidden API check (runtime/hidden_api.cc) reads the list of a member from it.

The flags come from the published hiddenapi-flags.csv of the Android release Minato matches (third_party/sources.json,
"hiddenapi-flags"): one line `Lclass;->name:type,flag...` or `Lclass;->name(args)ret,flag...` per member. A flag name
is a list (sdk, unsupported, blocked, max-target-o to -s), which gives the low four bits, or a domain
(core-platform-api, test-api), which gives bit 4 and 5; public-api, system-api, removed and lo-prio say where a member
came from and are ignored, as ART's does. A member the file does not name gets --missing's flag, `blocked` by default:
it is not part of Android's API, so an app finds no such member (platform code, the boot class path's own, is not
checked at all). With no CSV the jars stay as they are and one line says so, so that a clone without the download
still builds.

Each dex file's checksum and SHA-1 are made again, its old map list becomes padding, and the jar keeps its entries,
times and the 4-byte alignment of the stored dex files.

A file that is not the pinned one would put another Android release's lists into the jars, which is the main risk
of this step: --pin <third_party/sources.json> checks the CSV's size and SHA-256 against the entry "hiddenapi-flags"
first.

Usage: hiddenapi-encode.py --csv <hiddenapi-flags.csv> [--pin <sources.json>] [--missing blocked|sdk] <jar>...
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import zipfile
import zlib

LISTS = {"sdk": 0, "unsupported": 1, "blocked": 2, "max-target-o": 3, "max-target-p": 4, "max-target-q": 5,
         "max-target-r": 6, "max-target-s": 7}
DOMAINS = {"core-platform-api": 1 << 4, "test-api": 1 << 5}
IGNORED = {"public-api", "system-api", "removed", "lo-prio"}
HIDDENAPI_CLASS_DATA = 0xF000
MAP_LIST = 0x1000
VERSIONS = (b"035", b"037", b"038", b"039")
TIMESTAMP = (2008, 1, 1, 0, 0, 0)


def flags_of(names):
    """The dex flags of one CSV line's flag names; ValueError for a name nobody knows, two lists, or no list."""
    value = None
    domains = 0
    for name in names:
        if name in LISTS:
            if value is not None and value != LISTS[name]:
                raise ValueError(f"two lists: {','.join(names)}")
            value = LISTS[name]
        elif name in DOMAINS:
            domains |= DOMAINS[name]
        elif name not in IGNORED:
            raise ValueError(f"unknown flag {name}")
    if value is None:
        raise ValueError(f"no list in {','.join(names)}")
    return value | domains


def read_flags(path):
    """{member: dex flags} of a hiddenapi-flags.csv."""
    table = {}
    with open(path, encoding="utf-8", errors="surrogateescape") as handle:
        for number, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            member, _, rest = line.partition(",")
            try:
                value = flags_of(rest.split(","))
                if member in table:
                    raise ValueError(f"{member} twice")
            except ValueError as error:
                raise ValueError(f"{path}:{number}: {error}") from None
            table[member] = value
    return table


def check_pin(csv, manifest):
    """SystemExit unless `csv` has the bytes and the SHA-256 that `manifest`'s entry "hiddenapi-flags" pins."""
    with open(manifest) as handle:
        pin = json.load(handle)["hiddenapi-flags"]
    digest = hashlib.sha256()
    size = 0
    with open(csv, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
            size += len(block)
    if size != pin["bytes"] or digest.hexdigest() != pin["sha256"]:
        raise SystemExit(f"hiddenapi-encode: {csv} is not the pinned file ({size} bytes, SHA-256 {digest.hexdigest()}; "
                         f"third_party/sources.json pins {pin['bytes']} bytes, {pin['sha256']})")


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def read_uleb(data, pos):
    result = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7


class Dex:
    """The tables of a standard dex file that name a member: strings, types, protos, fields, methods, classes."""

    def __init__(self, data):
        self.data = data
        (self.string_count, self.string_off, self.type_count, self.type_off, self.proto_count, self.proto_off,
         self.field_count, self.field_off, self.method_count, self.method_off, self.class_count,
         self.class_off) = struct.unpack_from("<12I", data, 0x38)
        self.strings = {}
        self.types = {}

    def string(self, index):
        text = self.strings.get(index)
        if text is None:
            pos = struct.unpack_from("<I", self.data, self.string_off + 4 * index)[0]
            _, pos = read_uleb(self.data, pos)
            end = self.data.index(0, pos)
            text = self.strings[index] = self.data[pos:end].decode("utf-8", "surrogateescape")
        return text

    def type(self, index):
        text = self.types.get(index)
        if text is None:
            text = self.types[index] = self.string(struct.unpack_from("<I", self.data, self.type_off + 4 * index)[0])
        return text

    def field(self, index):
        """(name, ':' + type) of a field id."""
        _, kind, name = struct.unpack_from("<HHI", self.data, self.field_off + 8 * index)
        return self.string(name), ":" + self.type(kind)

    def method(self, index):
        """(name, '(' params ')' return) of a method id."""
        _, proto, name = struct.unpack_from("<HHI", self.data, self.method_off + 8 * index)
        _, ret, params = struct.unpack_from("<III", self.data, self.proto_off + 12 * proto)
        listed = ""
        if params:
            count = struct.unpack_from("<I", self.data, params)[0]
            listed = "".join(self.type(t) for t in struct.unpack_from(f"<{count}H", self.data, params + 4))
        return self.string(name), f"({listed}){self.type(ret)}"

    def members(self, class_index):
        """The `Lclass;->member` keys of a class's fields and methods in class data order, or None without class
        data."""
        descriptor_type, _, _, _, _, _, data_off, _ = struct.unpack_from("<8I", self.data,
                                                                         self.class_off + 32 * class_index)
        if data_off == 0:
            return None
        descriptor = self.type(descriptor_type)
        sizes = []
        pos = data_off
        for _ in range(4):
            size, pos = read_uleb(self.data, pos)
            sizes.append(size)
        keys = []
        for group, size in enumerate(sizes):
            index = 0
            for _ in range(size):
                delta, pos = read_uleb(self.data, pos)
                _, pos = read_uleb(self.data, pos)  # access flags
                index += delta
                if group < 2:
                    name, signature = self.field(index)
                else:
                    _, pos = read_uleb(self.data, pos)  # code offset
                    name, signature = self.method(index)
                keys.append(f"{descriptor}->{name}{signature}")
        return keys


def encode_dex(data, table, missing):
    """(the dex file with its hiddenapi class data section, {"members": n, "missing": n}); a member `table` does not
    name gets the flag `missing`. A dex file whose flags are all zero stays as it is, as with ART's tool."""
    if data[:4] != b"dex\n" or data[4:7] not in VERSIONS:
        raise ValueError(f"not a standard dex file (version {bytes(data[4:7])!r})")
    map_off = struct.unpack_from("<I", data, 0x34)[0]
    count = struct.unpack_from("<I", data, map_off)[0]
    items = [struct.unpack_from("<HHII", data, map_off + 4 + 12 * i) for i in range(count)]
    if any(item[0] == HIDDENAPI_CLASS_DATA for item in items):
        raise ValueError("the dex file already has hiddenapi class data")
    if map_off + 4 + 12 * count != len(data):
        raise ValueError("the map list is not at the end of the dex file")
    dex = Dex(data)
    stats = {"members": 0, "missing": 0}
    section = bytearray(4 * (dex.class_count + 1))
    nonzero = False
    for index in range(dex.class_count):
        keys = dex.members(index)
        start = len(section)
        if keys is None:
            continue
        flags = []
        for key in keys:
            stats["members"] += 1
            found = table.get(key)
            if found is None:
                stats["missing"] += 1
                found = missing
            flags.append(found)
        if any(flags):
            nonzero = True
            struct.pack_into("<I", section, 4 * (index + 1), start)
            for value in flags:
                section += uleb(value)
    if not nonzero:
        return bytes(data), stats
    struct.pack_into("<I", section, 0, len(section))
    out = bytearray(data)
    out[map_off:] = bytes(len(data) - map_off)  # the old map list becomes padding
    out += bytes(-len(out) % 4)
    section_off = len(out)
    out += section
    out += bytes(-len(out) % 4)
    new_map = len(out)
    kept = [item for item in items if item[0] != MAP_LIST]
    kept.append((HIDDENAPI_CLASS_DATA, 0, 1, section_off))
    kept.append((MAP_LIST, 0, 1, new_map))
    out += struct.pack("<I", len(kept)) + b"".join(struct.pack("<HHII", *item) for item in kept)
    data_off = struct.unpack_from("<I", out, 0x6C)[0]
    struct.pack_into("<I", out, 0x20, len(out))
    struct.pack_into("<I", out, 0x34, new_map)
    struct.pack_into("<I", out, 0x68, len(out) - data_off)
    out[12:32] = hashlib.sha1(bytes(out[32:])).digest()
    struct.pack_into("<I", out, 8, zlib.adler32(bytes(out[12:])) & 0xFFFFFFFF)
    return bytes(out), stats


def encode_jar(path, table, missing):
    """The jar at `path` with every dex file encoded; the totals of encode_dex."""
    total = {"dex": 0, "members": 0, "missing": 0}
    temporary = path + ".hiddenapi"
    with zipfile.ZipFile(path) as source, zipfile.ZipFile(temporary, "w") as target:
        for info in source.infolist():
            data = source.read(info)
            fresh = zipfile.ZipInfo(info.filename, info.date_time)
            fresh.external_attr = info.external_attr
            fresh.compress_type = info.compress_type
            fresh.extra = info.extra
            if info.filename.endswith(".dex"):
                try:
                    data, stats = encode_dex(data, table, missing)
                except ValueError as error:
                    raise SystemExit(f"hiddenapi-encode: {path}: {info.filename}: {error}")
                total["dex"] += 1
                total["members"] += stats["members"]
                total["missing"] += stats["missing"]
                if info.compress_type == zipfile.ZIP_STORED:
                    # zipalign's extra field (0xd935) pads the data to a 4-byte boundary.
                    start = target.fp.tell() + 30 + len(info.filename.encode()) + 6
                    padding = -start % 4
                    fresh.extra = struct.pack("<HHH", 0xD935, 2 + padding, 4) + bytes(padding)
            target.writestr(fresh, data)
    os.replace(temporary, path)
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--csv", required=True)
    parser.add_argument("--pin", help="sources.json, whose entry hiddenapi-flags the CSV must match")
    parser.add_argument("--missing", choices=("blocked", "sdk"), default="blocked")
    parser.add_argument("jars", nargs="+")
    arguments = parser.parse_args()
    if not os.path.exists(arguments.csv):
        print(f"hiddenapi-encode: {arguments.csv} is missing: the boot jars carry no hiddenapi data "
              f"(third_party/sources.json, hiddenapi-flags)")
        return 0
    if arguments.pin:
        check_pin(arguments.csv, arguments.pin)
    table = read_flags(arguments.csv)
    missing = LISTS[arguments.missing]
    for path in arguments.jars:
        total = encode_jar(path, table, missing)
        print(f"hiddenapi-encode: {path}: {total['dex']} dex files, {total['members']} members, "
              f"{total['missing']} not in the list ({arguments.missing})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
