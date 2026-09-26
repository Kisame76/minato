"""The Khronos XML registries as tools/frontgen reads them: vk.xml (dialect "vulkan"), gl.xml with ANGLE's
gl_angle_ext.xml ("gl", API gles2) and egl.xml with ANGLE's egl_angle_ext.xml ("egl"), merged the way
ANGLE's own scripts/registry_xml.py merges them. Only what is valid for the one API is kept: Vulkan SC's
definitions, disabled and provisional extensions are left out."""
import re
import xml.etree.ElementTree as ElementTree

from abi import CType

API = {"vulkan": "vulkan", "gl": "gles2", "egl": "egl"}

# C's own types and Khronos' on arm64 (LP64): (kind, size, signed).
BUILTIN = {
    "void": ("void", 0, False),
    "char": ("char", 1, None),
    "signed char": ("int", 1, True), "unsigned char": ("int", 1, False),
    "short": ("int", 2, True), "unsigned short": ("int", 2, False),
    "int": ("int", 4, True), "unsigned int": ("int", 4, False), "unsigned": ("int", 4, False),
    "long": ("int", 8, True), "unsigned long": ("int", 8, False),
    "long long": ("int", 8, True), "unsigned long long": ("int", 8, False),
    "float": ("float", 4, True), "double": ("float", 8, True), "long double": ("long double", 16, True),
    "int8_t": ("int", 1, True), "uint8_t": ("int", 1, False), "int16_t": ("int", 2, True),
    "uint16_t": ("int", 2, False), "int32_t": ("int", 4, True), "uint32_t": ("int", 4, False),
    "int64_t": ("int", 8, True), "uint64_t": ("int", 8, False), "size_t": ("int", 8, False),
    "ssize_t": ("int", 8, True), "intptr_t": ("int", 8, True), "uintptr_t": ("int", 8, False),
    "ptrdiff_t": ("int", 8, True), "bool": ("int", 1, False), "_Bool": ("int", 1, False),
    "va_list": ("va_list", 8, None),
    "khronos_int8_t": ("int", 1, True), "khronos_uint8_t": ("int", 1, False),
    "khronos_int16_t": ("int", 2, True), "khronos_uint16_t": ("int", 2, False),
    "khronos_int32_t": ("int", 4, True), "khronos_uint32_t": ("int", 4, False),
    "khronos_int64_t": ("int", 8, True), "khronos_uint64_t": ("int", 8, False),
    "khronos_intptr_t": ("int", 8, True), "khronos_uintptr_t": ("int", 8, False),
    "khronos_ssize_t": ("int", 8, True), "khronos_usize_t": ("int", 8, False),
    "khronos_float_t": ("float", 4, True), "khronos_utime_nanoseconds_t": ("int", 8, False),
    "khronos_stime_nanoseconds_t": ("int", 8, True), "khronos_boolean_enum_t": ("int", 4, True),
}
# What EGL/eglplatform.h defines in its Android branch; egl.xml only names these types.
EGL_PLATFORM = {
    "EGLNativeDisplayType": ("pointer", 8, False), "EGLNativePixmapType": ("pointer", 8, False),
    "EGLNativeWindowType": ("pointer", 8, False), "NativeDisplayType": ("pointer", 8, False),
    "NativePixmapType": ("pointer", 8, False), "NativeWindowType": ("pointer", 8, False),
    "EGLint": ("int", 4, True),
}
QUALIFIERS = ("const", "volatile", "struct", "union", "enum")


def version(number):
    return tuple(int(part) for part in number.split("."))


class Command:
    def __init__(self, name, result, params, alias=None):
        self.name, self.result, self.params, self.alias = name, result, params, alias


class Extension:
    def __init__(self, name, element, dialect):
        self.name = name
        separator = "," if dialect == "vulkan" else "|"
        self.supported = set((element.get("supported") or "").split(separator))
        self.platform = element.get("platform")
        self.provisional = element.get("provisional") == "true" or self.platform == "provisional"
        self.kind = element.get("type")
        self.commands = []   # (command, depends), in the XML's order
        self.types = []
        self.enum_extends = set()   # the enumerations it adds values to


def children(root, tag):
    section = root.find(tag)
    return list(section) if section is not None else []


