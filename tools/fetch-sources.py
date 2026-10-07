#!/usr/bin/env python3
"""Fetches the sources listed in third_party/sources.json: the AOSP sources of the guest libc (the list "sources",
ADR-0021) or, with --set art, those of ART and its libraries and the tools that build them (the list "art",
ADR-0033 a).

Each source is an archive of one commit, or single files and folders of it, from
android.googlesource.com, extracted to build/sources/<name>/. A source with a "url" is an archive from elsewhere,
checked against its "sha256" before anything of it is extracted into its "directory" below the repository. An
archive of one commit that its host makes on request, as GitHub's are (MoltenVK's sources, ADR-0044 b), names
its "commit" instead of a "sha256", all 40 hex digits, which its "url" names too, and only its content is pinned, as
for android.googlesource.com.
The content is checked against the pinned digest in the list:
SHA-256 over the sorted lines "<path> NUL <SHA-256 of the file>", which does not depend on how the
server packs the archive. A source whose digest matches is not fetched again.

Usage: tools/fetch-sources.py [--set <list>] [--pin] [name...]
  --set   the list to fetch: sources (the default) or art
  --pin   write the digests of what was fetched into the list (only for a new or changed entry)
"""
import base64
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "third_party", "sources.json")
DESTINATION = os.path.join(ROOT, "build", "sources")


# The host's answer and the wait between tries; the test puts its own in their place.
urlopen = urllib.request.urlopen
sleep = time.sleep
TRIES = 5
LONGEST_WAIT = 120


def fetch(url):
    """The body at url. A 429 (Too Many Requests) is asked again after its Retry-After, at most 120 s, or after
    5, 10, 20 and 40 s without one; after 5 tries it gives up. Any other error ends the run as before."""
    for attempt in range(TRIES):
        try:
            with urlopen(url, timeout=120) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code != 429:
                raise
            if attempt == TRIES - 1:
                raise SystemExit(f"{url}: the host answered 429 (Too Many Requests) {TRIES} tries in a row; "
                                 "run again later")
            retry_after = (error.headers or {}).get("Retry-After", "")
            wait = int(retry_after) if retry_after.isdigit() else 5 * 2 ** attempt
            sleep(min(wait, LONGEST_WAIT))


def digest(directory):
    lines = []
    for folder, _, names in os.walk(directory):
        for name in names:
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, directory)
            if os.path.islink(path):
                content = ("link:" + os.readlink(path)).encode()
            else:
                with open(path, "rb") as handle:
                    content = handle.read()
            lines.append(relative.encode() + b"\0" + hashlib.sha256(content).hexdigest().encode() + b"\n")
    return hashlib.sha256(b"".join(sorted(lines))).hexdigest()


def inside(path, directory):
    """Whether `path`, with every link on its way resolved, is `directory` or lies below it."""
    path, directory = os.path.realpath(path), os.path.realpath(directory)
    return path == directory or path.startswith(directory + os.sep)


def extract(archive, directory):
    """Extracts a .tar.gz, refusing absolute paths, '..', anything but files, folders and links, and a member that
    would be written through a link or in a link's place: links that each look harmless by name can lead out
    together ("b -> ." and "a -> b/.." make "a" the folder's parent, so "a/x" would land beside it), and Python 3.9's
    tarfile has no filter against that. Links that point out of the archive (AOSP has a few into sibling projects)
    are left out, and once all is extracted, so are links that leave the folder through other links. Where tarfile
    has filters, it takes 'tar', which keeps link targets as written: 3.14's default, 'data', writes "b/.." as ".",
    which would keep such a link and change the content digest."""
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        for member in tar.getmembers():
            parts = member.name.split("/")
            if member.name.startswith("/") or ".." in parts:
                raise SystemExit(f"refusing archive member {member.name!r}")
            if not (member.isfile() or member.isdir() or member.issym()):
                raise SystemExit(f"refusing archive member {member.name!r} of an unexpected kind")
            way = directory
            for part in parts:
                way = os.path.join(way, part)
                if os.path.islink(way):
                    raise SystemExit(f"refusing archive member {member.name!r}, which would be written through or "
                                     f"in place of the link {os.path.relpath(way, directory)!r}")
            if member.issym():
                resolved = os.path.normpath(os.path.join(os.path.dirname(member.name), member.linkname))
                if member.linkname.startswith("/") or resolved == ".." or resolved.startswith("../"):
                    print(f"  left out link {member.name} -> {member.linkname}")
                    continue
            tar.extract(member, directory, **({"filter": "tar"} if hasattr(tarfile, "tar_filter") else {}))
    leaving = [os.path.join(folder, name) for folder, folders, names in os.walk(directory) for name in folders + names
               if os.path.islink(os.path.join(folder, name)) and not inside(os.path.join(folder, name), directory)]
    for link in leaving:
        print(f"  left out link {os.path.relpath(link, directory)} -> {os.readlink(link)}")
        os.remove(link)


