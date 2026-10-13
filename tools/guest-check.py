#!/usr/bin/env python3
"""Checks the guest system in a built Minato.app (ADR-0039 c4): everything a Java app's start needs under Guest/, as
tools/embed-guest-system.sh lays it out and game mode mounts it, beside what a NativeActivity app's start needs; and
every container in Guest/ passes `codesign -v`.

  bin/              activity-host and app_process64, the programs /system/bin/activity-host and app_process64
  lib64/            /system/lib64: bionic, libandroid, the front libs, libandroid_runtime.so with libsqlite.so (the
                    compatibility plan's gap 7), hwui's set with libjnigraphics (ADR-0041 a5) and ART's copies for it
                    (graphics/tests/hwui_load_run.py's PLATFORM_COPIES)
  system/           /system's files: framework.jar and the framework-res folder in framework/ (ADR-0039, decision 8),
                    platform.xml in etc/permissions/, public.libraries.txt, the time zones and the fonts, and an
                    empty etc/security/cacerts, a name the runtime links to Conscrypt's certificates
  apex/             /apex's files: the boot jars that BOOTCLASSPATH names (runtime/src/environment.cpp), ICU's data,
                    the tzdata module's files (the compatibility plan's gap 21), and Conscrypt's CA certificates in
                    its cacerts/, the files of build/sources/ca-certificates (ADR-0046 decision 4)
  apex-lib64/       the APEXes' lib64: libart.so and libandroidicu.so (libsqlite's ICU) in ART's, libjavacrypto.so in
                    Conscrypt's
  resource-cache/   /data/resource-cache for a Java app (ADR-0043 f)): each overlay of
                    framework/monet-overlays.txt and its idmap

system/, apex/ and resource-cache/ are what the guest's file system mounts (GuestStart.swift), and the runtime
resolves every guest path below its mount's folder with O_RESOLVE_BENEATH: a link whose walk leaves it answers EACCES
(ADR-0052), even one that comes back or names a file inside by an absolute text. So a link there must stay in its
folder at every step; the folder itself may be a link, as apkrt_mount takes the host path's realpath.

A missing path, an empty file, CA certificates other than build/sources/ca-certificates/files, a container that
fails `codesign -v` and a link that leaves its mounted folder are named; the run ends with 1 then.
Runs on /usr/bin/python3 (3.9).
Usage: tools/guest-check.py <Minato.app>
"""
import filecmp
import os
import subprocess
import sys

HWUI = ["libcutils.so", "libft2.so", "libharfbuzz_ng.so", "libharfbuzz_subset.so", "libhwui.so", "libimage_io.so",
        "libjnigraphics.so", "libjpeg.so", "libjpegdecoder.so", "libjpegencoder.so", "libminikin.so", "libpng.so",
        "libultrahdr.so", "server_configurable_flags.so"]
ART_COPIES = ["libbase.so", "libziparchive.so", "libexpat.so", "libc++_shared.so"]
FRONT_LIBS = ["libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so"]
BIONIC = ["libc.so", "libdl.so", "libm.so", "liblog.so", "libstdc++.so", "libandroid.so", "libnativewindow.so",
          "libz.so"]
ART_JARS = ["core-oj.jar", "core-libart.jar", "okhttp.jar", "bouncycastle.jar", "core-icu4j.jar"]
# The tzdata module's files in etc/tz: bionic's tzdata with its tz_version, and in versioned/8 the same two for libcore,
# the lookups and ICU's time zone resources.
TZDATA_MODULE = (["tzdata", "tz_version"]
                 + [f"versioned/8/{name}" for name in ("tzdata", "tz_version", "tzlookup.xml", "telephonylookup.xml")]
                 + [f"versioned/8/icu/{name}.res" for name in ("zoneinfo64", "metaZones", "timezoneTypes",
                                                               "windowsZones")])


