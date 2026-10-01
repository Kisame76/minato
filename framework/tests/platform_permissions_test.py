#!/usr/bin/env python3
"""ADR-0045 decision 6's protection-level table: our framework-res's binary manifest (tools/build-framework-res.sh)
declares the platform's permissions with the protection levels Android 15 gives them.

The oracle for b3's Java reader, not shipped: aapt2 dumps the manifest's tree, and each <permission> under
<manifest> gives its name and android:protectionLevel (normal when absent). A permission behind an
android:featureFlag is skipped as Android 15's parser skips it (AconfigFlags.skipCurrentElement): when the flag is
off, or on for "!flag", or unknown; the values are those framework-res was built with
(build/aconfig/feature-flags.txt, tools/aconfig-flags.py). The level goes through
PermissionInfo.fixProtectionLevel, as Android's parser applies it, and is written as
PermissionInfo.protectionToString writes it. The result must equal fixtures/expected/platform-permissions.txt:
the reference AVD's permissions of the package "android" from `pm list permissions -f`, one "<name> <level>" per
line, sorted ("#" lines are comments).

Prints "skipped: ..." while aapt2, framework-res or the flag values are not built.
Usage: platform_permissions_test.py [<aapt2> <framework-res.apk> <feature-flags.txt> <expected>]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ANDROID = "http://schemas.android.com/apk/res/android:"

# PermissionInfo at android-15.0.0_r1.
BASES = {0: "normal", 1: "dangerous", 2: "signature", 3: "signatureOrSystem", 4: "internal"}
FLAGS = [(0x10, "privileged"), (0x20, "development"), (0x40, "appop"), (0x80, "pre23"), (0x100, "installer"),
         (0x200, "verifier"), (0x400, "preinstalled"), (0x800, "setup"), (0x1000, "instant"),
         (0x2000, "runtime"), (0x4000, "oem"), (0x8000, "vendorPrivileged"), (0x10000, "textClassifier"),
         (0x80000, "configurator"), (0x100000, "incidentReportApprover"), (0x200000, "appPredictor"),
         (0x800000, "companion"), (0x1000000, "retailDemo"), (0x2000000, "recents"), (0x4000000, "role"),
         (0x8000000, "knownSigner"), (0x400000, "module")]
PROTECTION_SIGNATURE_OR_SYSTEM = 3
PROTECTION_SIGNATURE = 2
PROTECTION_FLAG_PRIVILEGED = 0x10
PROTECTION_FLAG_VENDOR_PRIVILEGED = 0x8000


def fix_protection_level(level):
    """PermissionInfo.fixProtectionLevel."""
    if level == PROTECTION_SIGNATURE_OR_SYSTEM:
        level = PROTECTION_SIGNATURE | PROTECTION_FLAG_PRIVILEGED
    if level & PROTECTION_FLAG_VENDOR_PRIVILEGED and not level & PROTECTION_FLAG_PRIVILEGED:
        level &= ~PROTECTION_FLAG_VENDOR_PRIVILEGED
    return level


def protection_to_string(level):
    """PermissionInfo.protectionToString, its flags in its order."""
    return BASES.get(level & 0xF, "????") + "".join(f"|{name}" for bit, name in FLAGS if level & bit)


def read_flags(path):
    """aconfig's values as '<package>.<name>=true|false' lines."""
    flags = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle.read().splitlines():
            if "=" in line:
                name, value = line.strip().split("=", 1)
                flags[name] = value == "true"
    return flags


def skipped(feature_flag, flags):
    """AconfigFlags.skipCurrentElement for an element's android:featureFlag, or None for none."""
    if feature_flag is None:
        return False
    name = feature_flag.strip()
    negated = name.startswith("!")
    if negated:
        name = name[1:].strip()
    value = flags.get(name)
    return value is None or value == negated


