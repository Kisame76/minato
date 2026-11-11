#!/usr/bin/env python3
"""tools/hiddenapi-encode.py against dex files and jars built here by hand: ART's hiddenapi tool puts a hiddenapi
class data section (map item type 0xF000) into a dex file, with a uleb128 flag per field and method in class data
order, and the encoder does the same from the published hiddenapi-flags.csv. The expected bytes below follow
art/tools/hiddenapi/hiddenapi.cc (HiddenapiClassDataBuilder, DexFileEditor).

Usage: hiddenapi_encode_test.py <repository-root>
"""
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
TOOL = os.path.join(ROOT, "tools", "hiddenapi-encode.py")
NO_INDEX = 0xFFFFFFFF


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def shorty_char(descriptor):
    return "L" if descriptor[0] in "L[" else descriptor[0]


def make_dex(classes):
    """A standard dex file (version 039) of `classes`: dicts with desc, static_fields and instance_fields (name,
    type), direct and virtual (name, return type, parameter types)."""
    strings = {"Ljava/lang/Object;"}
    for cls in classes:
        strings.add(cls["desc"])
        for name, kind in cls.get("static_fields", []) + cls.get("instance_fields", []):
            strings.update((name, kind))
        for name, ret, params in cls.get("direct", []) + cls.get("virtual", []):
            strings.update((name, ret, shorty_char(ret) + "".join(shorty_char(p) for p in params), *params))
    strings = sorted(strings, key=lambda text: text.encode())
    string_index = {text: i for i, text in enumerate(strings)}
    types = sorted({text for text in strings if text[0] in "LIJZBSCFDV[" and text.endswith((";",)) or len(text) == 1
                    and text in "IJZBSCFDV"}, key=lambda text: string_index[text])
    # A name that looks like a descriptor (none here) would be a type too; only real descriptors are listed.
    used = {"Ljava/lang/Object;"}
    for cls in classes:
        used.add(cls["desc"])
        for _, kind in cls.get("static_fields", []) + cls.get("instance_fields", []):
            used.add(kind)
        for _, ret, params in cls.get("direct", []) + cls.get("virtual", []):
            used.update([ret] + list(params))
    types = sorted(used, key=lambda text: string_index[text])
    type_index = {text: i for i, text in enumerate(types)}

    protos = set()
    for cls in classes:
        for _, ret, params in cls.get("direct", []) + cls.get("virtual", []):
            protos.add((ret, tuple(params)))
    protos = sorted(protos, key=lambda p: (type_index[p[0]], tuple(type_index[x] for x in p[1])))
    proto_index = {p: i for i, p in enumerate(protos)}

    fields = sorted({(cls["desc"], name, kind) for cls in classes
                     for name, kind in cls.get("static_fields", []) + cls.get("instance_fields", [])},
                    key=lambda f: (type_index[f[0]], string_index[f[1]], type_index[f[2]]))
    field_index = {f: i for i, f in enumerate(fields)}
    methods = sorted({(cls["desc"], name, (ret, tuple(params))) for cls in classes
                      for name, ret, params in cls.get("direct", []) + cls.get("virtual", [])},
                     key=lambda m: (type_index[m[0]], string_index[m[1]], proto_index[m[2]]))
    method_index = {m: i for i, m in enumerate(methods)}

    string_ids_off = 0x70
    type_ids_off = string_ids_off + 4 * len(strings)
    proto_ids_off = type_ids_off + 4 * len(types)
    field_ids_off = proto_ids_off + 12 * len(protos)
    method_ids_off = field_ids_off + 8 * len(fields)
    class_defs_off = method_ids_off + 8 * len(methods)
    data_off = class_defs_off + 32 * len(classes)

    data = bytearray()
    string_offsets = []
    for text in strings:
        string_offsets.append(data_off + len(data))
        data += uleb(len(text)) + text.encode() + b"\0"
    type_list_offsets = {}
    for params in sorted({p[1] for p in protos if p[1]}):
        data += bytes(-(data_off + len(data)) % 4)
        type_list_offsets[params] = data_off + len(data)
        data += struct.pack("<I", len(params)) + b"".join(struct.pack("<H", type_index[x]) for x in params)
    class_data_offsets = []
    for cls in classes:
        def members(names, index, key):
            ids = sorted(index[key(cls, item)] for item in names)
            out, previous = b"", 0
            for number in ids:
                out += uleb(number - previous) + uleb(1)  # access flags: public
                previous = number
            return out, len(ids)
        sf, sf_n = members(cls.get("static_fields", []), field_index, lambda c, i: (c["desc"], i[0], i[1]))
        inf, inf_n = members(cls.get("instance_fields", []), field_index, lambda c, i: (c["desc"], i[0], i[1]))
        dm, dm_n = members(cls.get("direct", []), method_index, lambda c, i: (c["desc"], i[0], (i[1], tuple(i[2]))))
        vm, vm_n = members(cls.get("virtual", []), method_index, lambda c, i: (c["desc"], i[0], (i[1], tuple(i[2]))))
        # The method entries carry a code offset (0, abstract) after the flags; rebuild them with it.
        def with_code(raw):
            out, pos = b"", 0
            while pos < len(raw):
                start = pos
                for _ in range(2):
                    while raw[pos] & 0x80:
                        pos += 1
                    pos += 1
                out += raw[start:pos] + b"\0"
            return out
        if not (sf_n or inf_n or dm_n or vm_n):
            class_data_offsets.append(0)
            continue
        class_data_offsets.append(data_off + len(data))
        data += uleb(sf_n) + uleb(inf_n) + uleb(dm_n) + uleb(vm_n) + sf + inf + with_code(dm) + with_code(vm)
    data += bytes(-(data_off + len(data)) % 4)
    map_off = data_off + len(data)
    sections = [(0x0000, 1, 0), (0x0001, len(strings), string_ids_off), (0x0002, len(types), type_ids_off),
                (0x0003, len(protos), proto_ids_off), (0x0004, len(fields), field_ids_off),
                (0x0005, len(methods), method_ids_off), (0x0006, len(classes), class_defs_off),
                (0x2002, len(strings), string_offsets[0]), (0x1000, 1, map_off)]
    sections = [s for s in sections if s[1]]
    data += struct.pack("<I", len(sections)) + b"".join(struct.pack("<HHII", t, 0, n, o) for t, n, o in sections)

    body = bytearray()
    body += b"".join(struct.pack("<I", o) for o in string_offsets)
    body += b"".join(struct.pack("<I", string_index[t]) for t in types)
    for ret, params in protos:
        shorty = shorty_char(ret) + "".join(shorty_char(p) for p in params)
        body += struct.pack("<III", string_index[shorty], type_index[ret], type_list_offsets.get(params, 0))
    for desc, name, kind in fields:
        body += struct.pack("<HHI", type_index[desc], type_index[kind], string_index[name])
    for desc, name, proto in methods:
        body += struct.pack("<HHI", type_index[desc], proto_index[proto], string_index[name])
    for cls, class_data in zip(classes, class_data_offsets):
        body += struct.pack("<8I", type_index[cls["desc"]], 1, type_index["Ljava/lang/Object;"], 0, NO_INDEX, 0,
                            class_data, 0)
    out = bytearray(0x70) + body + data
    struct.pack_into("<8s", out, 0, b"dex\n039\0")
    struct.pack_into("<IIII", out, 0x20, len(out), 0x70, 0x12345678, 0)
    struct.pack_into("<IIIIII", out, 0x34, map_off, len(strings), string_ids_off, len(types), type_ids_off,
                     len(protos))
    struct.pack_into("<IIIIIIII", out, 0x4C, proto_ids_off, len(fields), field_ids_off, len(methods),
                     method_ids_off, len(classes), class_defs_off, len(out) - data_off)
    struct.pack_into("<I", out, 0x6C, data_off)
    seal(out)
    return bytes(out)


