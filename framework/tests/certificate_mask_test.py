#!/usr/bin/env python3
"""framework/tests/certificate_mask.py (ADR-0039 b3.5b1): both harnesses replace the certificate SHA-256s that the
test APK's check proved, which make-apk's per-machine debug key gives (ADR-0031), each by the mark of its role, and
nothing else. Run with a stand-in for apk-inspect in a scratch directory."""
import base64
import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import certificate_mask  # noqa: E402

failures = 0
SIGNER_DER = b"\x30\x03own"
PAST_DER = b"\x30\x04past"
OLDEST_DER = b"\x30\x06oldest"
SECOND_DER = b"\x30\x06second"
OWN, PAST, OLDEST, SECOND = (hashlib.sha256(der).hexdigest() for der in (SIGNER_DER, PAST_DER, OLDEST_DER, SECOND_DER))
OTHER = "3257d599a49d2c961a471ca9843f59d341a405884583fc087df4237b733bbd6d"
SIGNER = certificate_mask.SIGNER


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def b64(der):
    return base64.b64encode(der).decode()


# A rotated key: the signer, and two older keys, oldest first; the lineage ends with the signer's own.
ROTATED = {"scheme": "v3", "certificates": [b64(SIGNER_DER)],
           "lineage": [{"certificate": b64(OLDEST_DER), "flags": 23}, {"certificate": b64(PAST_DER), "flags": 23},
                       {"certificate": b64(SIGNER_DER), "flags": 23}]}
marks = certificate_mask.roles(ROTATED)
check(marks == {OWN: SIGNER, OLDEST: "<the APK's past certificate 1>", PAST: "<the APK's past certificate 2>"},
      f"a rotated key's marks: {marks}")
check(certificate_mask.roles({"scheme": "v1", "certificates": [b64(SIGNER_DER), b64(SECOND_DER)], "lineage": []})
      == {OWN: SIGNER, SECOND: "<the APK's certificate 2>"}, "several signers")

# Each role keeps its own mark, so the oldest in place of the signer's still shows; another SHA-256 stays visible.
text = f"I/app: signatures [{OLDEST}]\nI/app: history [{OLDEST}, {PAST}, {OWN}]\nI/app: other {OTHER}\n"
masked = certificate_mask.mask(text, marks)
check(masked == "I/app: signatures [<the APK's past certificate 1>]\n"
      f"I/app: history [<the APK's past certificate 1>, <the APK's past certificate 2>, {SIGNER}]\n"
      f"I/app: other {OTHER}\n", f"mask: {masked!r}")
check(certificate_mask.mask(text, {}) == text, "nothing proven, no change")
# Only a whole SHA-256 is a certificate: anything else refuses, so that a wrong reading cannot mask broadly.
for wrong in ("", "abc", OWN.upper(), OWN[:63], OWN + "0"):
    try:
        certificate_mask.mask(text, {wrong: SIGNER})
        check(False, f"accepted {wrong!r}")
    except ValueError:
        pass

# The proof comes from apk-inspect --json; an APK whose check proved nothing has none.
with tempfile.TemporaryDirectory() as scratch:
    inspect = os.path.join(scratch, "apk-inspect")
    reports = {"app.apk": {"signature": {"certificates": [], "proof": ROTATED}},
               "unverified.apk": {"signature": {"certificates": [{"sha256": OWN, "subject": "x"}]}}}
    for name, report in reports.items():
        with open(os.path.join(scratch, name + ".json"), "w") as handle:
            json.dump(report, handle)
    with open(inspect, "w") as handle:
        handle.write("#!/bin/sh\n"
                     '[ "$1" = --json ] || exit 2\n'
                     f'cat "{scratch}/$2.json"\n')
    os.chmod(inspect, os.stat(inspect).st_mode | stat.S_IEXEC)
    check(certificate_mask.marks(inspect, "app.apk") == marks, "apk-inspect's proof")
    check(certificate_mask.marks(inspect, "unverified.apk") == {}, "no proof, no marks")
    result = subprocess.run([sys.executable, certificate_mask.__file__, inspect, "app.apk"], input=text,
                            capture_output=True, text=True)
    check(result.returncode == 0 and result.stdout == masked, f"the filter: {result.stdout!r} {result.stderr!r}")

if failures:
    print(f"certificate_mask_test: {failures} failed")
    sys.exit(1)
print("certificate_mask_test: fine")
