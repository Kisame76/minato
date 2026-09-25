#!/usr/bin/env python3
"""Static safety and authenticity check for a local APK. Reads the file only; executes nothing from it.

Reports the SHA-256, contents that deserve a look (native libraries, code outside classes*.dex),
v1 signature files, and verifies APK Signature Scheme v2/v3/v3.1 cryptographically: the content
digest is recomputed over the file and each signer's signature is checked with openssl.

Usage: tools/apk-check.py <file.apk>
Permissions and components: swift run --package-path importer/APKKit apk-inspect <file.apk>
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter

SCHEMES = {0x7109871A: "v2", 0xF05368C0: "v3", 0x1B93AD61: "v3.1"}
SOURCE_STAMP_IDS = {0x6DFF800D: "source stamp v2", 0x2B09189E: "source stamp v1"}
# Google Play's source stamp certificate (SHA-256 of the DER).
PLAY_STAMP_SHA256 = "3257d599a49d2c961a471ca9843f59d341a405884583fc087df4237b733bbd6d"
ALGORITHMS = {  # signature algorithm id: (content hash, openssl digest, openssl options)
    0x0101: ("sha256", "-sha256", ["-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32"]),
    0x0102: ("sha512", "-sha512", ["-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:64"]),
    0x0103: ("sha256", "-sha256", []),
    0x0104: ("sha512", "-sha512", []),
    0x0201: ("sha256", "-sha256", []),
    0x0202: ("sha512", "-sha512", []),
    0x0301: ("sha256", "-sha256", []),
}


# Supported by Android, but over the verity tree, which this script does not compute.
VERITY_ALGORITHMS = {0x0421, 0x0423, 0x0425}
STRIPPING_PROTECTION_ID = 0xBEEFF00D  # additional attribute of v2: "this APK also had a v3 block"


def length_prefixed(buffer, offset, what="a field"):
    """The uint32-length-prefixed field at offset, and the offset after it; ValueError, naming what, if
    it does not fit, as Android refuses it (getLengthPrefixedSlice)."""
    if offset + 4 > len(buffer):
        raise ValueError(f"{what}: its length prefix runs past the end")
    size = struct.unpack("<I", buffer[offset:offset + 4])[0]
    if offset + 4 + size > len(buffer):
        raise ValueError(f"{what}: its length runs past the end")
    return buffer[offset + 4:offset + 4 + size], offset + 4 + size


def records(sequence, what):
    """The length-prefixed records of a sequence."""
    result, position = [], 0
    while position < len(sequence):
        record, position = length_prefixed(sequence, position, what)
        result.append(record)
    return result


def der_element(der, offset):
    """(start, content start, end) of the DER element at offset."""
    if offset + 2 > len(der):
        raise ValueError("a DER element runs past the certificate")
    length, content = der[offset + 1], offset + 2
    if length & 0x80:
        count = length & 0x7F
        length, content = int.from_bytes(der[content:content + count], "big"), content + count
    if content + length > len(der):
        raise ValueError("a DER element runs past the certificate")
    return offset, content, content + length


def subject_public_key_info(certificate):
    """The certificate's SubjectPublicKeyInfo, exactly as it stands in the certificate (RFC 5280)."""
    _, content, _ = der_element(certificate, 0)
    _, position, end = der_element(certificate, content)  # TBSCertificate
    fields = []
    while position < end:
        fields.append(der_element(certificate, position))
        position = fields[-1][2]
    index = (1 if fields and certificate[fields[0][0]] == 0xA0 else 0) + 5  # [0] version, serial, algorithm, issuer, validity, subject
    if index >= len(fields):
        raise ValueError("the certificate has no SubjectPublicKeyInfo")
    return certificate[fields[index][0]:fields[index][2]]


def describe_certificate(der, tmp):
    """Subject and validity of an X.509 certificate, or None where openssl cannot read it."""
    path = os.path.join(tmp, "cert.der")
    with open(path, "wb") as handle:
        handle.write(der)
    result = subprocess.run(["openssl", "x509", "-inform", "DER", "-in", path, "-noout", "-subject", "-startdate", "-enddate"],
                            capture_output=True, text=True)
    return result.stdout.strip().replace("\n", " | ") if result.returncode == 0 else None


