"""The NDK's C headers as the tests of ADR-0030 read them: the prototypes of Android's GLES, EGL and
Vulkan functions, the C types of their arguments and results, and where each argument lies in
Android's ABI (AAPCS64) and in Apple's.

Written independently of tools/frontgen/, which reads the same functions from the Khronos XML: test 1
compares the two readings name by name, and test 2's caller is built from this one, so an error in
either reading shows as a difference. Standard library only."""
import glob
import os
import re
import subprocess

# The headers of each front lib, below the sysroot's usr/include, in the order they are read.
HEADERS = {
    "gles": ["KHR/khrplatform.h", "GLES/gl.h", "GLES/glext.h", "GLES2/gl2.h", "GLES2/gl2ext.h", "GLES3/gl3.h",
             "GLES3/gl31.h", "GLES3/gl32.h"],
    "egl": ["KHR/khrplatform.h", "EGL/eglplatform.h", "EGL/egl.h", "EGL/eglext.h"],
    "vulkan": ["vulkan/vulkan_core.h", "vulkan/vulkan_android.h"],
}
# For the names the NDK lacks (ANGLE's extensions, Vulkan's newer commands): the host library's own C
# headers, below ANGLE's include/ or MoltenVK's include/.
HOST_HEADERS = {
    "gles": ["KHR/khrplatform.h", "GLES/gl.h", "GLES/glext.h", "GLES2/gl2.h", "GLES2/gl2ext.h",
             "GLES2/gl2ext_angle.h", "GLES3/gl3.h", "GLES3/gl31.h", "GLES3/gl32.h"],
    "egl": ["KHR/khrplatform.h", "EGL/eglplatform.h", "EGL/egl.h", "EGL/eglext.h", "EGL/eglext_angle.h"],
    "vulkan": ["vulkan/vulkan_core.h", "vulkan/vulkan_android.h"],
}
PROTOTYPE = {
    "gles": re.compile(r"GL_API(?:CALL)?\s+([^;]*?)\s*GL_APIENTRY\s+(\w+)\s*\(([^;]*?)\)\s*;"),
    "egl": re.compile(r"EGLAPI\s+([^;]*?)\s*EGLAPIENTRY\s+(\w+)\s*\(([^;]*?)\)\s*;"),
    "vulkan": re.compile(r"VKAPI_ATTR\s+([^;]*?)\s+VKAPI_CALL\s+(\w+)\s*\(([^;]*?)\)\s*;"),
}
# The libraries of each front lib in the NDK's stubs.
LIBRARIES = {"gles": ["libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so"], "egl": ["libEGL.so"],
             "vulkan": ["libvulkan.so"]}

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SIMPLE_TYPEDEF = re.compile(r"typedef\s+([^;{}()]*?)\s*\b(\w+)\s*;")
FUNCTION_TYPEDEF = re.compile(r"typedef\s+[^;{}]*?\(\s*(?:\w+\s+)?\*?\s*(\w+)\s*\)\s*\(")
TAGGED_TYPEDEF = re.compile(r"typedef\s+(struct|union|enum)\s+(\w+)\s*\{")
HANDLE = re.compile(r"VK_DEFINE(?:_NON_DISPATCHABLE)?_HANDLE\((\w+)\)")
ANDROID_BRANCH = re.compile(r"#elif defined\(__ANDROID__\) \|\| defined\(ANDROID\)(.*?)#(?:elif|else|endif)", re.S)