def seal(out):
    """The signature and the checksum of a dex file."""
    out[12:32] = hashlib.sha1(bytes(out[32:])).digest()
    struct.pack_into("<I", out, 8, zlib.adler32(bytes(out[12:])) & 0xFFFFFFFF)


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def map_items(dex):
    off = u32(dex, 0x34)
    return [struct.unpack_from("<HHII", dex, off + 4 + 12 * i) for i in range(u32(dex, off))]


def verify(test, dex):
    """What ART's dex file verifier checks of the header: sizes, checksum, signature, a map list at the end."""
    test.assertEqual(u32(dex, 0x20), len(dex))
    test.assertEqual(u32(dex, 8), zlib.adler32(dex[12:]) & 0xFFFFFFFF)
    test.assertEqual(dex[12:32], hashlib.sha1(dex[32:]).digest())
    map_off = u32(dex, 0x34)
    test.assertEqual(map_off + 4 + 12 * u32(dex, map_off), len(dex))
    test.assertEqual(u32(dex, 0x68), len(dex) - u32(dex, 0x6C))
    offsets = [item[3] for item in map_items(dex)]
    test.assertEqual(offsets, sorted(offsets))


FOO = {"desc": "LFoo;", "static_fields": [("a", "I")], "instance_fields": [("b", "Ljava/lang/String;")],
       "direct": [("<init>", "V", [])], "virtual": [("run", "J", ["I", "Ljava/lang/String;"])]}
