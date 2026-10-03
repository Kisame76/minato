#!/usr/bin/env python3
"""tools/hwui-plan.py (ADR-0043 a1): the graphics set's Android.bp files as a CMake plan, as Soong builds hwui's host
flavor, on the tag's sources (the lists that tools/build-hwui.sh, tools/build-art.sh and tools/build-bionic.sh fetch;
without them the test skips). It must plan without problems, and the plan must build what the host flavor builds:
- libhwui with -DHWUI_NULL_GPU, -std=gnu++2b (Soong's "experimental") and the host's render thread (platform/host),
  without the device's (EGL, Vulkan, the graphicsstats proto), with its system properties generated from
  HWUIProperties.sysprop, and with the guest system's libbase and libziparchive as shared libraries, never compiled in;
- Skia without its GPU sources and without its RAW codec, whose libraries the plan never names;
- minikin without its Rust hyphenator (-DMINATO_WITHOUT_RUST), whose FFI library the plan never names, with the
  text flags it includes on its __ANDROID__ side, and against the ICU4C headers of the libicuuc and libicui18n it
  links, with -DANDROID_LINK_SHARED_ICU4C as libhwui, not libicu's NDK headers, which only the FFI library's C++
  half exports and a Rust library does not pass on;
- the NDK libraries of the graphics AIDL interfaces, which no source of the host flavor includes, never named;
- the includes of hwui's and libcutils' units end with Soong's global includes that the tree holds, in Soong's order,
  and the plan names none that it does not hold;
- the host flavor's side of __ANDROID__: the tree's C and C++ files of frameworks/base/libs, libutils, libcutils and
  incremental_delivery test __ANDROID__ no more, each folder through a copy of its own, while every other real file of
  the tree is one that a patch changes (bionic, Skia and minikin keep their tests); and libhwui compiles with
  -DANDROID_LINK_SHARED_ICU4C: it links libicuuc itself, as the host flavor does, which ICU's uconfig_local.h stops
  on __ANDROID__ without the flag;
- patch 0003's #include <memory> in HintSessionWrapper.h, which the NDK's libc++ no longer brings with <future>, and
  patch 0004's Bitmap.cpp without libbinder's IServiceManager.h, of which it uses nothing, and with the <stdlib.h> of
  its calloc and free;
- no library with a max page size of its own (a2): our linker_hugepage_aligned in place of bionic's leaves out its
  2 MiB, so libhwui takes graphics/hwui/CMakeLists.txt's 16 KB, the guest's pages, as the rest of the set does;
- libhwui with the device's version script, libhwui.map.txt (a2), which the host flavor lacks: the guest sees what
  Android 15's libhwui exports, and the libpng and libcutils members that the host flavor's static libandroidfw
  brings into libhwui stay local instead of preempting libpng.so's and libcutils.so's. Patch 0005 takes out the 14
  names that only the device flavor builds, which lld refuses as undefined: the ABitmap C API and GraphicsStatsService's
  registration.
Usage: hwui_plan_test.py [--scratch <dir>]"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "hwui-plan.py")
SOURCES = os.path.join(ROOT, "build", "sources")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
# Soong's global includes (cc/config/global.go's commonGlobalIncludes at the tag) that the graphics set's tree holds,
# in Soong's order, and a header the set includes through each.
GLOBAL_INCLUDES_HELD = ["system/core/include", "system/logging/liblog/include", "hardware/libhardware/include",
                        "frameworks/native/include", "frameworks/native/opengl/include"]
GLOBAL_HEADERS = ["system/core/include/utils/Compat.h", "hardware/libhardware/include/hardware/hardware.h",
                  "frameworks/native/include/android/configuration.h", "frameworks/native/include/gui/TraceUtils.h",
                  "frameworks/native/opengl/include/GLES/gl.h"]
# The folders whose C and C++ files take the host side of __ANDROID__ (tools/hwui-plan.py's HOST_SIDE), each with a
# file that tests __ANDROID__ in the sources.
HOST_SIDE = {"frameworks/base/libs": "frameworks/base/libs/hwui/hwui/Bitmap.cpp",
             "system/core/libutils": "system/core/libutils/Threads.cpp",
             "system/core/libcutils": "system/core/libcutils/sockets_unix.cpp",
             "system/incremental_delivery": "system/incremental_delivery/incfs/util/map_ptr.cpp"}
C_FILES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")
ANDROID_TEST = re.compile(r"^[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif)\b.*__ANDROID__", re.M)
PATCHES = os.path.join(ROOT, "graphics", "hwui", "patches")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def unit(plan, target):
    """The text of the plan's call that defines `target`, or ""."""
    match = re.search(r"^hwui_unit\(" + re.escape(target) + r"\n(.*?)\)$", plan, re.M | re.S)
    return match.group(1) if match else ""


