#!/usr/bin/env python3
"""tools/release-entry.py, which writes the website's source entry (source-entry.json) of a release: the version, the
build number, the entitlements and the privacy texts are read from the BUILT app, because AltStore refuses an IPA that
differs from them. On a small app it writes: every NS*UsageDescription key of the Info.plist is in "privacy" with its
text (the microphone, camera, location, photo library and so on), and no other key; the entitlements are the sorted keys
of entitlements.plist; the size and the SHA-256 are the IPA's. Python 3.9."""
import json
import os
import plistlib
import subprocess
import sys
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "release-entry.py")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        print("FAIL", what)
        failures += 1


with tempfile.TemporaryDirectory() as out:
    app = os.path.join(out, "Minato.app")
    os.makedirs(app)
    texts = {
        "NSMicrophoneUsageDescription": "Apps you run in Minato can use the microphone when they ask for it.",
        "NSCameraUsageDescription": "Apps you run in Minato can use the camera when they ask for it.",
        "NSLocationWhenInUseUsageDescription": "Apps you run in Minato can use your location when they ask for it.",
        "NSPhotoLibraryAddUsageDescription": "Lets apps you run in Minato save photos and videos to your library.",
    }
    info = dict(texts)
    info.update({"MinimumOSVersion": "26.0", "CFBundleName": "Minato", "NSAppTransportSecurity": {"x": 1}})
    with open(os.path.join(app, "Info.plist"), "wb") as f:
        plistlib.dump(info, f)
    with open(os.path.join(out, "entitlements.plist"), "wb") as f:
        plistlib.dump({"com.apple.developer.kernel.increased-memory-limit": True, "a.second": True}, f)
    with open(os.path.join(out, "Minato.ipa"), "wb") as f:
        f.write(b"not a real ipa")
    with open(os.path.join(out, "Minato.ipa.sha256"), "w") as f:
        f.write("abc123  Minato.ipa\n")
    arguments = [out, app, "1.0.0", "2", "https://example.org/Minato.ipa", "2026-11-05", "The notes."]
    result = subprocess.run([sys.executable, TOOL] + arguments, capture_output=True, text=True)
    check(result.returncode == 0, "it runs: " + result.stderr)
    entry = json.load(open(os.path.join(out, "source-entry.json"))) if result.returncode == 0 else {}
    check(entry.get("privacy") == dict(sorted(texts.items())), "the privacy texts are those of the Info.plist: %r" %
          entry.get("privacy"))
    check(entry.get("entitlements") == ["a.second", "com.apple.developer.kernel.increased-memory-limit"],
          "the entitlements are sorted: %r" % entry.get("entitlements"))
    check(entry.get("size") == len(b"not a real ipa") and entry.get("sha256") == "abc123", "size and sha256")
    check(entry.get("version") == "1.0.0" and entry.get("buildVersion") == "2" and entry.get("minOSVersion") == "26.0",
          "version, build, minimum OS")
    check(entry.get("notes") == {"en": "The notes."} and entry.get("downloadURL") == "https://example.org/Minato.ipa",
          "notes and URL")

if failures:
    sys.exit(1)
print("release_entry_test: fine")
