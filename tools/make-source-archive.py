#!/usr/bin/env python3
"""The source archive of a release: the corresponding source of the GPL-2.0 with the Classpath Exception parts of the
IPA (core-oj, libopenjdk, libopenjdkjvm; THIRD_PARTY.md, ADR-0033), which tools/release.sh uploads next to Minato.ipa,
where SideStore and AltStore take the IPA from (GPL-2.0 section 3(a)).

It holds README-SOURCE.txt, inputs/libcore and inputs/art, the trees of third_party/sources.json's pinned commits as
tools/fetch-sources.py extracts them into build/sources, and ours/, the scripts that control the build. Each input's
content digest must be the pinned one, else nothing is made. The archive is reproducible: members sorted by name,
time 0, owner and group 0 without names, modes 0644 and 0755, gzip without a time; the same inputs give the same bytes
(with the same Python and zlib; the digests are the proof). Next to it goes <archive>.sha256.

Usage: tools/make-source-archive.py VERSION RELEASE_URL OUT_DIRECTORY
"""
import gzip
import hashlib
import importlib.util
import io
import json
import os
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The inputs: sources of third_party/sources.json's list "art" that the GPL-2.0+CE parts are built from. All of libcore,
# because tools/art-plan.py's boot jars compile its Java in one run, and all of art, in whose tree libopenjdkjvm builds.
INPUTS = ["libcore", "art"]
# What controls their build (GPL-2.0 section 3: "the scripts used to control compilation and installation").
OURS = ["runtime/art", "tools/art-plan.py", "tools/fetch-sources.py", "tools/build-art.sh",
        "tools/embed-guest-system.sh", "third_party/sources.json"]
SKIPPED = {".DS_Store", "__pycache__"}


def tree_digest(directory):
    spec = importlib.util.spec_from_file_location("fetch_sources", os.path.join(ROOT, "tools", "fetch-sources.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.digest(directory)


def files_of(directory):
    """(path relative to `directory`, absolute path) of every file and link under it, in name order."""
    found = []
    for folder, folders, names in os.walk(directory):
        folders[:] = [name for name in folders if name not in SKIPPED]
        for name in names:
            if name not in SKIPPED:
                path = os.path.join(folder, name)
                found.append((os.path.relpath(path, directory), path))
    return sorted(found)


def readme(version, release_url, pins):
    lines = [
        f"Source code of the GPL-2.0 with the Classpath Exception parts of Minato {version}",
        "",
        "Minato's app carries three components under the GNU General Public License, version 2, with the Classpath",
        "Exception (the license text is in the app's Licenses folder and in inputs/libcore/LICENSE):",
        "",
        "  core-oj.jar    Java classes of libcore's ojluni (OpenJDK's), in Guest/apex/com.android.art/javalib",
        "  libopenjdk     their native code, in Guest/apex-lib64",
        "  libopenjdkjvm  openjdkjvm of ART, in Guest/apex-lib64",
        "",
        "This archive is their complete corresponding source, as GPL-2.0 section 3(a) asks: it sits on the release",
        "page of the IPA that holds them,",
        "",
        f"  {release_url}",
        "",
        "inputs/    the sources the components are built from, the AOSP trees at android-15.0.0_r1 pinned by commit:",
    ]
    for name in INPUTS:
        lines.append(f"             inputs/{name}  platform/{name} at {pins[name]}")
    lines += [
        "             They are exactly what tools/fetch-sources.py --set art extracts, whose content digests the",
        "             archive's maker checked against ours/third_party/sources.json. libcore's Java is all compiled in",
        "             one run (ours/runtime/art/boot_jars.py): the classes of ojluni go into core-oj.jar.",
        "ours/      the scripts that control the build: ART's patches and overlay (none of them touches ojluni,",
        "           libopenjdk or openjdkjvm; Apache-2.0 parts of ART only), the build plan, the fetch tool and the",
        "           scripts that build ART and embed it into the app (docs/BUILDING.md of the project, the same tag).",
        "",
        "To rebuild: fetch the inputs again with ours/tools/fetch-sources.py (or use inputs/), then build the guest",
        "system with the project's tools/build-bionic.sh, ours/tools/build-art.sh and",
        "ours/tools/embed-guest-system.sh.",
        "The rest of Minato is Apache-2.0 and public in the project's repository at the tag of this release.",
        "",
    ]
    return "\n".join(lines)


def add(tar, name, data=None, path=None, link=None, directory=False):
    member = tarfile.TarInfo(name)
    member.mtime = 0
    member.uid = member.gid = 0
    member.uname = member.gname = ""
    if directory:
        member.type = tarfile.DIRTYPE
        member.mode = 0o755
        tar.addfile(member)
        return
    if link is not None:
        member.type = tarfile.SYMTYPE
        member.linkname = link
        member.mode = 0o777
        tar.addfile(member)
        return
    if data is None:
        with open(path, "rb") as handle:
            data = handle.read()
        member.mode = 0o755 if os.stat(path).st_mode & 0o111 else 0o644
    else:
        member.mode = 0o644
    member.size = len(data)
    tar.addfile(member, io.BytesIO(data))


def make(root, sources, version, release_url, names, ours, out):
    """Makes the archive and its .sha256 in `out`; returns the archive's path. SystemExit on a problem."""
    with open(os.path.join(root, "third_party", "sources.json")) as handle:
        pinned = {entry["name"]: entry for entry in json.load(handle)["art"]}
    pins = {}
    for name in names:
        tree = os.path.join(sources, name)
        if name not in pinned or not os.path.isdir(tree):
            sys.exit(f"stopped: {tree} is not fetched (tools/fetch-sources.py --set art)")
        if tree_digest(tree) != pinned[name]["digest"]:
            sys.exit(f"stopped: {tree} is not the pinned {name}: its content digest differs from "
                     "third_party/sources.json (tools/fetch-sources.py --set art)")
        pins[name] = pinned[name]["commit"]
    members = [("README-SOURCE.txt", readme(version, release_url, pins).encode(), None)]
    for name in names:
        tree = os.path.join(sources, name)
        members += [(f"inputs/{name}/{relative}", None, path) for relative, path in files_of(tree)]
    for item in ours:
        path = os.path.join(root, item)
        if os.path.isdir(path):
            members += [(f"ours/{item}/{relative}", None, found) for relative, found in files_of(path)]
        elif os.path.isfile(path):
            members.append((f"ours/{item}", None, path))
        else:
            sys.exit(f"stopped: {path} is missing")
    top = f"Minato-{version}-source"
    os.makedirs(out, exist_ok=True)
    target = os.path.join(out, f"{top}.tar.gz")
    partial = target + ".partial"
    with open(partial, "wb") as raw:
        zipped = gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0)
        with zipped, tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as tar:
            add(tar, top, directory=True)
            for name, data, path in sorted(members, key=lambda member: member[0]):
                if path is not None and os.path.islink(path):
                    add(tar, f"{top}/{name}", link=os.readlink(path))
                else:
                    add(tar, f"{top}/{name}", data=data, path=path)
    os.replace(partial, target)
    with open(target, "rb") as handle:
        checksum = hashlib.sha256(handle.read()).hexdigest()
    with open(target + ".sha256", "w") as handle:
        handle.write(f"{checksum}  {os.path.basename(target)}\n")
    return target


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    version, release_url, out = sys.argv[1:]
    made = make(ROOT, os.path.join(ROOT, "build", "sources"), version, release_url, INPUTS, OURS, out)
    print(made)
    return 0


if __name__ == "__main__":
    sys.exit(main())