def library(plan, name):
    """The text of the plan's call that links the library `name`, or ""."""
    match = re.search(r"^hwui_library\(" + re.escape(name) + r"\n(.*?)\)$", plan, re.M | re.S)
    return match.group(1) if match else ""


def field(text, key):
    """The values of a field of a call's text (INCLUDES, ARCHIVES, NEEDED, ...), in order and unquoted."""
    match = re.search(r"^  " + key + r" (.*)$", text, re.M)
    return [value.strip('"') for value in match.group(1).split()] if match else []


def missing_sources():
    """What the plan reads below build/sources and is not there, each with the list of tools/fetch-sources.py that
    has it."""
    spec = importlib.util.spec_from_file_location("hwui_plan", TOOL)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    with open(MANIFEST) as handle:
        manifest = json.load(handle)
    lists = {source["name"]: name for name, sources in manifest.items() if isinstance(sources, list)
             for source in sources}
    paths = list(tool.TREE.values()) + list(tool.FLAGS.values()) + [tool.VALUES] + list(tool.SYSPROPS)
    return [(path, lists.get(path.split("/")[0], "?")) for path in paths
            if not os.path.exists(os.path.join(SOURCES, path))]


def check_host_side(tree, hwui):
    """The tree's host side of __ANDROID__, patch 0003 and ICU's flag (see the docstring)."""
    for folder, example in HOST_SIDE.items():
        path = os.path.join(tree, example)
        check(os.path.isfile(path) and not os.path.islink(path) and "__ANDROID__" not in open(path).read(),
              f"{example} is a copy of the host side, without __ANDROID__")
    patched = set()
    for patch in os.listdir(PATCHES):
        with open(os.path.join(PATCHES, patch)) as handle:
            patched.update(re.findall(r"^\+\+\+ b/(\S+)", handle.read(), re.M))
    left, copied_outside = [], []
    for directory, _, files in os.walk(tree):
        for name in files:
            path = os.path.join(directory, name)
            tree_path = os.path.relpath(path, tree)
            inside = any(tree_path.startswith(folder + "/") for folder in HOST_SIDE)
            if inside and name.endswith(C_FILES):
                with open(path, "rb") as handle:
                    if ANDROID_TEST.search(handle.read().decode("utf-8", "replace")):
                        left.append(tree_path)
            elif not inside and not os.path.islink(path) and tree_path not in patched:
                copied_outside.append(tree_path)
    check(not left, f"no C or C++ file of the host side folders tests __ANDROID__: {left[:5]}")
    check(not copied_outside, f"the tree's real files outside them are the patched ones: {copied_outside[:5]}")
    wrapper = os.path.join(tree, "frameworks/base/libs/hwui/renderthread/HintSessionWrapper.h")
    check(os.path.exists(wrapper) and "#include <memory>" in open(wrapper).read(),
          "HintSessionWrapper.h includes <memory> (patch 0003)")
    bitmap = os.path.join(tree, HOST_SIDE["frameworks/base/libs"])
    bitmap_text = open(bitmap).read() if os.path.exists(bitmap) else ""
    check(bitmap_text and "binder/" not in bitmap_text, "Bitmap.cpp includes nothing of libbinder (patch 0004)")
    check("#include <stdlib.h>" in bitmap_text, "Bitmap.cpp includes <stdlib.h> for calloc and free (patch 0004)")
    check('"-DANDROID_LINK_SHARED_ICU4C"' in hwui, "libhwui compiles with -DANDROID_LINK_SHARED_ICU4C")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scratch")
    args = parser.parse_args()
    missing = missing_sources()
    if missing:
        path, listed = missing[0]
        print(f"skipped: build/sources/{path} is missing, see tools/fetch-sources.py --set {listed}")
        return 0
    with tempfile.TemporaryDirectory() as temporary:
        scratch = args.scratch or temporary
        os.makedirs(scratch, exist_ok=True)
        output = os.path.join(scratch, "plan.cmake")
        result = subprocess.run([sys.executable, TOOL, SOURCES, output], capture_output=True, text=True)
        check(result.returncode == 0, f"hwui-plan: exit {result.returncode}:\n{result.stdout}{result.stderr}")
        plan = open(output).read() if os.path.exists(output) else ""
        hwui = unit(plan, "hwui_libhwui_shared")
        check("hwui_library(libhwui\n" in plan, "the plan links libhwui")
        check('"-DHWUI_NULL_GPU"' in hwui, "libhwui compiles with -DHWUI_NULL_GPU")
        check("/frameworks/base/libs/hwui/platform/host/renderthread/RenderThread.cpp" in hwui,
              "libhwui compiles the host's render thread")
        for device in ("/libs/hwui/renderthread/EglManager.cpp", "/libs/hwui/renderthread/VulkanManager.cpp",
                       "graphicsstats"):
            check(device not in hwui, f"libhwui leaves out {device}")
        check("/minato/hwui/generated/sysprop/HWUIProperties.sysprop.cpp" in hwui,
              "libhwui compiles its generated system properties")
        check('"-std=gnu++2b"' in hwui, 'libhwui compiles with -std=gnu++2b, what Soong compiles for "experimental"')
        check("max-page-size" not in plan, "no library of the plan sets a max page size of its own")
        hwui_library = library(plan, "libhwui")
        script = (field(hwui_library, "VERSION_SCRIPT") or [""])[0]
        check(script.endswith("/frameworks/base/libs/hwui/libhwui.map.txt"),
              "libhwui links with the device's version script, libhwui.map.txt")
        script_text = open(script).read() if os.path.isfile(script) else ""
        check(script_text and "ABitmap" not in script_text and "GraphicsStatsService" not in script_text,
              "the version script names neither the ABitmap C API nor GraphicsStatsService, which only the device "
              "flavor builds (patch 0005)")
        check("libziparchive" in field(hwui_library, "NEEDED"), "libhwui needs the guest system's libziparchive")
        for archive in ("hwui_libbase_static", "hwui_libziparchive_static"):
            check(archive not in field(hwui_library, "ARCHIVES"),
                  f"libhwui links no {archive}: the guest system has the library as a shared one")
        skia = unit(plan, "hwui_libskia_static")
        check("/external/skia/src/core/SkCanvas.cpp" in skia, "the plan compiles Skia")
        check("/external/skia/src/gpu/" not in skia, "Skia compiles none of its GPU sources")
        check("SkRawCodec.cpp" not in skia, "Skia leaves its RAW codec out")
        minikin = unit(plan, "hwui_libminikin_shared")
        check("hwui_library(libminikin\n" in plan, "the plan links libminikin")
        check('"-DMINATO_WITHOUT_RUST"' in minikin, "minikin compiles without its Rust hyphenator")
        check("/minato/hwui/generated/flags" in minikin, "minikin finds the generated text flags")
        check('"-DANDROID_LINK_SHARED_ICU4C"' in minikin, "minikin compiles with -DANDROID_LINK_SHARED_ICU4C")
        check("/external/icu/libicu/ndk_headers" not in minikin, "minikin compiles without libicu's NDK headers")
        for left_out in ("libminikin_rust_ffi", "libdng_sdk", "libpiex", "android.hardware.graphics.common-V5-ndk",
                         "android.hardware.graphics.composer3-V3-ndk", "libaconfig_storage_read_api_cc"):
            check(left_out not in plan, f"the plan never names {left_out}")
        tree = os.path.join(os.path.dirname(os.path.abspath(output)), "tree")
        for header in GLOBAL_HEADERS:
            check(os.path.exists(os.path.join(tree, header)), f"the tree holds {header}")
        held = [os.path.join(tree, include) for include in GLOBAL_INCLUDES_HELD]
        for target in ("hwui_libhwui_shared", "hwui_libcutils_static"):
            check(field(unit(plan, target), "INCLUDES")[-len(held):] == held,
                  f"{target} ends its includes with Soong's global includes the tree holds, in Soong's order")
        for absent in ("frameworks/av/include", "system/media/audio/include", "hardware/ril/include"):
            check(f"/{absent}\"" not in plan, f"the plan never names {absent}, which the tree does not hold")
        check_host_side(tree, hwui)
    print(f"hwui-plan: {'all checks passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
