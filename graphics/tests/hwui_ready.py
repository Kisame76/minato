#!/usr/bin/env python3
"""Whether an app process can start with hwui (ADR-0043 test 2): app_process registers hwui's graphics natives at
every start, and ActivityThread loads the system's font map. So it needs:
  - hwui's libraries (tools/build-hwui.sh), built with graphics/hwui/patches as they are: a build of older patches
    crashes the start (patch 0007). The build lists its patches in patches.sha256 beside its lib folder;
  - bionic's two font lists in its /system (tools/build-bionic.sh), as Android has them: font_fallback.xml, without
    which the font map is empty and Typeface.setSystemFontMap ends the start with a NullPointerException, and the
    legacy fonts.xml, which Typeface's class init reads first.
framework/tests/app_process_run.py's missing() asks it before every start, and the checks that start app processes
before they build app_process: those print what it prints and stop.

Usage: hwui_ready.py <hwui-lib> <bionic-system>: prints "skipped: ..." while a part is not ready, nothing else.
"""
import os
import sys

from hwui_load_run import built_digests, patch_digests

FONT_LISTS = ["font_fallback.xml", "fonts.xml"]


def missing(hwui_lib, bionic_system):
    """The first part that is not ready, as "skipped: ..." text, or None."""
    library = os.path.join(hwui_lib, "libhwui.so")
    if not os.path.exists(library):
        return f"skipped: {library} is not built (tools/build-hwui.sh)"
    if built_digests(hwui_lib) != patch_digests():
        build = os.path.dirname(os.path.normpath(hwui_lib))
        return f"skipped: {build} is not built with graphics/hwui/patches as they are (tools/build-hwui.sh)"
    for name in FONT_LISTS:
        path = os.path.join(bionic_system, "etc", name)
        if not os.path.exists(path):
            return f"skipped: {path} is not built (tools/build-bionic.sh)"
    return None


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    skipped = missing(sys.argv[1], sys.argv[2])
    if skipped:
        print(skipped)


if __name__ == "__main__":
    main()