BAR = {"desc": "LBar;", "static_fields": [("x", "I")]}
EMPTY = {"desc": "LEmpty;"}
CSV = """LFoo;->a:I,sdk,public-api
LFoo;->b:Ljava/lang/String;,unsupported
LFoo;-><init>()V,blocked
LFoo;->run(ILjava/lang/String;)J,max-target-p,test-api
LBar;->x:I,sdk
"""


def load():
    spec = importlib.util.spec_from_file_location("hiddenapi_encode", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


import unittest  # noqa: E402


class Flags(unittest.TestCase):
    def test_names(self):
        flags = load().flags_of
        self.assertEqual(flags(["sdk", "public-api"]), 0)
        self.assertEqual(flags(["unsupported"]), 1)
        self.assertEqual(flags(["blocked"]), 2)
        self.assertEqual([flags([f"max-target-{c}"]) for c in "opqrs"], [3, 4, 5, 6, 7])
        self.assertEqual(flags(["sdk", "test-api"]), 0 | 1 << 5)
        self.assertEqual(flags(["core-platform-api", "sdk", "system-api", "test-api"]), 1 << 4 | 1 << 5)
        self.assertEqual(flags(["unsupported", "removed", "lo-prio"]), 1)

    def test_bad_lines(self):
        flags = load().flags_of
        for names in (["sdk", "blocked"], ["no-such-list"], ["test-api"], []):
            with self.assertRaises(ValueError):
                flags(names)

    def test_csv(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "flags.csv")
            with open(path, "w") as handle:
                handle.write(CSV)
            table = load().read_flags(path)
            self.assertEqual(table["LFoo;->run(ILjava/lang/String;)J"], 4 | 1 << 5)
            self.assertEqual(len(table), 5)
            with open(path, "a") as handle:
                handle.write("LBar;->x:I,blocked\n")
            with self.assertRaises(ValueError):
                load().read_flags(path)


class Encode(unittest.TestCase):
    def table(self):
        return {"LFoo;->a:I": 0, "LFoo;->b:Ljava/lang/String;": 1, "LFoo;-><init>()V": 2,
                "LFoo;->run(ILjava/lang/String;)J": 4 | 1 << 5, "LBar;->x:I": 0}

    def test_section_bytes(self):
        tool = load()
        dex = make_dex([FOO, BAR, EMPTY])
        out, stats = tool.encode_dex(dex, self.table(), 2)
        verify(self, out)
        self.assertEqual(out[:0x34], dex[:0x34][:8] + out[8:0x20] + dex[0x20:0x34][:0] + out[0x20:0x34])
        types = [item[0] for item in map_items(out)]
        self.assertEqual(types[-2:], [0xF000, 0x1000])
        self.assertEqual(sorted(types), sorted(types[:-2] + [0xF000, 0x1000]))
        offset = [item for item in map_items(out) if item[0] == 0xF000][0][3]
        self.assertEqual(offset % 4, 0)
        # Three class defs: Foo (flags), Bar (all zero: no data, offset 0), Empty (no class data).
        # ART's builder: size, one offset per class def, then each class's uleb128 flags; Foo's four members are
        # static a (sdk 0), instance b (unsupported 1), direct <init> (blocked 2), virtual run (max-target-p 4 plus
        # test-api 32).
        expected = struct.pack("<IIII", 4 * 4 + 4, 16, 0, 0) + bytes([0, 1, 2, 0x24])
        self.assertEqual(out[offset:offset + len(expected)], expected)
        self.assertEqual(stats, {"members": 5, "missing": 0})
        # The classes' own bytes are untouched up to the old map list, which became padding.
        old_map = u32(dex, 0x34)
        self.assertEqual(out[0x70:old_map], dex[0x70:old_map])
        self.assertEqual(out[old_map:len(dex)], bytes(len(dex) - old_map))

    def test_all_zero_adds_nothing(self):
        tool = load()
        dex = make_dex([BAR])
        out, _ = tool.encode_dex(dex, {"LBar;->x:I": 0}, 2)
        verify(self, out)
        self.assertNotIn(0xF000, [item[0] for item in map_items(out)])

    def test_missing_members(self):
        tool = load()
        dex = make_dex([FOO])
        out, stats = tool.encode_dex(dex, {"LFoo;->a:I": 0}, 2)
        self.assertEqual(stats, {"members": 4, "missing": 3})
        offset = [item for item in map_items(out) if item[0] == 0xF000][0][3]
        self.assertEqual(out[offset + 8:offset + 12], bytes([0, 2, 2, 2]))
        out, stats = tool.encode_dex(dex, {"LFoo;->a:I": 0}, 0)
        self.assertNotIn(0xF000, [item[0] for item in map_items(out)])

    def test_twice_is_refused(self):
        tool = load()
        out, _ = tool.encode_dex(make_dex([FOO]), self.table(), 2)
        with self.assertRaises(ValueError):
            tool.encode_dex(out, self.table(), 2)

    def test_other_dex_version(self):
        dex = bytearray(make_dex([FOO]))
        dex[4:7] = b"041"
        seal(dex)
        with self.assertRaises(ValueError):
            load().encode_dex(bytes(dex), self.table(), 2)


def write_jar(path, entries):
    with zipfile.ZipFile(path, "w") as jar:
        for name, data in entries:
            info = zipfile.ZipInfo(name, (2008, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            if name.endswith(".dex"):
                info.compress_type = zipfile.ZIP_STORED
                start = jar.fp.tell() + 30 + len(name.encode()) + 6
                padding = -start % 4
                info.extra = struct.pack("<HHH", 0xD935, 2 + padding, 4) + bytes(padding)
            else:
                info.compress_type = zipfile.ZIP_DEFLATED
            jar.writestr(info, data)


class Command(unittest.TestCase):
    def run_tool(self, *arguments):
        return subprocess.run([sys.executable, TOOL, *arguments], capture_output=True, text=True)

    def test_jar_and_missing_csv(self):
        with tempfile.TemporaryDirectory() as folder:
            csv = os.path.join(folder, "flags.csv")
            with open(csv, "w") as handle:
                handle.write(CSV)
            jar = os.path.join(folder, "boot.jar")
            first, second = make_dex([FOO, BAR]), make_dex([BAR])
            write_jar(jar, [("classes.dex", first), ("classes2.dex", second), ("res/a.txt", b"text\n")])

            result = self.run_tool("--csv", os.path.join(folder, "none.csv"), jar)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(result.stdout.strip().splitlines()), 1)
            self.assertIn("no hiddenapi data", result.stdout)
            with zipfile.ZipFile(jar) as handle:
                self.assertEqual(handle.read("classes.dex"), first)

            result = self.run_tool("--csv", csv, jar)
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(jar) as handle:
                self.assertEqual([i.filename for i in handle.infolist()], ["classes.dex", "classes2.dex", "res/a.txt"])
                self.assertEqual(handle.read("res/a.txt"), b"text\n")
                one = handle.read("classes.dex")
                verify(self, one)
                self.assertIn(0xF000, [item[0] for item in map_items(one)])
                for info in handle.infolist()[:2]:
                    self.assertEqual(info.compress_type, zipfile.ZIP_STORED)
                    self.assertEqual(info.header_offset + 30 + len(info.filename) + len(info.extra) & 3, 0)
                self.assertEqual(handle.infolist()[0].date_time, (2008, 1, 1, 0, 0, 0))
            # The pin: the right file passes, any other one stops the build.
            pin = os.path.join(folder, "sources.json")
            with open(csv, "rb") as handle:
                content = handle.read()
            with open(pin, "w") as handle:
                json.dump({"hiddenapi-flags": {"bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}},
                          handle)
            fresh = os.path.join(folder, "fresh.jar")
            write_jar(fresh, [("classes.dex", first)])
            self.assertEqual(self.run_tool("--csv", csv, "--pin", pin, fresh).returncode, 0)
            with open(csv, "a") as handle:
                handle.write("LBar;->y:I,sdk\n")
            write_jar(fresh, [("classes.dex", first)])
            wrong = self.run_tool("--csv", csv, "--pin", pin, fresh)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertIn("not the pinned file", wrong.stderr)
            with zipfile.ZipFile(fresh) as handle:
                self.assertEqual(handle.read("classes.dex"), first)
            again = self.run_tool("--csv", csv, jar)
            self.assertNotEqual(again.returncode, 0)
            self.assertIn("already", again.stderr)


if __name__ == "__main__":
    sys.argv = sys.argv[:1]
    unittest.main()