def monet_files():
    """Each overlay's file of framework/monet-overlays.txt and its idmap, by idmap2's name for it (ADR-0043 f))."""
    listed = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "framework", "monet-overlays.txt")
    files = [line.split()[2] for line in open(listed, encoding="utf-8") if line.startswith("overlay ")]
    return [name for file in files for name in (file, f"data@resource-cache@{file}@idmap")]


REQUIRED = (
    ["bin/activity-host.dylib", "bin/app_process64.dylib"]
    + [f"lib64/{name}.dylib" for name in BIONIC + FRONT_LIBS + ["libandroid_runtime.so", "libsqlite.so"] + HWUI
       + ART_COPIES]
    + ["system/framework/framework.jar", "system/framework/framework-res/AndroidManifest.xml",
       "system/framework/framework-res/resources.arsc", "system/etc/permissions/platform.xml",
       "system/etc/public.libraries.txt", "system/etc/export-table.txt", "system/usr/share/zoneinfo/tzdata"]
    + [f"apex/com.android.art/javalib/{jar}" for jar in ART_JARS]
    + ["apex/com.android.conscrypt/javalib/conscrypt.jar", "apex/com.android.i18n/etc/icu/icudt75l.dat",
       "apex-lib64/com.android.art/libart.so.dylib", "apex-lib64/com.android.art/libandroidicu.so.dylib",
       "apex-lib64/com.android.conscrypt/libjavacrypto.so.dylib"]
    # The tzdata module as Android 15's (the compatibility plan's gap 21).
    + [f"apex/com.android.tzdata/etc/tz/{name}" for name in TZDATA_MODULE]
    + [f"resource-cache/{name}" for name in monet_files()]
)

# The CA certificates that Guest/apex/com.android.conscrypt/cacerts must hold (ADR-0046 decision 4).
CERTIFICATES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "sources", "ca-certificates",
                            "files")

# The folders of Guest/ that the guest's file system mounts, as /system, /apex and /data/resource-cache.
MOUNTED = ["system", "apex", "resource-cache"]
LINK_LIMIT = 32  # Darwin's MAXSYMLINKS: a longer chain answers ELOOP


class Leaves(Exception):
    """A link's walk that leaves its mounted folder, or never ends; the text says why."""


def walk(root, below, text, links):
    """Where `text` leads from the folder `below` (its components under `root`), as Darwin's kernel walks a path
    under a descriptor with O_RESOLVE_BENEATH: step by step, so an absolute text and a ".." above `root` leave it
    even when the text would come back, and a link on the way is followed by its own text from its folder, whose ".."
    is that folder's parent. Returns the components it ends at and the links followed, or raises Leaves."""
    if text.startswith("/"):
        raise Leaves("an absolute text leaves the folder")
    below = list(below)
    for name in text.split("/"):
        if name in ("", "."):
            continue
        if name == "..":
            if not below:
                raise Leaves('a ".." leaves the folder')
            below.pop()
        elif os.path.islink(os.path.join(root, *below, name)):
            if links == LINK_LIMIT:
                raise Leaves(f"more than {LINK_LIMIT} links on the way")
            below, links = walk(root, below, os.readlink(os.path.join(root, *below, name)), links + 1)
        else:
            below.append(name)
    return below, links