class Registry:
    def __init__(self, path, dialect, extension_path=None):
        self.dialect, self.api = dialect, API[dialect]
        root = ElementTree.parse(path).getroot()
        if extension_path:
            extra = ElementTree.parse(extension_path).getroot()
            for section in ("types", "commands", "extensions"):
                target = root.find(section)
                for part in extra.iter(section):
                    target.extend(list(part))
        self.root = root
        self.types = {}
        for element in children(root, "types"):
            if element.tag != "type":
                continue
            name = element.get("name") or element.findtext("name") or element.findtext("proto/name")
            if name and self._ours(element) and (name not in self.types or element.get("api")):
                self.types[name] = element
        self.callback_names = {name for name, element in self.types.items() if self._is_callback(element)}
        self.callbacks = {name: self._callback(name, self.types[name]) for name in sorted(self.callback_names)}
        self.commands, self.aliases = {}, {}
        for element in children(root, "commands"):
            if element.tag != "command" or not self._ours(element):
                continue
            if element.get("alias"):
                self.aliases[element.get("name")] = element.get("alias")
                continue
            name = element.findtext("proto/name")
            alias = element.find("alias")
            if alias is not None:
                self.aliases[name] = alias.get("name")
            self.commands[name] = self._command(name, element)
        self.versions = {}
        for feature in root.iter("feature"):
            if self.api not in feature.get("api", "").split(","):
                continue
            for part in feature.findall("require"):
                if not self._ours(part):
                    continue
                for command in part.findall("command"):
                    name = command.get("name")
                    number = version(feature.get("number"))
                    self.versions[name] = min(self.versions.get(name, number), number)
        self.extensions = {}
        for element in children(root, "extensions"):
            if element.tag != "extension":
                continue
            extension = Extension(element.get("name"), element, dialect)
            if self.api not in extension.supported or extension.provisional:
                continue
            for part in element.findall("require"):
                if not self._ours(part):
                    continue
                for command in part.findall("command"):
                    extension.commands.append((command.get("name"), part.get("depends")))
                extension.types += [kind.get("name") for kind in part.findall("type")]
                extension.enum_extends |= {enum.get("extends") for enum in part.findall("enum") if enum.get("extends")}
            self.extensions[extension.name] = extension

    def _ours(self, element):
        api = element.get("api")
        return api is None or self.api in api.split(",")

    def _is_callback(self, element):
        if self.dialect == "vulkan":
            return element.get("category") == "funcpointer"
        return "(" in "".join(element.itertext()) and element.findtext("name") is not None

    def target(self, name):
        """The command an alias stands for (itself for a command)."""
        seen = set()
        while name in self.aliases and name not in seen:
            seen.add(name)
            name = self.aliases[name]
        return name

    def command(self, name):
        """The definition of a name: its own (GL writes aliases out), or the command it stands for."""
        return self.commands.get(name) or self.commands.get(self.target(name))

    # Types.

    def _parse(self, element):
        """A <param> or <proto>: its name and CType."""
        text = "".join(element.itertext())
        if text.strip() == "...":
            return "...", CType("variadic", text="...")
        name_element = element.find("name")
        name = name_element.text if name_element is not None else ""
        tail = (name_element.tail or "") if name_element is not None else ""
        before = text[:len(text) - len(tail) - len(name)] if name else text
        base = element.findtext("type") or element.findtext("ptype")
        pointer = "*" in before or "[" in tail
        if base is None:
            base = " ".join(word for word in re.findall(r"\w+", before) if word not in QUALIFIERS)
        value = self.ctype(base, pointer, " ".join((before + tail).split()))
        value.base = base
        return name, value

    def ctype(self, base, pointer, text):
        if pointer:
            return CType("pointer", 8, False, text)
        return self._resolve(base, text, set())

    def _resolve(self, base, text, seen):
        if base in seen:
            return CType("unknown", text=text)
        seen = seen | {base}
        if base in self.callback_names:
            return CType("callback", 8, False, text, callback=base)
        if self.dialect == "egl" and base in EGL_PLATFORM:
            return CType(*EGL_PLATFORM[base], text=text)
        element = self.types.get(base)
        if element is None or (element.get("requires") and not "".join(element.itertext()).strip()):
            if base in BUILTIN:
                return CType(*BUILTIN[base], text=text)
            return CType("unknown", text=text)
        if self.dialect == "vulkan":
            return self._vulkan_type(base, element, text, seen)
        return self._typedef(element, text, seen)

    def _vulkan_type(self, base, element, text, seen):
        if element.get("alias"):
            return self._resolve(element.get("alias"), text, seen)
        category = element.get("category")
        if category == "handle":
            return CType("pointer", 8, False, text)
        if category == "enum":
            return CType("int", 4, True, text)
        if category in ("struct", "union"):
            return CType("struct", text=text)
        body = "".join(element.itertext())
        if category in ("bitmask", "basetype"):
            if "*" in body:
                return CType("pointer", 8, False, text)
            inner = element.findtext("type")
            if inner:
                return self._resolve(inner, text, seen)
            if body.strip().startswith("struct"):
                return CType("struct", text=text)
        if base in BUILTIN:
            return CType(*BUILTIN[base], text=text)
        return CType("unknown", text=text)

    def _typedef(self, element, text, seen):
        """GL and EGL: a type is its C typedef."""
        body = " ".join("".join(element.itertext()).split())
        name = element.findtext("name") or element.get("name")
        if body.startswith("struct"):
            return CType("struct", text=text)
        match = re.match(r"typedef (.*?)\b" + re.escape(name) + r"\s*;$", body)
        if not match:
            return CType("unknown", text=text)
        old = match.group(1)
        if "*" in old:
            return CType("pointer", 8, False, text)
        words = " ".join(word for word in re.findall(r"\w+", old) if word not in QUALIFIERS)
        resolved = self._resolve(words, text, seen)
        return CType(resolved.kind, resolved.size, resolved.signed, text, resolved.callback)

    # Commands and callbacks.

    def _command(self, name, element):
        _, result = self._parse(element.find("proto"))
        params = [self._parse(param)[1] for param in element.findall("param") if self._ours(param)]
        return Command(name, result, params)

    def _callback(self, name, element):
        """A callback's result and parameters: Vulkan writes them as a <proto> with <param>s, GL and EGL as
        a C typedef."""
        if element.find("proto") is not None:
            _, result = self._parse(element.find("proto"))
            return result, [self._parse(param)[1] for param in element.findall("param")]
        body = " ".join("".join(element.itertext()).split())
        match = re.match(r"typedef (.*?)\(\s*\*?\s*" + re.escape(name) + r"\s*\)\s*\((.*)\)\s*;$", body)
        if not match:
            return CType("unknown", text=body), []
        result = self._text_type(match.group(1))
        params = match.group(2).strip()
        if params in ("", "void"):
            return result, []
        return result, [self._text_type(param, named=True) for param in params.split(",")]

    def _text_type(self, text, named=False):
        """A C type written out, as in a typedef of GL or EGL; with named, the declaration ends in a name."""
        text = " ".join(text.split())
        if named and re.search(r"(\w\s+|\*\s*)\w+$", text):
            text = re.sub(r"\w+$", "", text).strip()
        if "*" in text:
            return CType("pointer", 8, False, text)
        words = " ".join(word for word in re.findall(r"\w+", text) if word not in QUALIFIERS)
        return self._resolve(words, text, set())

    # Structures (Vulkan).

    def members(self, name):
        """The type names of a structure's members."""
        element = self.types.get(name)
        if element is None or element.get("category") not in ("struct", "union"):
            return []
        return [member.findtext("type") for member in element.findall("member") if self._ours(member)]

    def extends(self, name):
        element = self.types.get(name)
        return set((element.get("structextends") or "").split(",")) - {""} if element is not None else set()

    def required_by(self):
        """Per type, the APIs of the features and extensions that require it, Vulkan SC's included."""
        result = {}
        for parent in list(self.root.iter("feature")) + list(self.root.iter("extension")):
            apis = set((parent.get("api") or parent.get("supported") or "").split(","))
            if parent.get("provisional") == "true" or parent.get("platform") == "provisional":
                continue
            for part in parent.iter("require"):
                for kind in part.findall("type"):
                    result.setdefault(kind.get("name"), set()).update(apis)
        return result
