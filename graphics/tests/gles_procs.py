"""gles-procs-test's names (graphics/tests/gles_procs_test.c). Standard library only.

--candidates: every function the NDK's headers declare, as C macro calls, for the measurement on the emulator:
  GL(name)      GLES/gl.h, GLES/glext.h, GLES2/gl2.h, GLES2/gl2ext.h and GLES3/gl3*.h (ES 1 to 3.2, extensions)
  EGL(name)     EGL/egl.h and EGL/eglext.h
  VK(name, level)  vulkan/vulkan_core.h and vulkan/vulkan_android.h, level 0 global, 1 instance, 2 device (by the
                first parameter's type, as the loader dispatches)
  EXT("extension", name)  each GLES 2/3 extension function of gl2ext.h with its extension
--required <file>: Android 15's measurement (graphics/tests/gles_procs_android.txt, "<check> <name>" lines) as
  REQUIRED("check", name) calls, the names the test asks for: every one of the GLES and EGL checks; of the Vulkan
  checks those of Vulkan 1.0 to 1.3 and of the extensions gles_procs_test.c enables, as the rest of a Vulkan
  driver's functions follow its own extensions (the emulator's video and performance query functions, say).

Usage: gles_procs.py --ndk <NDK> --out <file> (--candidates | --required <file>)"""
import argparse
import glob
import os
import re

GL_PROTOTYPE = re.compile(r"GL_API(?:CALL)?\s+[^;]*?GL_APIENTRY\s+(\w+)\s*\(")
EGL_PROTOTYPE = re.compile(r"EGLAPI\s+[^;]*?EGLAPIENTRY\s+(\w+)\s*\(")
VK_PROTOTYPE = re.compile(r"VKAPI_ATTR\s+[^;]*?VKAPI_CALL\s+(\w+)\s*\(\s*(?:const\s+)?(\w+)")
EXTENSION = re.compile(r"#ifndef\s+(GL_[A-Z0-9]+_\w+)\s*\n#define\s+\1\s+1")
VK_FEATURE = re.compile(r"#define\s+(VK_(?:VERSION_\d_\d|(?!USE_)[A-Z]+_[a-z]\w*))\s+1\b")
VK_REQUIRED = {"VK_VERSION_1_0", "VK_VERSION_1_1", "VK_VERSION_1_2", "VK_VERSION_1_3", "VK_KHR_surface",
               "VK_KHR_android_surface", "VK_KHR_swapchain", "VK_ANDROID_external_memory_android_hardware_buffer",
               "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_external_memory", "VK_EXT_queue_family_foreign",
               "VK_KHR_dedicated_allocation"}
GLOBAL = {"vkCreateInstance", "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties",
          "vkEnumerateInstanceVersion", "vkGetInstanceProcAddr"}
DEVICE_TYPES = {"VkDevice", "VkQueue", "VkCommandBuffer"}


def include(ndk):
    found = glob.glob(os.path.join(ndk, "toolchains", "llvm", "prebuilt", "*", "sysroot", "usr", "include"))
    if not found:
        raise SystemExit(f"no NDK sysroot below {ndk}")
    return found[0]


def read(path):
    with open(path) as source:
        return source.read()


def names(root, headers, pattern):
    seen = []
    for header in headers:
        for name in pattern.findall(read(os.path.join(root, header))):
            name = name if isinstance(name, str) else name[0]
            if name not in seen:
                seen.append(name)
    return seen


def candidates(root):
    lines = []
    gl = names(root, ["GLES/gl.h", "GLES/glext.h", "GLES2/gl2.h", "GLES2/gl2ext.h", "GLES3/gl3.h", "GLES3/gl31.h",
                      "GLES3/gl32.h"], GL_PROTOTYPE)
    lines += [f"GL({name})" for name in gl]
    lines += [f"EGL({name})" for name in names(root, ["EGL/egl.h", "EGL/eglext.h"], EGL_PROTOTYPE)]
    seen = set()
    for header in ("vulkan/vulkan_core.h", "vulkan/vulkan_android.h"):
        for name, first in VK_PROTOTYPE.findall(read(os.path.join(root, header))):
            if name in seen:
                continue
            seen.add(name)
            level = 0 if name in GLOBAL else 2 if first in DEVICE_TYPES and name != "vkGetDeviceProcAddr" else 1
            lines.append(f"VK({name}, {level})")
    text = read(os.path.join(root, "GLES2/gl2ext.h"))
    for found in EXTENSION.finditer(text):
        end = text.find("#endif /* " + found.group(1) + " */", found.end())
        for name in GL_PROTOTYPE.findall(text[found.end():end if end >= 0 else len(text)]):
            lines.append(f'EXT("{found.group(1)}", {name})')
    return lines


def vk_features(root):
    """Each Vulkan command's feature: the version or extension whose "#define VK_... 1" block declares it."""
    features = {}
    for header in ("vulkan/vulkan_core.h", "vulkan/vulkan_android.h"):
        text = read(os.path.join(root, header))
        blocks = [(found.start(), found.group(1)) for found in VK_FEATURE.finditer(text)
                  if not found.group(1).endswith(("_SPEC_VERSION", "_EXTENSION_NAME"))]
        for command in VK_PROTOTYPE.finditer(text):
            before = [feature for start, feature in blocks if start < command.start()]
            if before:
                features.setdefault(command.group(1), before[-1])
    return features


def required(path, root):
    features = vk_features(root)
    lines = []
    for line in read(path).splitlines():
        words = line.split("#")[0].split()
        if len(words) != 2:
            continue
        if words[0].startswith("vk:") and features.get(words[1]) not in VK_REQUIRED:
            continue
        lines.append(f'REQUIRED("{words[0]}", {words[1]})')
    return lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--candidates", action="store_true")
    parser.add_argument("--required")
    args = parser.parse_args()
    root = include(args.ndk)
    body = candidates(root) if args.candidates else required(args.required, root)
    with open(args.out, "w") as out:
        out.write("/* Generated by graphics/tests/gles_procs.py; do not edit. */\n" + "\n".join(body) + "\n")


if __name__ == "__main__":
    main()
