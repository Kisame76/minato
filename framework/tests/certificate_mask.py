#!/usr/bin/env python3
"""The marks both harnesses put in place of a test APK's certificate SHA-256s (ADR-0039 b3.5b1): make-apk signs with
a per-machine debug key (ADR-0031), so the certificates differ from machine to machine, while the reference run on
Android and ours install the same APK. framework/tests/java_app_run.py masks our log with them, and
tools/android-reference.sh Android's, before they are compared, so fixtures/expected stays Android's output with the
same marks applied.

Only what the APK's signature check proved, as apk-inspect --json reports it, is replaced, each certificate by the
mark of its role: the signer's own (SIGNER, further signers numbered from 2) and a rotated key's older ones, oldest
first (numbered from 1). Any other SHA-256 stays as it is, and the roles keep their own marks, so a wrong certificate,
or the lineage's oldest in place of the signer's, still shows.

Usage: certificate_mask.py <apk-inspect> <app.apk>   (copies stdin to stdout, masked)
"""
import base64
import hashlib
import json
import re
import subprocess
import sys

SIGNER = "<the APK's certificate>"


def marks(apk_inspect, apk):
    """Each certificate the APK's check proved, by SHA-256, with its mark: none when the check proved nothing."""
    report = subprocess.run([apk_inspect, "--json", apk], check=True, capture_output=True, text=True).stdout
    proof = json.loads(report)["signature"].get("proof")
    return roles(proof) if proof else {}


def roles(proof):
    """The marks for apk-inspect's proof: its certificates (base64 DER), and its lineage, the signer's own last."""
    result = {}
    for index, certificate in enumerate(proof["certificates"]):
        result.setdefault(sha256(certificate), SIGNER if index == 0 else f"<the APK's certificate {index + 1}>")
    for index, level in enumerate(proof["lineage"][:-1]):
        result.setdefault(sha256(level["certificate"]), f"<the APK's past certificate {index + 1}>")
    return result


def sha256(certificate):
    return hashlib.sha256(base64.b64decode(certificate, validate=True)).hexdigest()


def mask(text, marks):
    """The text with each SHA-256 of `marks`, 64 lowercase hex digits, replaced by its mark."""
    for digest, mark in marks.items():
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"not a SHA-256: {digest!r}")
        text = text.replace(digest, mark)
    return text


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.stdout.write(mask(sys.stdin.read(), marks(sys.argv[1], sys.argv[2])))


if __name__ == "__main__":
    main()
