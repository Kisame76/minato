// Signing identities whose private key Security.framework holds (ADR-0022): on the iPhone the one
// imported into the app's keychain, on the Mac one from a .p12 file for the tools and tests. The
// key never leaves Security.framework; the signer only asks it to sign. The one exception is on the
// Mac: tools/hand-over-certificate.sh exports the identity Xcode signs the app with, to put it into
// the app's keychain on the iPhone.
#pragma once

#include "apkconv/codesign.h"

#include <Security/Security.h>
#include <TargetConditionals.h>

#include <span>
#include <string>

namespace apk::codesign {

/// An identity that signs with `private_key` (RSA) for the certificates `chain`, leaf first.
/// Throws FormatError when the key is not an RSA key that can sign.
Identity identity_with_key(SecKeyRef private_key, std::vector<x509::Certificate> chain);

/// The identity `identity`, e.g. from the keychain, with its chain completed from
/// `chain_candidates` (complete_chain). Throws FormatError when that fails.
Identity identity_from(SecIdentityRef identity, std::span<const der::Bytes> chain_candidates);

/// The first identity in the PKCS #12 file `p12`, imported in memory only and never into a
/// keychain. Its chain is completed from the certificates in the file and from `chain_candidates`.
/// Throws FormatError when the file or the password is wrong or the chain is incomplete.
Identity identity_from_p12(std::span<const std::byte> p12, const std::string &password,
                           std::span<const der::Bytes> chain_candidates);

#if TARGET_OS_OSX
/// The identity in the Mac's keychains whose certificate is `certificate`, with its private key, as
/// a PKCS #12 file encrypted with `password`: the form in which the app on the iPhone imports it.
/// macOS asks the user whether to allow the export. Throws FormatError when no identity has that
/// certificate or the export fails or is not allowed.
der::Bytes keychain_identity_as_p12(const x509::Certificate &certificate, const std::string &password);
#endif

}  // namespace apk::codesign