def content_digest(data, block_start, cd_offset, eocd, hash_name):
    patched_eocd = bytearray(data[eocd:])
    patched_eocd[16:20] = struct.pack("<I", block_start)
    chunk_digests = []
    for section in (data[:block_start], data[cd_offset:eocd], bytes(patched_eocd)):
        for start in range(0, len(section), 1 << 20):
            chunk = section[start:start + (1 << 20)]
            chunk_digests.append(hashlib.new(hash_name, b"\xa5" + struct.pack("<I", len(chunk)) + chunk).digest())
    return hashlib.new(hash_name, b"\x5a" + struct.pack("<I", len(chunk_digests)) + b"".join(chunk_digests)).digest()


def signer_problems(scheme, signer, has_v3):
    """What Android's verifier refuses in a signer's structure (ApkSignatureSchemeV2Verifier and V3):
    returns the parts and a list of problems. Raises ValueError where Android cannot read the signer."""
    signed_data, offset = length_prefixed(signer, 0, "the signed data")
    if scheme != "v2":
        offset += 8  # minSdk, maxSdk
    signatures, offset = length_prefixed(signer, offset, "the signatures")
    public_key, _ = length_prefixed(signer, offset, "the public key")
    digests_blob, offset = length_prefixed(signed_data, 0, "the digests")
    certificates, offset = length_prefixed(signed_data, offset, "the certificates")
    if scheme != "v2":
        offset += 8  # minSdk, maxSdk
    attributes, _ = length_prefixed(signed_data, offset, "the additional attributes")

    digest_records, signature_records = records(digests_blob, "a digest"), records(signatures, "a signature")
    if any(len(record) < 8 for record in digest_records + signature_records):
        raise ValueError("a digest or signature record is too short for its algorithm and length")
    digest_algorithms = [struct.unpack("<I", record[:4])[0] for record in digest_records]
    signature_algorithms = [struct.unpack("<I", record[:4])[0] for record in signature_records]
    digests = {a: length_prefixed(r, 4, "a digest")[0] for a, r in zip(digest_algorithms, digest_records)}
    signature_values = [(a, length_prefixed(r, 4, "a signature")[0]) for a, r in zip(signature_algorithms, signature_records)]
    certificate_list = records(certificates, "a certificate")

    problems = []
    if digest_algorithms != signature_algorithms:
        problems.append("the algorithms of digests and signatures differ: "
                        f"{[hex(a) for a in digest_algorithms]} and {[hex(a) for a in signature_algorithms]}")
    if not any(a in ALGORITHMS or a in VERITY_ALGORITHMS for a in signature_algorithms):
        problems.append("no supported signature algorithm")
    elif not any(a in ALGORITHMS for a in signature_algorithms):
        problems.append("only verity algorithms, whose digest this script does not compute: nothing to check")
    if not certificate_list:
        problems.append("no certificate")
    else:
        try:
            if public_key != subject_public_key_info(certificate_list[0]):
                problems.append("the public key is not the first certificate's SubjectPublicKeyInfo")
        except ValueError as error:
            problems.append(f"the first certificate's SubjectPublicKeyInfo cannot be found ({error})")
    for attribute in records(attributes, "an additional attribute"):
        if len(attribute) < 4:
            problems.append("an additional attribute is too short for its ID")
        elif scheme == "v2" and struct.unpack("<I", attribute[:4])[0] == STRIPPING_PROTECTION_ID:
            if len(attribute) < 8:
                problems.append("the stripping protection attribute is too short")
            elif struct.unpack("<I", attribute[4:8])[0] == 3 and not has_v3:
                problems.append("the stripping protection says the APK was signed with v3, but there is no v3 block")
    parts = {"signed_data": signed_data, "public_key": public_key, "certificates": certificate_list,
             "digests": digests, "signatures": signature_values}
    return parts, problems


