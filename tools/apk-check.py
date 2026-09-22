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


def length_prefixed(buffer, offset):
    size = struct.unpack("<I", buffer[offset:offset + 4])[0]
    return buffer[offset + 4:offset + 4 + size], offset + 4 + size


def describe_certificate(der, tmp):
    path = os.path.join(tmp, "cert.der")
    with open(path, "wb") as handle:
        handle.write(der)
    result = subprocess.run(["openssl", "x509", "-inform", "DER", "-in", path, "-noout", "-subject", "-startdate", "-enddate"],
                            capture_output=True, text=True)
    return result.stdout.strip().replace("\n", " | ") or "unparseable certificate"


def content_digest(data, block_start, cd_offset, eocd, hash_name):
    patched_eocd = bytearray(data[eocd:])
    patched_eocd[16:20] = struct.pack("<I", block_start)
    chunk_digests = []
    for section in (data[:block_start], data[cd_offset:eocd], bytes(patched_eocd)):
        for start in range(0, len(section), 1 << 20):
            chunk = section[start:start + (1 << 20)]
            chunk_digests.append(hashlib.new(hash_name, b"\xa5" + struct.pack("<I", len(chunk)) + chunk).digest())
    return hashlib.new(hash_name, b"\x5a" + struct.pack("<I", len(chunk_digests)) + b"".join(chunk_digests)).digest()


def verify_signer(scheme, signer, data, block_start, cd_offset, eocd, tmp):
    signed_data, offset = length_prefixed(signer, 0)
    if scheme != "v2":
        offset += 8  # minSdk, maxSdk
    signatures, offset = length_prefixed(signer, offset)
    public_key, _ = length_prefixed(signer, offset)

    digests_blob, offset = length_prefixed(signed_data, 0)
    certificates, _ = length_prefixed(signed_data, offset)
    digests = {}
    position = 0
    while position < len(digests_blob):
        record, position = length_prefixed(digests_blob, position)
        digests[struct.unpack("<I", record[:4])[0]] = length_prefixed(record, 4)[0]

    position = 0
    while position < len(certificates):
        der, position = length_prefixed(certificates, position)
        print(f"  {scheme} certificate sha256 {hashlib.sha256(der).hexdigest()}")
        print(f"    {describe_certificate(der, tmp)}")

    key_der, key_pem = os.path.join(tmp, "key.der"), os.path.join(tmp, "key.pem")
    with open(key_der, "wb") as handle:
        handle.write(public_key)
    subprocess.run(["openssl", "pkey", "-pubin", "-inform", "DER", "-in", key_der, "-out", key_pem], check=True, capture_output=True)

    all_ok = True
    position = 0
    while position < len(signatures):
        record, position = length_prefixed(signatures, position)
        algorithm = struct.unpack("<I", record[:4])[0]
        if algorithm not in ALGORITHMS:
            print(f"  {scheme} algorithm 0x{algorithm:04x}: not checked")
            continue
        hash_name, digest_option, options = ALGORITHMS[algorithm]
        signature_path, message_path = os.path.join(tmp, "sig"), os.path.join(tmp, "msg")
        with open(signature_path, "wb") as handle:
            handle.write(length_prefixed(record, 4)[0])
        with open(message_path, "wb") as handle:
            handle.write(signed_data)
        signature_ok = subprocess.run(["openssl", "dgst", digest_option, "-verify", key_pem, *options,
                                       "-signature", signature_path, message_path], capture_output=True).returncode == 0
        digest_ok = digests.get(algorithm) == content_digest(data, block_start, cd_offset, eocd, hash_name)
        all_ok &= signature_ok and digest_ok
        print(f"  {scheme} algorithm 0x{algorithm:04x}: signature {'OK' if signature_ok else 'FAILED'}, "
              f"content digest {'OK' if digest_ok else 'MISMATCH'}")
    return all_ok


def main(path):
    with open(path, "rb") as handle:
        data = handle.read()
    print(f"file:   {os.path.basename(path)}")
    print(f"sha256: {hashlib.sha256(data).hexdigest()}")
    print(f"size:   {len(data):,} bytes")

    archive = zipfile.ZipFile(path)
    infos = archive.infolist()
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

    eocd = data.rfind(b"PK\x05\x06")
    cd_offset = struct.unpack("<I", data[eocd + 16:eocd + 20])[0]
    if data[cd_offset - 16:cd_offset] != b"APK Sig Block 42":
        print("APK Signing Block: none (v1 only or unsigned)")
        return 1 if not v1_files else 0
    block_size = struct.unpack("<Q", data[cd_offset - 24:cd_offset - 16])[0]
    block_start = cd_offset - block_size - 8
    block = data[block_start + 8:cd_offset - 24]

    verified = []
    with tempfile.TemporaryDirectory() as tmp:
        position = 0
        while position + 12 <= len(block):
            length, block_id = struct.unpack("<QI", block[position:position + 12])
            value = block[position + 12:position + 8 + length]
            position += 8 + length
            if block_id in SOURCE_STAMP_IDS:
                certificate = length_prefixed(value, 0)[0]
                inner = length_prefixed(certificate, 0)[0]
                stamp_hashes = {hashlib.sha256(certificate).hexdigest(), hashlib.sha256(inner).hexdigest()}
                origin = "Google Play" if PLAY_STAMP_SHA256 in stamp_hashes else "unknown stamp"
                print(f"{SOURCE_STAMP_IDS[block_id]}: {origin} (stamp signature itself not verified)")
            scheme = SCHEMES.get(block_id)
            if not scheme:
                continue
            signers, _ = length_prefixed(value, 0)
            offset = 0
            while offset < len(signers):
                signer, offset = length_prefixed(signers, offset)
                verified.append(verify_signer(scheme, signer, data, block_start, cd_offset, eocd, tmp))
    ok = bool(verified) and all(verified)
    print(f"RESULT: {'signatures valid' if ok else 'SIGNATURE PROBLEM'}")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
