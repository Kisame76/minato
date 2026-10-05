#!/usr/bin/env python3
"""Turns the Android.bp files of the graphics set (hwui, Skia, minikin, HarfBuzz, FreeType, the image codecs, what
they include and libjnigraphics over hwui) into a CMake plan for guest code (ADR-0043 a1, ADR-0041 a5), as Soong builds
hwui's host flavor: HWUI_NULL_GPU, without the GPU, the render thread's device side or the services.
graphics/hwui/CMakeLists.txt builds the plan with the NDK compiler.

The modules are resolved for Soong's linux_bionic host on arm64: the host's branches, which hold the host flavor, with
bionic's and arm64's. graphics/hwui/modules.bp, read in the same syntax, holds what Minato puts in place of modules of
projects we do not fetch, of Rust and of generators; graphics/hwui/overlay and graphics/hwui/patches change sources
as runtime/art's do for ART (tools/soong_plan.py).

Usage: tools/hwui-plan.py <sources-directory> <output.cmake>
"""
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blueprint  # noqa: E402
import soong_plan  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(ROOT, "graphics", "hwui")  # with modules.bp, overlay and patches

# Where the projects of the partial AOSP tree lie below build/sources (tools/fetch-sources.py's sets graphics, art,
# framework and sources): a project fetched in parts maps each part.
TREE = {
    "frameworks/base/libs/hwui": "frameworks_base_hwui/libs/hwui",
    "frameworks/base/libs/hostgraphics": "frameworks_base_hwui/libs/hostgraphics",
    "frameworks/base/libs/androidfw": "frameworks_base/libs/androidfw",
    "frameworks/base/native/graphics/jni": "frameworks_base_jnigraphics/native/graphics/jni",
    "external/skia": "skia",
    "frameworks/minikin": "minikin",
    "external/harfbuzz_ng": "harfbuzz_ng",
    "external/freetype": "freetype",
    "external/libjpeg-turbo": "libjpeg-turbo",
    "external/webp": "webp",
    "external/wuffs-mirror-release-c": "wuffs",
    "external/libultrahdr": "libultrahdr",
    "external/image_io": "image_io",
    "external/modp_b64": "modp_b64",
    "external/libpng": "libpng",
    "external/zlib": "zlib",
    "external/expat": "expat",
    "external/fmtlib": "fmtlib",
    "external/icu": "icu",
    "external/vulkan-headers": "vulkan_headers_aosp",
    "external/googletest": "googletest_prod",
    "frameworks/native": "frameworks_native_graphics",
    "frameworks/native/include/android": "frameworks_native_headers/include/android",
    "frameworks/native/include/gui": "frameworks_native_headers/libs/gui/include/gui",  # AOSP's link to it
    "frameworks/native/opengl/include": "frameworks_native_headers/opengl/include",
    "frameworks/av": "frameworks_av_ndk",
    "hardware/interfaces": "hardware_interfaces_graphics",
    "hardware/libhardware/include/hardware": "libhardware/include_all/hardware",  # AOSP's links to them
    "system/core/include/cutils": "system_core_framework/libcutils/include/cutils",  # AOSP's links to them
    "system/core/include/system": "system_core_framework/libsystem/include/system",
    "system/core/include/utils": "system_core_framework/libutils/include/utils",
    "system/core/libcutils": "system_core_framework/libcutils",
    "system/core/libutils": "system_core_framework/libutils",
    "system/core/libsystem": "system_core_framework/libsystem",
    "system/libbase": "libbase",
    "system/libziparchive": "libziparchive",
    "system/incremental_delivery": "incremental_delivery",
    "system/logging": "logging",
    "system/server_configurable_flags": "server_configurable_flags",
    "libnativehelper": "libnativehelper",
    "bionic": "bionic",
}

# What the plan generates before it plans, into <build>/generated, which the tree holds at minato/hwui/generated, as
# Soong generates it for its modules: the aconfig flags of hwui and of the text stack as constants of Android 15's
# release values (tools/aconfig-flags.py cc) and hwui's system properties (tools/sysprop.py cc). graphics/hwui's
# modules.bp exports the flags; ADDITIONS compiles the properties.
FLAGS = {"com.android.graphics.hwui.flags": "frameworks_base_hwui/libs/hwui/aconfig",
         "com.android.text.flags": "frameworks_base/core/java/android/text/flags"}
