#!/usr/bin/env python3
"""hwui's registration against framework.jar (ADR-0043 test 2): every class that the 42 registrations of
LayoutlibLoader.cpp's list (patch 0006) look up or register natives for must be among framework.jar's classes or the
boot jars', and every native method they register must be declared native in that class or one it extends, with the
same name and descriptor, which is where ART's RegisterNatives looks for it. FindClassOrDie and RegisterMethodsOrDie
abort the process on the first one missing, so this finds that abort without starting ART. The fields and methods the
registrations look up with GetFieldID and GetMethodID are left to the wiring's first run.
Prints "skipped: ..." while hwui's tree or framework.jar's classes are not built.

Usage: hwui_jni_check.py <hwui tree> <framework classes.jar> <boot classes.jar>
  <hwui tree>: frameworks/base/libs/hwui in the tree of tools/build-hwui.sh's build (<build>/tree)
"""
import os
import re
import struct
import sys
import zipfile

ACC_NATIVE = 0x0100
# The bytes after the tag of each kind of constant in a class file's pool; Long and Double take two entries.
CONSTANT_SIZES = {3: 4, 4: 4, 5: 8, 6: 8, 7: 2, 8: 2, 9: 4, 10: 4, 11: 4, 12: 4, 15: 3, 16: 2, 17: 4, 18: 4, 19: 2,
                  20: 2}


def read_class(data):
    """A class file's superclass and the names and descriptors of the methods it declares native."""
    count, = struct.unpack_from(">H", data, 8)
    utf8, classes = {}, {}
    offset, index = 10, 1
    while index < count:
        tag = data[offset]
        if tag == 1:
            length, = struct.unpack_from(">H", data, offset + 1)
            utf8[index] = data[offset + 3:offset + 3 + length].decode("utf-8", "replace")
            offset += 3 + length
        else:
            if tag == 7:
                classes[index], = struct.unpack_from(">H", data, offset + 1)
            offset += 1 + CONSTANT_SIZES[tag]
            index += 1 if tag in (5, 6) else 0
        index += 1
    _, _, superclass, interfaces = struct.unpack_from(">HHHH", data, offset)
    offset += 8 + 2 * interfaces
    natives = set()
    for kind in ("fields", "methods"):
        members, = struct.unpack_from(">H", data, offset)
        offset += 2
        for _ in range(members):
            flags, name, descriptor, attributes = struct.unpack_from(">HHHH", data, offset)
            offset += 8
            for _ in range(attributes):
                length, = struct.unpack_from(">I", data, offset + 2)
                offset += 6 + length
            if kind == "methods" and flags & ACC_NATIVE:
                natives.add((utf8[name], utf8[descriptor]))
    return (utf8[classes[superclass]] if superclass else None), natives


class Classes:
    """The classes of some jars, read when asked for, by their binary names (android/graphics/Bitmap$Config)."""

    def __init__(self, jars):
        self.jars = [zipfile.ZipFile(jar) for jar in jars]
        self.read = {}

    def get(self, name):
        if name not in self.read:
            self.read[name] = None
            for jar in self.jars:
                try:
                    self.read[name] = read_class(jar.read(name + ".class"))
                    break
                except KeyError:
                    continue
        return self.read[name]

    def native(self, name, method, descriptor):
        """Whether the class or one it extends declares the native method."""
        while name:
            found = self.get(name)
            if found is None:
                return False
            superclass, natives = found
            if (method, descriptor) in natives:
                return True
            name = superclass
        return False


def strings(text):
    """The value of one or more adjacent C string literals."""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text))


