#!/usr/bin/env python3
"""Signs an APK again so that Android verifies it and Minato cannot (ADR-0048 b8).

The APK keeps its entries and its signer: the new v2 block has one signer with the same key and
certificate, whose only signature is RSASSA-PKCS1-v1_5 over a verity digest (0x0421). Android 15 checks
that digest (ApkSigningBlockUtils.java:127-130, 340-385, android-15.0.0_r1); Minato knows the algorithm but
cannot check it (SignatureVerifier.swift:107). In a set whose other APKs make-apk signed with the same key,
this APK is the one Minato cannot verify, as b7c's set-check.sh needs.

Android builds the verity tree only over a signing block that starts on a 4096-byte page and fills whole
pages (VerityBuilder.java:512-526), so zero bytes go before the block and a padding pair into it, as apksig
does.

Usage: tools/sign-verity.py [--key <folder>] <in.apk> <out.apk>
  --key  make-apk's key folder, with key.der (PKCS#1) and certificate.der; default
         ~/.android/minato-debug-key, the one make-apk signs with
macOS's own openssl (LibreSSL, /usr/bin/openssl) signs; no key is made and nothing is stored. The script
checks what it wrote as Android does before it ends.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

OPENSSL = "/usr/bin/openssl"
V2_BLOCK_ID = 0x7109871A
# apksig's ApkSigningBlockUtils.VERITY_PADDING_BLOCK_ID; Android skips pairs it does not know.
PADDING_BLOCK_ID = 0x42726577
# ApkSigningBlockUtils.SIGNATURE_VERITY_RSA_PKCS1_V1_5_WITH_SHA256 (:435), signed as SHA256withRSA.
ALGORITHM = 0x0421
MAGIC = b"APK Sig Block 42"
PAGE = 4096
# VerityBuilder.DEFAULT_SALT (:51).
SALT = bytes(8)


def u32(value):
    return struct.pack("<I", value)


def u64(value):
    return struct.pack("<Q", value)


def prefixed(data):
    return u32(len(data)) + data


def fail(message):
    sys.exit("sign-verity: " + message)


def end_record(data):
    """The offsets of the central directory and of the end record, as make-apk finds them."""
    for offset in range(len(data) - 22, max(-1, len(data) - 22 - 0xFFFF - 1), -1):
        if data[offset:offset + 4] == b"PK\x05\x06" and offset + 22 + struct.unpack_from("<H", data, offset + 20)[0] == len(data):
            central = struct.unpack_from("<I", data, offset + 16)[0]
            if central == 0xFFFFFFFF or central > offset:
                fail("a ZIP64 archive or a central directory after its end record")
            return central, offset
    fail("not a ZIP archive with a central directory")


def signing_block(data, central):
    """The start of the APK Signing Block that ends at the central directory, or None."""
    if central < 32 or data[central - 16:central] != MAGIC:
        return None
    size = struct.unpack_from("<Q", data, central - 24)[0]
    start = central - (size + 8)
    if start < 0 or struct.unpack_from("<Q", data, start)[0] != size:
        fail("the signing block's two size fields differ")
    return start


def verity_root(content):
    """VerityBuilder.generateVerityTreeInternal (:367-392) over content padded to whole pages: SHA-256 of the salt
    and each page, the digests padded to whole pages, again until one page is left, then that page's digest."""
    def level(pages):
        digests = b"".join(hashlib.sha256(SALT + pages[at:at + PAGE]).digest() for at in range(0, len(pages), PAGE))
        return digests + bytes(-len(digests) % PAGE)

    hashes = level(content)
    while len(hashes) > PAGE:
        hashes = level(hashes)
    return hashlib.sha256(SALT + hashes[:PAGE]).digest()


def verity_digest(entries, central, end, block_offset):
    """The content digest: the tree's root over the file without its signing block, whose end record names the
    block's offset as the central directory's (VerityBuilder.java:292-335), then the length of that content as an
    int64 (ApkSigningBlockUtils.java:340-363)."""
    content = entries + central + end[:16] + u32(block_offset) + end[20:]
    return verity_root(content + bytes(-len(content) % PAGE)) + u64(len(content))