def certificate_problems(guest):
    """Conscrypt's CA certificates: the files of CERTIFICATES in Guest/apex/com.android.conscrypt/cacerts, the same
    names and bytes, and Guest/system/etc/security/cacerts an empty folder, as the runtime links that name to them."""
    if not os.path.isdir(CERTIFICATES):
        return [f"missing: {os.path.normpath(CERTIFICATES)} (tools/fetch-sources.py --set art)"]
    problems = []
    folder = os.path.join(guest, "apex", "com.android.conscrypt", "cacerts")
    wanted = sorted(os.listdir(CERTIFICATES))
    there = sorted(os.listdir(folder)) if os.path.isdir(folder) else []
    missing = [name for name in wanted if name not in there]
    extra = [name for name in there if name not in wanted]
    changed = [name for name in wanted if name in there
               and not filecmp.cmp(os.path.join(CERTIFICATES, name), os.path.join(folder, name), shallow=False)]
    for names, what in ((missing, "missing"), (extra, "not of build/sources/ca-certificates"), (changed, "changed")):
        if names:
            problems.append(f"certificates: {len(names)} {what} in Guest/apex/com.android.conscrypt/cacerts, "
                            f"{names[0]} first")
    empty = os.path.join(guest, "system", "etc", "security", "cacerts")
    if not os.path.isdir(empty) or os.path.islink(empty) or os.listdir(empty):
        problems.append("certificates: Guest/system/etc/security/cacerts is not an empty folder")
    return problems


def export_table_problems(guest):
    """The export table (ADR-0055 decision 3, tools/export-table.py): its format's first line, and a block for each
    library container of lib64/ and apex-lib64/ and for nothing else, as each library's soname is its file name."""
    path = os.path.join(guest, "system", "etc", "export-table.txt")
    if not os.path.isfile(path):
        return []  # REQUIRED names it
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().splitlines()
    if not lines or lines[0] != "# minato export table 1":
        return ["export table: Guest/system/etc/export-table.txt does not start with \"# minato export table 1\""]
    blocks = {line[2:].split(" ")[0] for line in lines if line.startswith("@ ")}  # "@ <soname>" or "@ <soname> global"
    libraries = {name[:-len(".dylib")] for top in ("lib64", "apex-lib64")
                 for _, _, names in os.walk(os.path.join(guest, top)) for name in names if name.endswith(".so.dylib")}
    problems = [f"export table: no block for {name}" for name in sorted(libraries - blocks)]
    problems += [f"export table: a block for {name}, which the guest does not have"
                 for name in sorted(blocks - libraries)]
    return problems


def link_problems(guest):
    """The links in the mounted folders of `guest` whose walk leaves their folder or never ends, each named with its
    text. The mounted folder itself may be a link: apkrt_mount takes its realpath."""
    problems = []
    for top in MOUNTED:
        root = os.path.realpath(os.path.join(guest, top))
        for folder, directories, names in os.walk(root):
            below = [] if folder == root else os.path.relpath(folder, root).split(os.sep)
            for name in sorted(directories + names):
                path = os.path.join(folder, name)
                if not os.path.islink(path):
                    continue
                try:
                    walk(root, below, os.readlink(path), 1)
                except Leaves as why:
                    shown = os.path.join(top, *below, name)
                    problems.append(f"link: Guest/{shown} -> {os.readlink(path)}: {why} (Guest/{top})")
    return problems


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    guest = os.path.join(sys.argv[1], "Guest")
    problems = []
    for path in REQUIRED:
        full = os.path.join(guest, path)
        if not os.path.isfile(full):
            problems.append(f"missing: Guest/{path}")
        elif os.path.getsize(full) == 0:
            problems.append(f"empty: Guest/{path}")
    problems += certificate_problems(guest)
    problems += export_table_problems(guest)
    containers = sorted(os.path.join(folder, name) for folder, _, names in os.walk(guest) for name in names
                        if name.endswith(".dylib"))
    for container in containers:
        result = subprocess.run(["codesign", "-v", container], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True)
        if result.returncode != 0:
            problems.append(f"codesign -v fails for Guest/{os.path.relpath(container, guest)}: {result.stdout.strip()}")
    problems += link_problems(guest)
    for problem in problems:
        print(problem)
    files = sum(len(names) for _, _, names in os.walk(guest))
    if problems:
        print(f"guest check: {len(problems)} problems in {files} files")
        sys.exit(1)
    print(f"guest check: {len(REQUIRED)} paths of the starts there, {len(containers)} containers signed, {files} files, "
          f"no link that leaves {' or '.join(MOUNTED)}")


if __name__ == "__main__":
    main()