VALUES = "build_release/aconfig/ap3a"
SYSPROPS = ["frameworks_base_hwui/libs/hwui/HWUIProperties.sysprop"]
GENERATED = "minato/hwui/generated"

# Minato's additions to the set's modules, merged as Soong merges defaults:
# - minikin builds without its Rust hyphenator (patch 0001) and includes the text flags on its __ANDROID__ side, which
#   Android 15 takes;
# - hwui compiles its generated system properties, which Soong makes of HWUIProperties.sysprop in its sources;
# - Skia leaves its RAW codec out (patch 0002);
# - minikin's own folder is not an include directory: its sources include their headers by quotes from it, and on the
#   Mac's case-insensitive file system its Locale.h would answer libc++'s #include <locale.h>;
# - androidfw leaves CursorWindow out, which the host's Linux flavor builds on libbinder, a service Minato does not
#   have;
# - hwui and minikin link libicuuc and libicui18n themselves, as the host flavor does, so ICU's check for Android
#   libraries that should use libandroidicu (android_icu4c's uconfig_local.h), which the NDK's __ANDROID__ wakes, lets
#   them;
# - libhwui links with the device's version script, target.android's libhwui.map.txt, which the host flavor lacks
#   (a2): the guest sees what Android 15's libhwui exports, and the libpng and libcutils members that the host
#   flavor's static libandroidfw brings in stay local instead of preempting libpng.so's and libcutils.so's;
# - the graphics APEX's sources take the device's apex/android_bitmap.cpp too, the ABitmap C API that GLUtils' natives
#   and libjnigraphics use (ADR-0041 a3), without ABitmap_getHardwareBuffer (patch 0009);
# - libjnigraphics takes its module's device side (ADR-0041 a5), as target.android has it: bitmap.cpp, the NDK's
#   AndroidBitmap functions, and aassetstreamadaptor.cpp, AImageDecoder_createFromAAsset's stream, beside
#   imagedecoder.cpp, which both sides compile, with Minato's libandroid and its version script, so that the guest
#   sees what Android 15's libjnigraphics exports.
ADDITIONS = {
    "libskia": {"exclude_srcs": ["src/codec/SkRawCodec.cpp"]},
    "libminikin": {"cflags": ["-DMINATO_WITHOUT_RUST", "-DANDROID_LINK_SHARED_ICU4C"],
                   "header_libs": ["aconfig_text_flags_c_lib"],
                   "include_build_directory": False},
    "libandroidfw": {"exclude_srcs": ["CursorWindow.cpp"]},
    "libhwui_defaults": {"srcs": [f"../../../../{GENERATED}/sysprop/HWUIProperties.sysprop.cpp"],
                         "include_dirs": [f"{GENERATED}/sysprop"], "cflags": ["-DANDROID_LINK_SHARED_ICU4C"]},
    "libhwui": {"version_script": "libhwui.map.txt"},
    "android_graphics_apex": {"srcs": ["apex/android_bitmap.cpp"]},
    "libjnigraphics": {"srcs": ["aassetstreamadaptor.cpp", "bitmap.cpp"], "shared_libs": ["libandroid"],
                       "version_script": "libjnigraphics.map.txt"},
}

# Soong's global includes at the tag (cc/config/global.go's commonGlobalIncludes), which it gives platform and host
# modules after their own: hwui's sources and those of what it links include the NDK's headers of frameworks/native,
# GLES, libhardware and libutils through them. The plan names those the tree holds.
GLOBAL_INCLUDES = ["system/core/include", "system/logging/liblog/include", "system/media/audio/include",
                   "hardware/libhardware/include", "hardware/libhardware_legacy/include", "hardware/ril/include",
                   "frameworks/native/include", "frameworks/native/opengl/include", "frameworks/av/include"]

# The projects of hwui's host flavor: Soong builds it for the host, where __ANDROID__ is undefined, so their files take
# the host side of its tests (tools/soong_plan.py's make_tree). The system code they build on keeps its Android side:
# bionic, libc++, Skia, the codecs.
HOST_SIDE = ["frameworks/base/libs", "system/core/libutils", "system/core/libcutils", "system/incremental_delivery"]

# Soong's target groups of the linux_bionic host on arm64: the host flavor's branches with bionic's and arm64's.
TARGET_GROUPS = ["host", "linux", "bionic", "linux_bionic", "not_windows", "host_linux", "linux_arm64", "bionic_arm64",
                 "linux_bionic_arm64"]

