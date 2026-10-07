#!/usr/bin/env python3
"""Builds MoltenVK 1.4.2 from its pinned sources for Minato, with Apple's private Metal API compiled in and off by
default (ADR-0044 b), decision 2), in place of the release of tools/fetch-moltenvk.sh (ADR-0030).

Usage: tools/build-moltenvk.sh [--keep]      build, without the network
       tools/build-moltenvk.sh --self-test   check the script's guards

The sources are the list "moltenvk-src" of third_party/sources.json, which tools/fetch-sources.py --set moltenvk-src
fetches into build/sources/moltenvk/; each must still match its pinned digest. The script copies them into a work
tree, build/moltenvk-build/, and lays out MoltenVK's External/ as its fetchDependencies would, without its clones:
cereal, Vulkan-Headers, SPIRV-Cross, and SPIRV-Tools with SPIRV-Headers as its external/spirv-headers and the
generated files of Templates/spirv-tools/build.zip; Vulkan-Tools and Volk serve only MoltenVK's demos. The work tree
has no history, so its Scripts/gen_moltenvk_rev_hdr.sh records the pinned commit instead of asking git. Then it
builds as `fetchDependencies --ios --macos` and
`make ios macos MVK_USE_METAL_PRIVATE_API=1 MVK_CONFIG_USE_METAL_PRIVATE_API=0` do, with Xcode's DerivedData in the
work tree and the Mac's parts for arm64 only, as CMake builds them:
- the schemes ExternalDependencies-iOS and -macOS, then their XCFrameworks (Scripts/create_ext_lib_xcframeworks.sh);
- the schemes "MoltenVK Package (iOS only)" and "(macOS only)", with both settings as preprocessor definitions, as
  the Makefile passes them.
The outputs go where tools/fetch-moltenvk.sh puts the release's: the iOS device slice of the dynamic framework as
build/MoltenVK.xcframework, the macOS dylib as build/moltenvk/macos/libMoltenVK.dylib, the headers as
build/moltenvk/include, and vk.xml from the Vulkan-Headers sources, checked against the Git blob that the entry
"vulkan-headers" pins, as build/moltenvk/vk.xml. MoltenVK's license text goes to build/moltenvk/LICENSE for the app
bundle (third_party/licenses.json), and build/moltenvk/stamp.json records the build. The work tree goes after a
build that succeeded, unless --keep. The script writes only below build/moltenvk-build/ and build/moltenvk/ and to
build/MoltenVK.xcframework.
"""
import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "moltenvk")
WORK = os.path.join(ROOT, "build", "moltenvk-build")
XCFRAMEWORK = os.path.join(ROOT, "build", "MoltenVK.xcframework")
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
DEFINES = ("MVK_USE_METAL_PRIVATE_API=1", "MVK_CONFIG_USE_METAL_PRIVATE_API=0")
# The schemes' OS and the destinations' platform, in the order fetchDependencies and the Makefile build them.
PLATFORMS = (("iOS", "iOS"), ("macOS", "macOS"))
# External/'s folders by the list's names, as fetchDependencies clones them.
EXTERNAL = (("cereal", "cereal"), ("vulkan-headers", "Vulkan-Headers"), ("spirv-cross", "SPIRV-Cross"),
            ("spirv-tools", "SPIRV-Tools"))
REVISION_LINE = "MVK_GIT_REV=$(git rev-parse HEAD)"


def load(name, file_name):
    """A tool of tools/ as a module: fetch-moltenvk.py's checks of a binary, fetch-sources.py's digest."""
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", file_name))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fetch_moltenvk = load("fetch_moltenvk", "fetch-moltenvk.py")
fetch_sources = load("fetch_sources", "fetch-sources.py")


def inside(path, base):
    path, base = os.path.realpath(path), os.path.realpath(base)
    return path == base or path.startswith(base + os.sep)


def require_inside(path):
    if not any(inside(path, base) for base in (WORK, BUILD, XCFRAMEWORK)):
        raise SystemExit(f"build-moltenvk: refusing to write {path}, outside build/moltenvk-build/, build/moltenvk/ "
                         "and build/MoltenVK.xcframework")