# C's own types on arm64 (LP64): (kind, size, signed). Plain char has no fixed signedness across the
# two ABIs (unsigned in AAPCS64, signed on Apple platforms), so it is a kind of its own.
BUILTIN = {
    "void": ("void", 0, False),
    "char": ("char", 1, None),
    "signed char": ("int", 1, True), "unsigned char": ("int", 1, False),
    "short": ("int", 2, True), "short int": ("int", 2, True), "signed short": ("int", 2, True),
    "signed short int": ("int", 2, True), "unsigned short": ("int", 2, False), "unsigned short int": ("int", 2, False),
    "int": ("int", 4, True), "signed": ("int", 4, True), "signed int": ("int", 4, True),
    "unsigned": ("int", 4, False), "unsigned int": ("int", 4, False),
    "long": ("int", 8, True), "long int": ("int", 8, True), "signed long": ("int", 8, True),
    "signed long int": ("int", 8, True), "unsigned long": ("int", 8, False), "unsigned long int": ("int", 8, False),
    "long long": ("int", 8, True), "long long int": ("int", 8, True), "signed long long": ("int", 8, True),
    "signed long long int": ("int", 8, True), "unsigned long long": ("int", 8, False),
    "unsigned long long int": ("int", 8, False),
    "float": ("float", 4, True), "double": ("float", 8, True), "long double": ("long double", 16, True),
    "int8_t": ("int", 1, True), "uint8_t": ("int", 1, False), "int16_t": ("int", 2, True),
    "uint16_t": ("int", 2, False), "int32_t": ("int", 4, True), "uint32_t": ("int", 4, False),
    "int64_t": ("int", 8, True), "uint64_t": ("int", 8, False), "size_t": ("int", 8, False),
    "ssize_t": ("int", 8, True), "intptr_t": ("int", 8, True), "uintptr_t": ("int", 8, False),
    "ptrdiff_t": ("int", 8, True), "_Bool": ("int", 1, False), "bool": ("int", 1, False),
    "va_list": ("va_list", 8, None),
}


class CType:
    """A value's type as the ABIs see it: kind is int, float, pointer, callback (a function pointer),
    struct, char, long double, va_list or void."""

    def __init__(self, kind, size, signed, text):
        self.kind, self.size, self.signed, self.text = kind, size, signed, text

    def __repr__(self):
        return f"CType({self.kind}, {self.size}, {self.signed}, {self.text!r})"


class Prototype:
    def __init__(self, name, result, params):
        self.name, self.result, self.params = name, result, params


def sysroot(ndk):
    prebuilt = glob.glob(os.path.join(ndk, "toolchains", "llvm", "prebuilt", "*"))
    if not prebuilt:
        raise FileNotFoundError(f"no NDK at {ndk}")
    return prebuilt[0]


def ndk_exports(ndk, lib, api=35):
    """The names that the NDK's stubs of a front lib export at an API level, per library."""
    prebuilt = sysroot(ndk)
    nm = os.path.join(prebuilt, "bin", "llvm-nm")
    stubs = os.path.join(prebuilt, "sysroot", "usr", "lib", "aarch64-linux-android", str(api))
    names = {}
    for library in LIBRARIES[lib]:
        result = subprocess.run([nm, "-D", "--defined-only", "--format=just-symbols", os.path.join(stubs, library)],
                                check=True, capture_output=True, text=True)
        names[library] = {line.strip() for line in result.stdout.splitlines() if line.strip()}
    return names