# The guest system's libraries, which the plan links as they are and builds none of: bionic's (tools/build-bionic.sh),
# with Minato's libandroid, which libjnigraphics links, and those of ART's build that the set links
# (tools/build-art.sh).
GUEST_SYSTEM = ["libc", "libm", "libdl", "libdl_android", "ld-android", "liblog", "libz", "libandroid", "libbase",
                "libexpat", "libicu", "libicui18n", "libicuuc", "libnativehelper", "libziparchive"]

# What the plan builds: libhwui, with what it links (ADR-0043 a1), and libjnigraphics over it (ADR-0041 a5).
LIBRARIES = ["libhwui", "libjnigraphics"]

# Dependencies left out: minikin's Rust hyphenator (its C++ one through patch 0001), the aconfig storage reader in
# Rust, which hwui's flags as constants of the release's values do not need, Skia's RAW decoding (patch 0002), and the
# NDK libraries of two AIDL interfaces, which hwui's defaults link and none of the host flavor's sources includes
# (only libui's gralloc and format code, which the host flavor does not build).
DROPPED = {"libminikin_rust_ffi", "libaconfig_storage_read_api_cc", "libdng_sdk", "libpiex",
           "android.hardware.graphics.common-V5-ndk", "android.hardware.graphics.composer3-V3-ndk"}


def generate(sources, folder):
    """Writes FLAGS' and SYSPROPS' outputs into `folder`, rewriting a file only when it changes, so that what includes
    it stays built."""
    tools = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as scratch:
        for package, declarations in FLAGS.items():
            subprocess.run([sys.executable, os.path.join(tools, "aconfig-flags.py"), "cc", "--declarations",
                            os.path.join(sources, declarations), "--values", os.path.join(sources, VALUES), "--package",
                            package, "--out", os.path.join(scratch, "flags")], check=True, stdout=subprocess.DEVNULL)
        for sysprop in SYSPROPS:
            subprocess.run([sys.executable, os.path.join(tools, "sysprop.py"), "cc", "--out",
                            os.path.join(scratch, "sysprop"), os.path.join(sources, sysprop)], check=True)
        for directory, _, files in os.walk(scratch):
            for name in files:
                made = os.path.join(directory, name)
                target = os.path.join(folder, os.path.relpath(made, scratch))
                if not os.path.exists(target) or not filecmp.cmp(made, target, shallow=False):
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    shutil.copyfile(made, target)


class HwuiPlanner(soong_plan.Planner):
    """The graphics set: graphics/hwui's modules.bp, overlay and patches, resolved for the linux_bionic host."""
    TREE = TREE
    OURS = "minato/hwui"
    OURS_FOLDER = FOLDER
    OVERLAY = os.path.join(FOLDER, "overlay")
    PATCHES = os.path.join(FOLDER, "patches")
    TARGET_GROUPS = TARGET_GROUPS
    GUEST_SYSTEM = GUEST_SYSTEM
    DROPPED = DROPPED
    LIBRARIES = LIBRARIES
    PREFIX = "hwui"
    GENERATED = "APK_HWUI_GENERATED"
    TREE_VARIABLE = "APK_HWUI_TREE"
    GLOBAL_INCLUDES = GLOBAL_INCLUDES
    HOST_SIDE = HOST_SIDE

    def __init__(self, sources, tree):
        generated = os.path.join(os.path.dirname(tree), "generated")
        generate(sources, generated)
        self.TREE = dict(TREE, **{GENERATED: generated})
        super().__init__(sources, tree)

    def adapt_kind(self, module):
        """ADDITIONS."""
        if module.name in ADDITIONS:
            module.properties = blueprint.merge(module.properties, ADDITIONS[module.name])


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    build = os.path.dirname(os.path.abspath(sys.argv[2]))
    planner = HwuiPlanner(os.path.abspath(sys.argv[1]), os.path.join(build, "tree"))
    lines = planner.plan_linked()
    if planner.problems:
        raise SystemExit("hwui-plan: " + "\n  ".join(["cannot plan the build:"] + sorted(set(planner.problems))))
    with open(sys.argv[2], "w") as handle:
        handle.write("# Generated by tools/hwui-plan.py from the Android.bp files of the graphics set. Do not edit.\n")
        handle.write("\n\n".join(lines) + "\n")
    units = [target for target in planner.units.values() if target]
    print(f"hwui-plan: {len(planner.linked)} libraries, {len(units)} compile units, {len(planner.generated)} generators")


if __name__ == "__main__":
    main()
