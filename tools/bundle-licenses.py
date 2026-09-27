#!/usr/bin/env python3
"""Xcode build phase (project.yml): puts the license texts of what the app embeds into Minato.app/Licenses.

Reads third_party/licenses.json. A component goes in when its section of THIRD_PARTY.md is embedded
always, or with a framework the build has put into the bundle's Frameworks/. Its texts are copied from
where the build fetched them (build/sources, build/moltenvk, ANGLE's checkout in build/angle, the NDK,
the repository) into Licenses/<component>/, and Licenses/licenses.json lists what is there. A missing
text fails the build: the notices must travel with every binary. tools/license-check.py checks the result.
"""
import json
import os
import shutil
import sys


def main():
    root = os.environ["SRCROOT"]
    bundle = os.path.join(os.environ["TARGET_BUILD_DIR"], os.environ["UNLOCALIZED_RESOURCES_FOLDER_PATH"])
    frameworks = os.path.join(os.environ["TARGET_BUILD_DIR"], os.environ["FRAMEWORKS_FOLDER_PATH"])
    ndk = os.environ.get("ANDROID_NDK") or os.path.expanduser("~/Library/Android/sdk/ndk/30.0.16248370")
    roots = {"sources": os.path.join(root, "build", "sources"), "moltenvk": os.path.join(root, "build", "moltenvk"),
             "angle": os.path.join(root, "build", "angle", "angle"), "repo": root, "ndk": ndk}
    with open(os.path.join(root, "third_party", "licenses.json")) as handle:
        config = json.load(handle)

    out = os.path.join(bundle, "Licenses")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    index = []
    missing = []
    for component in config["components"]:
        rule = config["sections"][component["section"]]
        if rule == "never" or (rule != "always" and not os.path.isdir(os.path.join(frameworks, rule))):
            continue
        entry = {key: component[key] for key in ("name", "section", "license", "rows")}
        if "none" in component:
            entry["none"] = component["none"]
        entry["files"] = []
        for text in component.get("texts", []):
            base, _, path = text["from"].partition(":")
            source = os.path.join(roots[base], path)
            if not os.path.isfile(source):
                missing.append(source)
                continue
            with open(source, "rb") as handle:
                content = handle.read()
            if "lines" in text:
                first, last = text["lines"]
                content = b"".join(content.splitlines(keepends=True)[first - 1:last])
            os.makedirs(os.path.join(out, component["name"]), exist_ok=True)
            with open(os.path.join(out, component["name"], text["as"]), "wb") as handle:
                handle.write(content)
            entry["files"].append(text["as"])
        index.append(entry)
    for source in missing:
        print(f"error: license text {source} is missing; tools/fetch-sources.py, tools/fetch-moltenvk.sh, "
              "tools/build-angle.sh --fetch and the NDK of tools/build-bionic.sh provide the texts", file=sys.stderr)
    if missing:
        return 1
    with open(os.path.join(out, "licenses.json"), "w") as handle:
        json.dump({"components": index}, handle, indent=2)
        handle.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