class Headers:
    """The prototypes and types of one front lib's headers: the NDK's, or with `include` the host library's
    (HOST_HEADERS)."""

    def __init__(self, ndk, lib, include=None):
        headers = HOST_HEADERS[lib] if include else HEADERS[lib]
        include = include or os.path.join(sysroot(ndk), "sysroot", "usr", "include")
        self.typedefs, self.callbacks, self.structs, self.enums, self.handles = {}, set(), set(), set(), set()
        texts = []
        for header in headers:
            with open(os.path.join(include, header)) as file:
                text = COMMENT.sub(" ", file.read())
            texts.append(text)
            self._collect(text)
            if header == "EGL/eglplatform.h":
                # Only Android's branch of the platform types counts; it is read last, so it wins.
                self._collect(ANDROID_BRANCH.search(text).group(1))
        self.prototypes = {}
        for text in texts:
            for result, name, params in PROTOTYPE[lib].findall(text):
                self.prototypes[name] = Prototype(name, self.parse(result, named=False), self.parse_params(params))

    def _collect(self, text):
        for old, new in SIMPLE_TYPEDEF.findall(text):
            self.typedefs[new] = old
        self.callbacks.update(FUNCTION_TYPEDEF.findall(text))
        for tag, name in TAGGED_TYPEDEF.findall(text):
            (self.enums if tag == "enum" else self.structs).add(name)
        self.handles.update(HANDLE.findall(text))

    def parse_params(self, text):
        text = " ".join(text.split())
        if text in ("", "void"):
            return []
        return [self.parse(param, named=True) for param in text.split(",")]

    def parse(self, text, named):
        """The CType of a parameter declaration (with its name) or of a result type."""
        text = " ".join(text.split())
        if "(" in text:
            raise ValueError(f"inline function pointer: {text}")
        array = "[" in text
        text = re.sub(r"\[[^\]]*\]", "", text).strip()
        if named:
            match = re.match(r"^(.*?)\b(\w+)$", text)
            if match and re.findall(r"\w+", match.group(1)):
                text = match.group(1)
        if array or "*" in text:
            return CType("pointer", 8, False, text)
        words = [word for word in re.findall(r"\w+", text) if word not in ("const", "volatile")]
        return self.resolve(" ".join(words), text)

    def resolve(self, name, text):
        if name.startswith("enum "):
            return CType("int", 4, True, text)
        if name.startswith(("struct ", "union ")):
            return CType("struct", 0, None, text)
        if name in BUILTIN:
            kind, size, signed = BUILTIN[name]
            return CType(kind, size, signed, text)
        if name in self.handles:
            return CType("pointer", 8, False, text)
        if name in self.callbacks:
            return CType("callback", 8, False, text)
        if name in self.enums:
            return CType("int", 4, True, text)
        if name in self.structs:
            return CType("struct", 0, None, text)
        if name in self.typedefs:
            old = self.typedefs[name]
            if "*" in old:
                return CType("pointer", 8, False, text)
            words = [word for word in re.findall(r"\w+", old) if word not in ("const", "volatile")]
            resolved = self.resolve(" ".join(words), text)
            return CType(resolved.kind, resolved.size, resolved.signed, text)
        raise KeyError(f"unknown type {name!r} in {text!r}")


def places(params):
    """Per argument its place: ("x", n) or ("v", n), the same register in both ABIs, or ("stack", aapcs,
    apple), its offset in AAPCS64's stack area (an 8-byte slot each) and in Apple's (natural size and
    alignment)."""
    general = floating = 0
    result, offset = [], 0
    for param in params:
        if param.kind == "float" and floating < 8:
            result.append(("v", floating))
            floating += 1
        elif param.kind != "float" and general < 8:
            result.append(("x", general))
            general += 1
        else:
            offset = (offset + param.size - 1) // param.size * param.size
            result.append(("stack", 8 * sum(place[0] == "stack" for place in result), offset))
            offset += param.size
    return result


def stack_places(params):
    """The offsets of the arguments that go on the stack, in AAPCS64 and in Apple's ABI, and whether one
    of them is smaller than 32 bits."""
    stack = [(place, param) for place, param in zip(places(params), params) if place[0] == "stack"]
    return ([place[1] for place, _ in stack], [place[2] for place, _ in stack],
            any(param.kind == "int" and param.size < 4 for _, param in stack))


def classify(prototype):
    """same, extend or repack, or "refused: <why>" for what never crosses between the two ABIs."""
    for what, value in [("result", prototype.result)] + [("argument", param) for param in prototype.params]:
        if value.kind in ("struct", "char", "long double", "va_list"):
            return f"refused: {value.kind} {what} {value.text!r}"
    aapcs, apple, small_on_stack = stack_places(prototype.params)
    if aapcs != apple or small_on_stack:
        return "repack"
    general = 0
    for param in prototype.params:
        if param.kind == "float":
            continue
        general += 1
        if general <= 8 and param.kind == "int" and param.size < 4:
            return "extend"
    return "same"


def takes_allocator(prototype):
    return any("VkAllocationCallbacks" in param.text for param in prototype.params)