def openssl(arguments, data=b""):
    result = subprocess.run([OPENSSL] + arguments, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        fail("openssl " + arguments[0] + ": " + result.stderr.decode(errors="replace").strip())
    return result.stdout


def public_key(certificate):
    """The certificate's SubjectPublicKeyInfo, DER, which Android compares with the signer's byte for byte."""
    pem = openssl(["x509", "-inform", "DER", "-noout", "-pubkey"], certificate)
    return openssl(["pkey", "-pubin", "-outform", "DER"], pem)


def sign(apk, key_folder):
    central_offset, end_offset = end_record(apk)
    start = signing_block(apk, central_offset)
    entries = apk[:central_offset if start is None else start]
    central = apk[central_offset:end_offset]
    end = apk[end_offset:]
    entries += bytes(-len(entries) % PAGE)
    block_offset = len(entries)

    certificate = open(os.path.join(key_folder, "certificate.der"), "rb").read()
    digest = verity_digest(entries, central, end, block_offset)
    signed_data = (prefixed(prefixed(u32(ALGORITHM) + prefixed(digest)))
                   + prefixed(prefixed(certificate))
                   + prefixed(b""))
    signature = openssl(["dgst", "-sha256", "-keyform", "DER", "-sign", os.path.join(key_folder, "key.der")], signed_data)
    signer = (prefixed(signed_data)
              + prefixed(prefixed(u32(ALGORITHM) + prefixed(signature)))
              + prefixed(public_key(certificate)))
    value = prefixed(prefixed(signer))
    pairs = u64(4 + len(value)) + u32(V2_BLOCK_ID) + value

    # Whole pages: both size fields and the magic come to 32 bytes, a pair needs 12 of its own.
    missing = -(len(pairs) + 32) % PAGE
    if missing:
        if missing < 12:
            missing += PAGE
        pairs += u64(missing - 8) + u32(PADDING_BLOCK_ID) + bytes(missing - 12)
    block = u64(len(pairs) + 24) + pairs + u64(len(pairs) + 24) + MAGIC
    return entries + block + central + end[:16] + u32(block_offset + len(block)) + end[20:]


def check(apk, key_folder):
    """Reads what was written as Android 15 reads it: the block on whole pages, one v2 signer with the verity
    algorithm, its digest of this file, and its signature by the key's certificate."""
    central_offset, end_offset = end_record(apk)
    start = signing_block(apk, central_offset)
    if start is None or start % PAGE or (central_offset - start) % PAGE:
        fail("check: the signing block does not lie on whole pages")
    value = None
    at = start + 8
    while at < central_offset - 24:
        length = struct.unpack_from("<Q", apk, at)[0]
        if struct.unpack_from("<I", apk, at + 8)[0] == V2_BLOCK_ID:
            value = apk[at + 12:at + 8 + length]
        at += 8 + length
    if value is None or at != central_offset - 24:
        fail("check: no v2 block, or pairs that do not end at the block's size field")

    def take(data):
        size = struct.unpack_from("<I", data)[0]
        return data[4:4 + size], data[4 + size:]

    signers, rest = take(value)
    signer, others = take(signers)
    if rest or others:
        fail("check: more than one signer")
    signed_data, rest = take(signer)
    signatures, rest = take(rest)
    key, rest = take(rest)
    digests, after = take(signed_data)
    certificates, _ = take(after)
    record, rest = take(digests)
    algorithm, digest = struct.unpack_from("<I", record)[0], take(record[4:])[0]
    certificate = take(certificates)[0]
    entry, more = take(signatures)
    signature = take(entry[4:])[0]
    if algorithm != ALGORITHM or struct.unpack_from("<I", entry)[0] != ALGORITHM or rest or more:
        fail("check: a digest or a signature of another algorithm")
    if certificate != open(os.path.join(key_folder, "certificate.der"), "rb").read() or key != public_key(certificate):
        fail("check: another certificate or public key than the key folder's")
    if digest != verity_digest(apk[:start], apk[central_offset:end_offset], apk[end_offset:], start):
        fail("check: the verity digest is not this file's")
    with tempfile.TemporaryDirectory() as folder:
        for name, data in (("data", signed_data), ("signature", signature), ("key.der", key)):
            with open(os.path.join(folder, name), "wb") as file:
                file.write(data)
        openssl(["dgst", "-sha256", "-keyform", "DER", "-verify", os.path.join(folder, "key.der"), "-signature",
                 os.path.join(folder, "signature"), os.path.join(folder, "data")])


def main(arguments):
    key_folder = os.path.expanduser("~/.android/minato-debug-key")
    if arguments[:1] == ["--key"] and len(arguments) >= 2:
        key_folder, arguments = arguments[1], arguments[2:]
    if len(arguments) != 2:
        sys.stderr.write("usage: tools/sign-verity.py [--key <folder>] <in.apk> <out.apk>\n")
        return 64
    source, destination = arguments
    apk = open(source, "rb").read()
    signed = sign(apk, key_folder)
    check(signed, key_folder)
    with open(destination, "wb") as file:
        file.write(signed)
    print("sign-verity: wrote %s (%d bytes, its v2 signer's only digest a verity one)" % (destination, len(signed)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