def verify_signer(scheme, signer, data, block_start, cd_offset, eocd, tmp, has_v3):
    try:
        parts, problems = signer_problems(scheme, signer, has_v3)
    except ValueError as error:
        print(f"  {scheme} PROBLEM: the signer does not parse ({error})")
        return False
    signed_data, public_key, digests = parts["signed_data"], parts["public_key"], parts["digests"]

    for number, der in enumerate(parts["certificates"], 1):
        print(f"  {scheme} certificate sha256 {hashlib.sha256(der).hexdigest()}")
        description = describe_certificate(der, tmp)
        print(f"    {description or 'unparseable certificate'}")
        if description is None:  # Android decodes every certificate
            problems.append(f"certificate #{number} does not parse")
    for problem in problems:
        print(f"  {scheme} PROBLEM: {problem}")

    key_der, key_pem = os.path.join(tmp, "key.der"), os.path.join(tmp, "key.pem")
    with open(key_der, "wb") as handle:
        handle.write(public_key)
    if subprocess.run(["openssl", "pkey", "-pubin", "-inform", "DER", "-in", key_der, "-out", key_pem],
                      capture_output=True).returncode != 0:
        print(f"  {scheme} PROBLEM: the public key does not parse")
        return False

    all_ok = not problems
    for algorithm, signature in parts["signatures"]:
        if algorithm not in ALGORITHMS:
            print(f"  {scheme} algorithm 0x{algorithm:04x}: not checked")
            continue
        hash_name, digest_option, options = ALGORITHMS[algorithm]
        signature_path, message_path = os.path.join(tmp, "sig"), os.path.join(tmp, "msg")
        with open(signature_path, "wb") as handle:
            handle.write(signature)
        with open(message_path, "wb") as handle:
            handle.write(signed_data)
        signature_ok = subprocess.run(["openssl", "dgst", digest_option, "-verify", key_pem, *options,
                                       "-signature", signature_path, message_path], capture_output=True).returncode == 0
        digest_ok = digests.get(algorithm) == content_digest(data, block_start, cd_offset, eocd, hash_name)
        all_ok &= signature_ok and digest_ok
        print(f"  {scheme} algorithm 0x{algorithm:04x}: signature {'OK' if signature_ok else 'FAILED'}, "
              f"content digest {'OK' if digest_ok else 'MISMATCH'}")
    return all_ok


class NotFound(Exception):
    """Android finds no APK Signing Block (SignatureNotFoundException), so it verifies no v2 or v3 signature."""


def signing_block(data):
    """The APK Signing Block where Android finds it (ApkSigningBlockUtils.findSignature): its start, the
    offsets of the central directory and of the end record, and its pairs. NotFound where Android finds none."""
    eocd = data.rfind(b"PK\x05\x06")
    if eocd < 0 or eocd + 22 > len(data):
        raise NotFound("no ZIP end record")
    if eocd >= 20 and data[eocd - 20:eocd - 16] == b"PK\x06\x07":
        raise NotFound("a ZIP64 archive, which Android does not verify")
    cd_size, cd_offset = struct.unpack("<II", data[eocd + 12:eocd + 20])
    if cd_offset + cd_size != eocd:
        raise NotFound("the central directory does not end where the end record starts")
    if cd_offset < 32:
        raise NotFound("the central directory starts before byte 32, which leaves no room for a block")
    if data[cd_offset - 16:cd_offset] != b"APK Sig Block 42":
        raise NotFound("nothing before the central directory (v1 only or unsigned)")
    size = struct.unpack("<Q", data[cd_offset - 24:cd_offset - 16])[0]
    if size < 24 or size > 0x7FFFFFFF - 8:
        raise NotFound(f"the block's size is out of range: {size}")
    block_start = cd_offset - size - 8
    if block_start < 0:
        raise NotFound(f"the block's offset is out of range: {block_start}")
    if struct.unpack("<Q", data[block_start:block_start + 8])[0] != size:
        raise NotFound("the block's sizes in header and footer differ")
    return block_start, cd_offset, eocd, data[block_start + 8:cd_offset - 24]


def pairs_of(block):
    """The block's ID-value pairs up to the first that Android cannot read, and what is wrong with that
    one. Android's search for a scheme's block ends there (findApkSignatureSchemeBlock), so a block after
    it is as good as missing."""
    pairs, position = [], 0
    while position < len(block):
        number = len(pairs) + 1
        if len(block) - position < 8:
            return pairs, f"pair #{number} has no room for its size"
        length = struct.unpack("<Q", block[position:position + 8])[0]
        if length < 4 or length > min(0x7FFFFFFF, len(block) - position - 8):
            return pairs, f"pair #{number} has a size out of range: {length}"
        block_id = struct.unpack("<I", block[position + 8:position + 12])[0]
        pairs.append((block_id, block[position + 12:position + 8 + length]))
        position += 8 + length
    return pairs, None