def platform_permissions(tree, flags):
    """{name: level} of the <permission> elements directly under <manifest> in aapt2's xmltree dump, without those
    their feature flag leaves out."""
    permissions, open_elements, current = {}, [], None
    for line in tree.splitlines():
        indent = len(line) - len(line.lstrip())
        text = line.strip()
        if text.startswith("E: "):
            element = text[3:].split(" ", 1)[0]
            # The parent is the nearest element above with less indentation.
            while open_elements and open_elements[-1][0] >= indent:
                open_elements.pop()
            parent = open_elements[-1][1] if open_elements else None
            open_elements.append((indent, element))
            current = None
            if element == "permission" and parent == "manifest":
                current = {"level": 0, "flag": None}
                permissions.setdefault(None, []).append(current)
        elif text.startswith("A: ") and current is not None:
            match = re.match(r"A: " + re.escape(ANDROID) + r"(\w+)\(0x[0-9a-f]+\)=(.*)", text)
            if not match:
                continue
            attribute, value = match.groups()
            if attribute == "name":
                current["name"] = re.match(r'"([^"]*)"', value).group(1)
            elif attribute == "protectionLevel":
                current["level"] = int(re.search(r"0x[0-9a-f]+$", value).group(), 16)
            elif attribute == "featureFlag":
                current["flag"] = re.match(r'"([^"]*)"', value).group(1)
    return {p["name"]: p["level"] for p in permissions.get(None, []) if not skipped(p["flag"], flags)}


def check_rules():
    """The rules on cases framework-res does not have: an unknown or negated flag, a missing level, signatureOrSystem,
    vendorPrivileged without privileged, and protectionToString's order, where module comes after knownSigner."""
    flags = {"a.on": True, "a.off": False}
    assert [skipped(f, flags) for f in (None, "a.on", "a.off", "!a.on", " ! a.off ", "a.unknown", "!a.unknown")] \
        == [False, False, True, True, False, True, True], "featureFlag"
    assert fix_protection_level(3) == 0x12, "signatureOrSystem"
    assert fix_protection_level(0x8002) == 0x2 and fix_protection_level(0x8012) == 0x8012, "vendorPrivileged"
    assert protection_to_string(0x8400002) == "signature|knownSigner|module", "order"
    assert protection_to_string(0xe) == "????", "unknown base"
    tree = ("N: android=http://schemas.android.com/apk/res/android (line=1)\n  E: manifest (line=1)\n"
            "      E: permission (line=2)\n        A: " + ANDROID + 'name(0x01010003)="p.PLAIN" (Raw: "p.PLAIN")\n'
            "      E: application (line=3)\n        E: permission (line=4)\n          A: " + ANDROID
            + 'name(0x01010003)="p.NESTED" (Raw: "p.NESTED")\n')
    assert platform_permissions(tree, {}) == {"p.PLAIN": 0}, "a level-less permission under manifest only"


def expected_lines(path):
    with open(path, encoding="utf-8") as handle:
        return [line for line in handle.read().splitlines() if line and not line.startswith("#")]


def main():
    aapt2, apk, flags_file, expected = (sys.argv[1:5] if len(sys.argv) == 5 else
                                        (os.path.join(ROOT, "build/aapt2/aapt2"),
                                         os.path.join(ROOT, "build/framework-res/framework-res.apk"),
                                         os.path.join(ROOT, "build/aconfig/feature-flags.txt"),
                                         os.path.join(ROOT, "fixtures/expected/platform-permissions.txt")))
    for needed, how in ((aapt2, "tools/build-aapt2.sh"), (apk, "tools/build-framework-res.sh"),
                        (flags_file, "tools/build-framework-res.sh")):
        if not os.path.exists(needed):
            print(f"skipped: {needed} is not built ({how})")
            return
    check_rules()
    if not os.path.exists(expected):
        raise SystemExit(f"FAIL: no reference {expected}")
    tree = subprocess.run([aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml", apk], check=True,
                          capture_output=True, text=True).stdout
    ours = sorted(f"{name} {protection_to_string(fix_protection_level(level))}"
                  for name, level in platform_permissions(tree, read_flags(flags_file)).items())
    wanted = expected_lines(expected)
    if ours == wanted:
        print(f"platform permissions: {len(ours)} with Android 15's protection levels")
        return
    print(f"FAIL: ours {len(ours)}, Android's {len(wanted)}")
    for line in sorted(set(wanted) - set(ours)):
        print(f"  only Android's: {line}")
    for line in sorted(set(ours) - set(wanted)):
        print(f"  only ours:      {line}")
    sys.exit(1)


if __name__ == "__main__":
    main()
