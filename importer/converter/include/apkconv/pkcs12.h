// PKCS #12 files whose private key and certificate are stored without encryption: a plain keyBag
// and certBag in data content (RFC 7292), as SideStore 0.6.4 and later writes them for every app it
// installs and for its certificate export (ADR-0023). Security.framework's SecPKCS12Import rejects
// them: they have no MAC, the key sits in a plain keyBag, and the bags carry no localKeyID
// (errSecDecode, measured on iOS 27). Encrypted files stay with SecPKCS12Import. Written from the
// RFC; no code of SideStore, CodeSignKit or OpenSSL.
#pragma once

#include "apkconv/der.h"

#include <span>

namespace apk::pkcs12 {

struct UnencryptedContents {
    der::Bytes rsa_private_key;  // PKCS #1 RSAPrivateKey: what SecKeyCreateWithData takes
    der::Bytes certificate;      // DER of the certificate whose public key belongs to that key
};

/// A key or a part of the file is encrypted: SecPKCS12Import's case, not this reader's.
class EncryptedError : public der::FormatError {
public:
    using der::FormatError::FormatError;
};

/// Reads `pfx`. Throws EncryptedError when the key or a part is encrypted, and der::FormatError
/// when the file is malformed, holds no RSA private key or more than one, or no certificate for
/// the key. A MAC, if there is one, is not checked: nothing it would protect is encrypted.
UnencryptedContents read_unencrypted(std::span<const std::byte> pfx);

}  // namespace apk::pkcs12