def stamp_origin(value):
    """Who stamped the APK, by the stamp's certificate. Android's signature check does not read stamps."""
    try:
        certificate = length_prefixed(value, 0, "the stamp's certificate")[0]
    except ValueError as error:
        return f"does not parse ({error})"
    stamp_hashes = {hashlib.sha256(certificate).hexdigest()}
    try:
        stamp_hashes.add(hashlib.sha256(length_prefixed(certificate, 0)[0]).hexdigest())
    except ValueError:
        pass  # the certificate itself, without a further length prefix
    origin = "Google Play" if PLAY_STAMP_SHA256 in stamp_hashes else "unknown stamp"
    return f"{origin} (stamp signature itself not verified)"


def main(path):
    with open(path, "rb") as handle:
        data = handle.read()
    print(f"file:   {os.path.basename(path)}")
    print(f"sha256: {hashlib.sha256(data).hexdigest()}")
    print(f"size:   {len(data):,} bytes")

    zip_opens, v1_files = True, []
    try:
        infos = zipfile.ZipFile(path).infolist()
    except (zipfile.BadZipFile, ValueError, struct.error) as error:
        # Android's zip reader refuses what Python's cannot open.
        print(f"PROBLEM: the ZIP does not open ({error})")
        zip_opens = False
    else:
        print(f"entries: {len(infos)}, top level: {Counter(i.filename.split('/')[0] for i in infos).most_common(8)}")
        abis = sorted({i.filename.split("/")[1] for i in infos if i.filename.startswith("lib/") and i.filename.count("/") >= 2})
        print(f"ABIs: {abis or 'none (no native code)'}")
        extra_code = [i.filename for i in infos
                      if i.filename.lower().endswith((".dex", ".jar", ".apk", ".odex", ".vdex", ".so"))
                      and not i.filename.startswith("lib/")
                      and not (i.filename.startswith("classes") and i.filename.endswith(".dex"))]
        print(f"code outside lib/ and classes*.dex: {extra_code or 'none'}")
        v1_files = [i.filename for i in infos if i.filename.startswith("META-INF/")
                    and i.filename.upper().endswith((".RSA", ".DSA", ".EC"))]
        print(f"v1 signature files: {v1_files or 'none'}")

    try:
        block_start, cd_offset, eocd, block = signing_block(data)
    except NotFound as reason:
        print(f"APK Signing Block: none that Android finds: {reason}")
        if v1_files:
            return 0  # v1 only, which this script does not verify
        print("RESULT: SIGNATURE PROBLEM")
        return 1
    pairs, unreadable = pairs_of(block)
    if unreadable:
        print(f"APK Signing Block: {unreadable}; Android reads no pair after it")
    has_v3 = any(block_id in (0xF05368C0, 0x1B93AD61) for block_id, _ in pairs)

    verified = []
    with tempfile.TemporaryDirectory() as tmp:
        for block_id, value in pairs:
            if block_id in SOURCE_STAMP_IDS:
                print(f"{SOURCE_STAMP_IDS[block_id]}: {stamp_origin(value)}")
            scheme = SCHEMES.get(block_id)
            if not scheme:
                continue
            try:
                signers = records(length_prefixed(value, 0, f"the {scheme} block")[0], "a signer")
            except ValueError as error:
                print(f"  {scheme} PROBLEM: the block does not parse ({error})")
                verified.append(False)
                continue
            if not signers:
                print(f"  {scheme} PROBLEM: no signer")
                verified.append(False)
            for signer in signers:
                verified.append(verify_signer(scheme, signer, data, block_start, cd_offset, eocd, tmp, has_v3))
    if not verified:
        print("PROBLEM: no v2, v3 or v3.1 block that Android finds")
    ok = zip_opens and bool(verified) and all(verified)
    print(f"RESULT: {'signatures valid' if ok else 'SIGNATURE PROBLEM'}")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
