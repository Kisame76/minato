#!/usr/bin/env python3
"""Writes throwaway signing identities and .p12 files in every format the certificate import must
read (ADR-0023) into a folder, for tools/certificate-check.sh. Nothing here is a real certificate:
two self-signed identities with the team TESTTEAM01, made anew on every run.

  own.der, other.der   the two certificates; the app is told own.der is its own
  legacy.p12           LibreSSL's default (RC2/3DES, SHA-1 MAC), password "pw": AltStore 2.0
  openssl3.p12         OpenSSL 3's default (PBES2, AES-256, HMAC-SHA256), password "pw": AltStore 2.2+
  empty.p12            the same with an empty password: AltStore's Export without typing one
  sidestore.p12        no encryption, no MAC, no attributes: SideStore 0.6.4 and later
  other.p12            the other identity in SideStore's layout

openssl3.p12 and empty.p12 need OpenSSL 3 (Homebrew's openssl@3); without it they are left out.
Usage: tools/make-test-certificates.py <folder>"""
import os
import subprocess
import sys

LIBRESSL = "/usr/bin/openssl"
OPENSSL3 = "/opt/homebrew/opt/openssl@3/bin/openssl"


def run(*args):
    subprocess.run(args, check=True, capture_output=True)


def identity(folder, name, common_name):
    """A self-signed RSA-2048 identity; returns the PEM key and certificate paths."""
    key, certificate = f"{folder}/{name}.key.pem", f"{folder}/{name}.cert.pem"
    run(LIBRESSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", certificate,
        "-days", "365", "-subj", f"/CN={common_name}/OU=TESTTEAM01/O=APK-Runner Tests/C=DE")
    run(LIBRESSL, "x509", "-in", certificate, "-outform", "DER", "-out", f"{folder}/{name}.der")
    run(LIBRESSL, "pkcs8", "-topk8", "-nocrypt", "-in", key, "-outform", "DER", "-out", f"{folder}/{name}.pk8")
    return key, certificate


# DER, just enough for the PKCS #12 layout of RFC 7292.
def element(tag, body):
    length = len(body)
    if length < 0x80:
        encoded = bytes([length])
    else:
        octets = length.to_bytes((length.bit_length() + 7) // 8, "big")
        encoded = bytes([0x80 | len(octets)]) + octets
    return bytes([tag]) + encoded + body


def sequence(*parts):
    return element(0x30, b"".join(parts))


def explicit(body):
    return element(0xA0, body)


def oid(dotted):
    arcs = [int(arc) for arc in dotted.split(".")]
    body = bytes([40 * arcs[0] + arcs[1]])
    for arc in arcs[2:]:
        chunk = [arc & 0x7F]
        arc >>= 7
        while arc:
            chunk.insert(0, 0x80 | (arc & 0x7F))
            arc >>= 7
        body += bytes(chunk)
    return element(0x06, body)


def data_part(content):
    return sequence(oid("1.2.840.113549.1.7.1"), explicit(element(0x04, content)))


def unencrypted_pkcs12(pkcs8, certificate):
    """PFX v3 as SideStore 0.6.4+ writes it: a key part, then a certificate part, no MAC."""
    key_bag = sequence(oid("1.2.840.113549.1.12.10.1.1"), explicit(pkcs8))
    cert_bag = sequence(oid("1.2.840.113549.1.12.10.1.3"),
                        explicit(sequence(oid("1.2.840.113549.1.9.22.1"), explicit(element(0x04, certificate)))))
    safe = sequence(data_part(sequence(key_bag)), data_part(sequence(cert_bag)))
    return sequence(element(0x02, b"\x03"), data_part(safe))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    key, certificate = identity(folder, "own", "Apple Development: Test (TEST000001)")
    identity(folder, "other", "Apple Development: Other (TEST000002)")

    run(LIBRESSL, "pkcs12", "-export", "-inkey", key, "-in", certificate, "-passout", "pass:pw",
        "-out", f"{folder}/legacy.p12")
    if os.path.exists(OPENSSL3):
        run(OPENSSL3, "pkcs12", "-export", "-inkey", key, "-in", certificate, "-passout", "pass:pw",
            "-out", f"{folder}/openssl3.p12")
        run(OPENSSL3, "pkcs12", "-export", "-inkey", key, "-in", certificate, "-passout", "pass:",
            "-out", f"{folder}/empty.p12")
    else:
        print("OpenSSL 3 not found: openssl3.p12 and empty.p12 are left out", file=sys.stderr)
    for name in ("own", "other"):
        with open(f"{folder}/{name}.pk8", "rb") as pkcs8, open(f"{folder}/{name}.der", "rb") as der:
            target = "sidestore" if name == "own" else "other"
            with open(f"{folder}/{target}.p12", "wb") as out:
                out.write(unencrypted_pkcs12(pkcs8.read(), der.read()))
    # The keys only need to exist inside the .p12 files.
    for name in os.listdir(folder):
        if name.endswith((".pem", ".pk8")):
            os.remove(os.path.join(folder, name))


if __name__ == "__main__":
    main()
