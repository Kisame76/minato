#!/usr/bin/env python3
"""tools/hwui-plan.py (ADR-0043 a1): the graphics set's Android.bp files as a CMake plan, as Soong builds hwui's host
flavor, on the tag's sources (the lists that tools/build-hwui.sh, tools/build-art.sh and tools/build-bionic.sh fetch;
without them the test skips). It must plan without problems, and the plan must build what the host flavor builds:
- libhwui with -DHWUI_NULL_GPU, -std=gnu++2b (Soong's "experimental") and the host's render thread (platform/host),
  without the device's (EGL, Vulkan, the graphicsstats proto), with its system properties generated from
  HWUIProperties.sysprop, and with the guest system's libbase and libziparchive as shared libraries, never compiled in;
- Skia without its GPU sources and without its RAW codec, whose libraries the plan never names;
- minikin without its Rust hyphenator (-DMINATO_WITHOUT_RUST), whose FFI library the plan never names, and with the
  text flags it includes on its __ANDROID__ side;
- the NDK libraries of the graphics AIDL interfaces, which no source of the host flavor includes, never named;
- the includes of hwui's and libcutils' units end with Soong's global includes that the tree holds, in Soong's order,
  and the plan names none that it does not hold.
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
        hwui_library = library(plan, "libhwui")
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
    print(f"hwui-plan: {'all checks passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
