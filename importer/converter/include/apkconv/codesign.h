// Code signatures for container dylibs, written the way `codesign` writes them (ADR-0008,
// ADR-0022), so that the converter can sign on the iPhone, where there is no codesign:
//   - a SHA-256 CodeDirectory (version 0x20400) over 16 KiB pages, with the execution segment
//     and, for a certificate, the team ID;
//   - the requirements: none for an ad-hoc signature, otherwise the designated requirement codesign
//     derives for an Apple Development certificate;
//   - the CMS signature: detached SignedData over the CodeDirectory, with the certificate chain and
//     the signed attributes Apple's code signing uses (content type, signing time, message digest,
//     and the CDHashes in both forms, as a plist and as hash type and value).
// An ad-hoc signature comes out byte for byte like codesign's, which the tests check.
#pragma once

#include "apkconv/certificate.h"
#include "apkconv/der.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace apk::codesign {

/// A signing certificate and the means to sign with its private key, which stays with its owner
/// (on Apple platforms, Security.framework: security_identity.h).
struct Identity {
    /// The certificate chain, leaf first, up to its root. All of it goes into the signature, as
    /// codesign does it: iOS evaluates the chain from what the signature carries.
    std::vector<x509::Certificate> chain;
    /// The size of a signature in bytes: the size of the RSA key's modulus.
    std::size_t signature_size = 0;
    /// Signs `message` with the private key: RSA PKCS #1 v1.5 with SHA-256.
    std::function<der::Bytes(std::span<const std::byte> message)> sign;
};

struct Options {
    /// The signing identifier. On iOS it must be the app's bundle ID for a dylib outside the
    /// bundle to load (ADR-0008, spike S1b).
    std::string identifier;
    /// nullptr for an ad-hoc signature (macOS and the Simulator).
    const Identity *identity = nullptr;
    /// The signing time in the CMS signature; the current time when left at zero.
    std::chrono::sys_seconds signing_time{};
};

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Signs `image`, a thin arm64 Mach-O whose __LINKEDIT is its last segment and ends at the end of
/// the file, as `codesign --force --sign <identity> --identifier <identifier> --timestamp=none`
/// would. An existing signature is replaced. Without one, LC_CODE_SIGNATURE goes behind the load
/// commands, where the container keeps room for it (macho::signature_headroom). Throws FormatError
/// when the image cannot be signed this way.
void sign(std::vector<std::byte> &image, const Options &options);

/// What a code signature holds, as far as the signer needs to know it about other files.
struct Signature {
    std::string identifier;
    std::string team;     // empty for an ad-hoc signature
    std::uint32_t flags = 0;
    std::vector<der::Bytes> certificates;  // from the CMS signature, in its order; none when ad hoc
    /// The certificate that made the CMS signature, the leaf of the chain: the one its SignerInfo
    /// names by issuer and serial number. Empty when ad hoc. For the app's own executable it is the
    /// certificate the app was installed with, which iOS accepts for converted code (ADR-0008).
    der::Bytes leaf;
    der::Bytes code_directory;             // the first CodeDirectory blob
    der::Bytes requirements;               // the requirements blob
    der::Bytes cms;                        // the CMS signature, empty when ad hoc
};

/// Reads the code signature of a thin arm64 Mach-O, or of the arm64 slice of a universal one:
/// e.g. of the app's own executable, which carries the certificate chain Xcode or SideStore signed
/// it with. Throws FormatError when there is none or it is malformed.
Signature read_signature(std::span<const std::byte> image);

/// Completes the chain of `leaf` from `candidates`, e.g. the certificates of the app's own
/// signature: the issuer of each certificate follows it, up to a self-issued root. Throws
/// FormatError when an issuer is missing.
std::vector<x509::Certificate> complete_chain(const x509::Certificate &leaf,
                                              std::span<const der::Bytes> candidates);

}  // namespace apk::codesign
