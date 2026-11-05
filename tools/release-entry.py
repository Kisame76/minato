#!/usr/bin/env python3
"""Writes the source entry (source-entry.json) of a release into <out>: one entry of the website's data/releases.json.
AltStore refuses an install whose version, build number, entitlements or privacy texts differ from the IPA's, so they
are read from the built app: the Info.plist's minimum OS version and every NS*UsageDescription key (whatever texts the
build has, none is listed by hand), and <out>/entitlements.plist; the size and the SHA-256 are those of
<out>/Minato.ipa. tools/release.sh calls it after the IPA is made; tools/tests/release_entry_test.py tests it.

Usage: tools/release-entry.py <out> <app> <version> <build> <download URL> <date> <notes>"""
import json
import os
import plistlib
import sys

out, app, version, build, url, today, notes = sys.argv[1:]
with open(os.path.join(app, "Info.plist"), "rb") as f:
    info = plistlib.load(f)
with open(os.path.join(out, "entitlements.plist"), "rb") as f:
    entitlements = plistlib.load(f)
ipa = os.path.join(out, "Minato.ipa")
# One entry of the website's data/releases.json: AltStore refuses an install whose version, build number,
# entitlements or privacy texts differ from the IPA's, so they are read from the built app.
entry = {
    "version": version,
    "buildVersion": build,
    "date": today,
    "downloadURL": url,
    "size": os.path.getsize(ipa),
    "sha256": open(ipa + ".sha256").read().split()[0],
    "minOSVersion": info["MinimumOSVersion"],
    "entitlements": sorted(entitlements),
    "privacy": {key: value for key, value in sorted(info.items()) if key.endswith("UsageDescription")},
    "notes": {"en": notes},
}
with open(os.path.join(out, "source-entry.json"), "w") as f:
    json.dump(entry, f, indent=2)
    f.write("\n")