def lookups(path):
    """What one JNI file registers and looks up: (class, [(method, descriptor)]) per RegisterMethodsOrDie, the
    classes of its FindClassOrDie calls, and what it could not read."""
    text = open(path, errors="replace").read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"(?<![:\"])//[^\n]*", " ", text)
    constants = dict(re.findall(r'char\s*(?:const\s*)?\*?\s*(?:const\s+)?(\w+)\s*(?:\[\s*\])?\s*=\s*"([^"]*)"', text))

    def name(argument):
        argument = argument.strip()
        return strings(argument) if argument.startswith('"') else constants.get(argument)

    tables = {}
    for table, body in re.findall(r"JNINativeMethod\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S):
        tables[table] = [(method, strings(descriptor))
                         for method, descriptor in re.findall(r'\{\s*"(\w+)"\s*,\s*((?:"[^"]*"\s*)+),', body)]
    registered, unread = [], []
    for argument, table in re.findall(r"RegisterMethodsOrDie\(\s*env\s*,\s*([^,]+?)\s*,\s*(\w+)\s*,", text):
        if name(argument) is None or not tables.get(table):
            unread.append(f"{os.path.basename(path)}: RegisterMethodsOrDie({argument.strip()}, {table})")
        else:
            registered.append((name(argument), tables[table]))
    found = []
    for argument in re.findall(r"FindClassOrDie\(\s*env\s*,\s*([^)]+?)\s*\)", text):
        if name(argument) is None:
            unread.append(f"{os.path.basename(path)}: FindClassOrDie({argument.strip()})")
        else:
            found.append(name(argument))
    return registered, found, unread


def registration_files(hwui):
    """The JNI file of each function in LayoutlibLoader.cpp's list, in the list's order."""
    loader = open(os.path.join(hwui, "apex", "LayoutlibLoader.cpp")).read()
    functions = dict(re.findall(r'\{"([\w.]+)",\s*REG_JNI\((\w+)\)\}', loader))
    listed = [functions[name] for name in re.findall(r'^ +"(android\.[\w.]+)",$', loader, re.M)]
    defined = {}
    for directory, _, names in os.walk(os.path.join(hwui, "jni")):
        for file_name in names:
            if file_name.endswith(".cpp"):
                path = os.path.join(directory, file_name)
                for function in re.findall(r"^int\s+(register_\w+)\s*\(\s*JNIEnv", open(path, errors="replace").read(),
                                           re.M):
                    defined[function] = path
    return listed, [defined.get(function) for function in listed]


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    hwui, framework, boot = sys.argv[1:]
    for path, how in ((os.path.join(hwui, "apex", "LayoutlibLoader.cpp"), "tools/build-hwui.sh"),
                      (framework, "tools/build-framework.sh"), (boot, "tools/build-art.sh")):
        if not os.path.exists(path):
            print(f"skipped: {path} is not built ({how})")
            return
    listed, files = registration_files(hwui)
    failures = [f"{function} is defined in no file of jni/" for function, path in zip(listed, files) if path is None]
    if not listed:
        failures.append("LayoutlibLoader.cpp lists no class (patch 0006)")
    classes = Classes([framework, boot])
    checked_classes, checked_natives = set(), 0
    for path in filter(None, files):
        registered, found, unread = lookups(path)
        failures += [f"cannot read {what}" for what in unread]
        for name in found + [name for name, _ in registered]:
            checked_classes.add(name)
            if classes.get(name) is None:
                failures.append(f"{os.path.basename(path)} looks up {name}, which framework.jar does not have")
        for name, methods in registered:
            for method, descriptor in methods:
                checked_natives += 1
                if classes.get(name) is not None and not classes.native(name, method, descriptor):
                    failures.append(f"{os.path.basename(path)} registers {name}.{method}{descriptor}, which is no "
                                    "native method there")
    if failures:
        for failure in sorted(set(failures))[:40]:
            print(f"FAIL: {failure}")
        print(f"{len(set(failures))} failures in {len(listed)} registrations")
        sys.exit(1)
    print(f"framework.jar has the {len(checked_classes)} classes and {checked_natives} natives of hwui's "
          f"{len(listed)} registrations")


if __name__ == "__main__":
    main()