def remove(path):
    require_inside(path)
    if os.path.islink(path) or os.path.isfile(path):
        os.remove(path)
    elif os.path.isdir(path):
        shutil.rmtree(path)


def run(command, cwd=None, env=None, log=None):
    if log is None:
        result = subprocess.run(command, cwd=cwd, env=env)
    else:
        with open(log, "a") as handle:
            handle.write("\n$ " + " ".join(command) + "\n")
            handle.flush()
            result = subprocess.run(command, cwd=cwd, env=env, stdout=handle, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        where = f", see {log}" if log else ""
        raise SystemExit(f"build-moltenvk: {' '.join(command)} failed with {result.returncode}{where}")


def output(command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout.strip()


def sources():
    """The fetched sources by the list's names: the one folder of each archive (GitHub's <repo>-<commit>) and its
    commit, each checked against the list's digest; and the manifest."""
    with open(MANIFEST) as handle:
        manifest = json.load(handle)
    found = {}
    for entry in manifest["moltenvk-src"]:
        directory = os.path.join(ROOT, entry["directory"])
        if not entry["digest"] or not os.path.isdir(directory) or fetch_sources.digest(directory) != entry["digest"]:
            raise SystemExit(f"build-moltenvk: {entry['directory']} is missing or not the pinned {entry['name']}; "
                             "run tools/fetch-sources.py --set moltenvk-src")
        inner = [os.path.join(directory, child) for child in os.listdir(directory)]
        if len(inner) != 1 or not os.path.isdir(inner[0]):
            raise SystemExit(f"build-moltenvk: {entry['directory']} holds more than its archive's one folder")
        found[entry["name"]] = (inner[0], entry["commit"])
    return found, manifest


def clone(source, target):
    """A copy of the folder `source` as `target`, as APFS clones."""
    require_inside(target)
    os.makedirs(target)
    run(["cp", "-c", "-R", source + "/.", target])


def record_revision(tree, commit):
    """Scripts/gen_moltenvk_rev_hdr.sh with the pinned commit in place of git's answer, which a tree without history
    cannot give (MoltenVK's version string carries it)."""
    script = os.path.join(tree, "Scripts", "gen_moltenvk_rev_hdr.sh")
    require_inside(script)
    with open(script) as handle:
        text = handle.read()
    if text.count(REVISION_LINE) != 1:
        raise SystemExit(f"build-moltenvk: {script} no longer has the line {REVISION_LINE!r} of MoltenVK 1.4.2")
    with open(script, "w") as handle:
        handle.write(text.replace(REVISION_LINE, f"MVK_GIT_REV={commit}"))


def lay_out(found):
    """The work tree: MoltenVK with its External/ as fetchDependencies leaves it."""
    remove(WORK)
    tree = os.path.join(WORK, "MoltenVK")
    clone(found["moltenvk"][0], tree)
    external = os.path.join(tree, "External")
    for name, folder in EXTERNAL:
        clone(found[name][0], os.path.join(external, folder))
    tools = os.path.join(external, "SPIRV-Tools")
    clone(found["spirv-headers"][0], os.path.join(tools, "external", "spirv-headers"))
    run(["unzip", "-o", "-q", "-d", tools, os.path.join(tree, "Templates", "spirv-tools", "build.zip")])
    remove(os.path.join(tools, "__MACOSX"))
    record_revision(tree, found["moltenvk"][1])
    return tree


def mac_architectures(platform):
    return ["ARCHS=arm64"] if platform == "macOS" else []


def build(tree):
    """fetchDependencies' builds and packaging, then the Makefile's; the seconds of each part."""
    log = os.path.join(WORK, "xcodebuild.log")
    seconds = {}
    start = time.monotonic()
    quiet = dict(os.environ, SKIP_PACKAGING="Y")
    for name, platform in PLATFORMS:
        run(["xcodebuild", "-project", "ExternalDependencies.xcodeproj", "-scheme", f"ExternalDependencies-{name}",
             "-destination", f"generic/platform={platform}", "-configuration", "Release",
             "-derivedDataPath", os.path.join("External", "build", "Intermediates", platform), "-quiet", "build"]
            + mac_architectures(platform), cwd=tree, env=quiet, log=log)
    # As fetchDependencies after its builds: the XCFrameworks, Latest, and no Intermediates. Its `make clean` is left
    # out: nothing of MoltenVK was built in this tree yet.
    packaging = dict(os.environ, PROJECT_DIR=".", CONFIGURATION="Release", SKIP_PACKAGING="")
    run(["bash", "-c", ". ./Scripts/create_ext_lib_xcframeworks.sh"], cwd=tree, env=packaging, log=log)
    latest = os.path.join(tree, "External", "build", "Latest")
    remove(latest)
    os.symlink("Release", latest)
    remove(os.path.join(tree, "External", "build", "Intermediates"))
    seconds["dependencies"] = round(time.monotonic() - start, 1)

    definitions = "${inherited} " + " ".join(DEFINES)
    for name, platform in PLATFORMS:
        start = time.monotonic()
        run(["xcodebuild", "build", "-project", "MoltenVKPackaging.xcodeproj",
             "-scheme", f"MoltenVK Package ({name} only)", "-destination", f"generic/platform={platform}",
             "-derivedDataPath", os.path.join(WORK, "DerivedData"), "GCC_PREPROCESSOR_DEFINITIONS=" + definitions,
             "-quiet"] + mac_architectures(platform), cwd=tree, log=log)
        seconds["moltenvk_" + name.lower()] = round(time.monotonic() - start, 1)
    return seconds


def vk_xml(found, manifest):
    """vk.xml of the Vulkan-Headers sources, checked against the Git blob that the entry "vulkan-headers" pins."""
    pinned = manifest["vulkan-headers"]
    if pinned["commit"] != found["vulkan-headers"][1]:
        raise SystemExit("build-moltenvk: the Vulkan-Headers sources are not the commit of the entry vulkan-headers")
    item = next(item for item in pinned["files"] if item["path"] == "vk.xml")
    with open(os.path.join(found["vulkan-headers"][0], "registry", "vk.xml"), "rb") as handle:
        content = handle.read()
    fetch_moltenvk.check_blob(content, item["git_blob"], "vk.xml")
    target = os.path.join(BUILD, "vk.xml")
    require_inside(target)
    with open(target, "wb") as handle:
        handle.write(content)


def install(tree, found, manifest):
    """The outputs in the release's places; the stamp's facts about them."""
    package = os.path.join(tree, "Package", "Release", "MoltenVK")
    slice_framework = os.path.join(package, "dynamic", "MoltenVK.xcframework", "ios-arm64", "MoltenVK.framework")
    if not os.path.isdir(slice_framework):
        raise SystemExit(f"build-moltenvk: no iOS device slice in {os.path.dirname(os.path.dirname(slice_framework))}")
    os.makedirs(BUILD, exist_ok=True)
    remove(XCFRAMEWORK)
    run(["xcodebuild", "-quiet", "-create-xcframework", "-framework", slice_framework, "-output", XCFRAMEWORK])
    dylib = os.path.join(BUILD, "macos", "libMoltenVK.dylib")
    fetch_moltenvk.copy(os.path.join(package, "dynamic", "dylib", "macOS", "libMoltenVK.dylib"), dylib)
    include = os.path.join(BUILD, "include")
    remove(include)
    shutil.copytree(os.path.join(package, "include"), include, symlinks=False)
    vk_xml(found, manifest)
    fetch_moltenvk.copy(os.path.join(found["moltenvk"][0], "LICENSE"), os.path.join(BUILD, "LICENSE"))

    slices = sorted(name for name in os.listdir(XCFRAMEWORK) if os.path.isdir(os.path.join(XCFRAMEWORK, name)))
    framework = {}
    for name in slices:
        directory = os.path.join(XCFRAMEWORK, name, "MoltenVK.framework")
        framework[name] = fetch_moltenvk.check_binary(os.path.join(directory, "MoltenVK"), ios=True)
        framework[name]["installed"] = 1024 * fetch_moltenvk.kilobytes(directory)
        framework[name]["zipped"] = fetch_moltenvk.zipped_bytes(directory)
    return {"xcframework_slices": slices, "framework": framework,
            "macos_dylib": fetch_moltenvk.check_binary(dylib, ios=False)}


def licenses(found):
    """The license texts of the sources, relative to build/sources/moltenvk/."""
    top = os.path.join(ROOT, "build", "sources", "moltenvk")
    texts = []
    for name in sorted(found):
        for entry in sorted(os.listdir(found[name][0])):
            if entry.upper().startswith(("LICENSE", "COPYING")):
                texts.append(os.path.relpath(os.path.join(found[name][0], entry), top))
    return texts


def main_build(keep):
    found, manifest = sources()
    start = time.monotonic()
    tree = lay_out(found)
    seconds = build(tree)
    facts = install(tree, found, manifest)
    pinned = manifest["moltenvk"]
    if pinned["commit"] != found["moltenvk"][1]:
        raise SystemExit("build-moltenvk: the MoltenVK sources are not the commit of the entry moltenvk")
    stamp = dict(facts, **{
        "moltenvk": {"tag": pinned["tag"], "commit": pinned["commit"]},
        "vulkan_headers": {"tag": manifest["vulkan-headers"]["tag"], "commit": manifest["vulkan-headers"]["commit"]},
        "built": {"from": {name: commit for name, (_, commit) in sorted(found.items())},
                  "preprocessor_definitions": list(DEFINES), "xcode": output(["xcodebuild", "-version"]).split("\n"),
                  "seconds": dict(seconds, total=round(time.monotonic() - start, 1)),
                  "work_tree_kb": fetch_moltenvk.kilobytes(WORK)},
        "licenses": licenses(found),
        "finished": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    })
    if not keep:
        remove(WORK)
    stamp["disk_kb"] = fetch_moltenvk.kilobytes(BUILD)
    with open(os.path.join(BUILD, "stamp.json"), "w") as handle:
        json.dump(stamp, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(json.dumps(stamp, indent=2, sort_keys=True))


def expect(condition, what):
    if not condition:
        raise SystemExit("build-moltenvk: self-test failed: " + what)
    print("self-test: " + what, flush=True)


def refused(action):
    try:
        action()
    except SystemExit:
        return True
    return False


def self_test():
    """The guards, without a build: writes stay inside the three places, and a revision script that is not 1.4.2's
    is refused."""
    expect(refused(lambda: require_inside(os.path.join(ROOT, "build", "sources", "moltenvk"))),
           "a write to the fetched sources is refused")
    expect(refused(lambda: require_inside(os.path.join(ROOT, "build", "moltenvk-build-other"))),
           "a write beside the work tree is refused")
    expect(refused(lambda: require_inside(ROOT)) and refused(lambda: require_inside(MANIFEST)),
           "a write to the repository is refused")
    expect(not refused(lambda: require_inside(os.path.join(WORK, "MoltenVK"))) and
           not refused(lambda: require_inside(os.path.join(BUILD, "include"))) and
           not refused(lambda: require_inside(os.path.join(XCFRAMEWORK, "Info.plist"))),
           "a write below the work tree, build/moltenvk/ and the XCFramework is taken")
    tree = os.path.join(WORK, "self-test")
    remove(tree)
    os.makedirs(os.path.join(tree, "Scripts"))
    try:
        script = os.path.join(tree, "Scripts", "gen_moltenvk_rev_hdr.sh")
        with open(script, "w") as handle:
            handle.write("MVK_GIT_REV=$(git describe)\n")
        expect(refused(lambda: record_revision(tree, "0" * 40)), "a revision script that is not 1.4.2's is refused")
        with open(script, "w") as handle:
            handle.write("#!/bin/bash\n" + REVISION_LINE + "\necho \"$MVK_GIT_REV\"\n")
        record_revision(tree, "1" * 40)
        with open(script) as handle:
            expect(handle.read() == "#!/bin/bash\nMVK_GIT_REV=" + "1" * 40 + "\necho \"$MVK_GIT_REV\"\n",
                   "1.4.2's revision script records the pinned commit")
    finally:
        remove(tree)
    print("self-test: passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--keep", action="store_true", help="keep the work tree build/moltenvk-build/")
    parser.add_argument("--self-test", action="store_true", help="check the script's guards")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
    else:
        main_build(arguments.keep)


if __name__ == "__main__":
    sys.exit(main())