def main():
    arguments = sys.argv[1:]
    pin = "--pin" in arguments
    chosen = "sources"
    if "--set" in arguments:
        index = arguments.index("--set")
        if index + 1 == len(arguments):
            raise SystemExit("--set needs the name of a list: sources or art")
        chosen = arguments[index + 1]
        del arguments[index:index + 2]
    wanted = [argument for argument in arguments if argument != "--pin"]
    with open(MANIFEST) as handle:
        manifest = json.load(handle)
    if not isinstance(manifest.get(chosen), list):
        raise SystemExit(f"{os.path.relpath(MANIFEST, ROOT)} has no list {chosen!r}: sources or art")
    host = manifest["host"]
    changed = False
    os.makedirs(DESTINATION, exist_ok=True)

    for source in manifest[chosen]:
        name = source["name"]
        if wanted and name not in wanted:
            continue
        target = os.path.join(ROOT, source["directory"]) if "directory" in source else os.path.join(DESTINATION, name)
        if source["digest"] and os.path.isdir(target) and digest(target) == source["digest"]:
            print(f"{name}: up to date")
            continue

        os.makedirs(os.path.dirname(target), exist_ok=True)
        staging = tempfile.mkdtemp(dir=os.path.dirname(target), prefix=f".{name}-")
        try:
            downloaded = 0
            if "files" in source or "directories" in source:
                for directory in source.get("directories", []):
                    data = fetch(f"{host}/{source['project']}/+archive/{source['commit']}/{directory}.tar.gz")
                    downloaded += len(data)
                    extract(data, os.path.join(staging, directory))
                for path in source.get("files", []):
                    url = f"{host}/{source['project']}/+/{source['commit']}/{path}?format=TEXT"
                    data = fetch(url)
                    downloaded += len(data)
                    file_path = os.path.join(staging, path)
                    os.makedirs(os.path.dirname(file_path), exist_ok=True)
                    with open(file_path, "wb") as handle:
                        handle.write(base64.b64decode(data))
            elif "url" in source:
                if "sha256" not in source and "commit" not in source:
                    raise SystemExit(f"{name}: an archive needs its SHA-256, or the commit it is an archive of")
                if "sha256" not in source and not (re.fullmatch("[0-9a-f]{40}", source["commit"])
                                                   and source["commit"] in source["url"]):
                    raise SystemExit(f"{name}: an archive without a SHA-256 is of the commit its url names, by all 40 "
                                     f"hex digits, not {source['commit']!r} and {source['url']}")
                data = fetch(source["url"])
                downloaded = len(data)
                archive_sha256 = hashlib.sha256(data).hexdigest()
                if "sha256" in source and archive_sha256 != source["sha256"]:
                    raise SystemExit(f"{name}: the archive's SHA-256 {archive_sha256} does not match the pinned "
                                     f"{source['sha256']}")
                extract(data, staging)
            else:
                data = fetch(f"{host}/{source['project']}/+archive/{source['commit']}.tar.gz")
                downloaded = len(data)
                extract(data, staging)

            actual = digest(staging)
            if source["digest"] and actual != source["digest"]:
                raise SystemExit(f"{name}: digest {actual} does not match the pinned {source['digest']}")
            if not source["digest"]:
                if not pin:
                    raise SystemExit(f"{name}: no pinned digest; check the source, then run with --pin")
                source["digest"] = actual
                changed = True
            shutil.rmtree(target, ignore_errors=True)
            os.rename(staging, target)
            print(f"{name}: {downloaded / 1e6:.2f} MB fetched, digest {actual}")
        finally:
            shutil.rmtree(staging, ignore_errors=True)

    if changed:
        with open(MANIFEST, "w") as handle:
            json.dump(manifest, handle, indent=2)
            handle.write("\n")
        print(f"pinned new digests in {os.path.relpath(MANIFEST, ROOT)}")


if __name__ == "__main__":
    main()
