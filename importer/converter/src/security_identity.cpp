#include "apkconv/security_identity.h"

#include <format>
#include <memory>
#include <type_traits>

namespace apk::codesign {
namespace {

/// Owns one reference of a Core Foundation object.
template <typename Reference>
using Owned = std::unique_ptr<std::remove_pointer_t<Reference>, decltype(&CFRelease)>;

template <typename Reference>
Owned<Reference> own(Reference reference) {
    return Owned<Reference>(reference, &CFRelease);
}

/// The text of a Core Foundation error, for exception messages.
std::string describe(CFErrorRef error) {
    if (error == nullptr) return "no reason given";
    const auto text = own(CFErrorCopyDescription(error));
    char buffer[512] = {};
    if (!text || !CFStringGetCString(text.get(), buffer, sizeof buffer, kCFStringEncodingUTF8)) return "unknown error";
    return buffer;
}

der::Bytes bytes_of(CFDataRef data) {
    const auto *begin = reinterpret_cast<const std::byte *>(CFDataGetBytePtr(data));
    return der::Bytes(begin, begin + CFDataGetLength(data));
}

x509::Certificate certificate_of(SecCertificateRef certificate) {
    const auto data = own(SecCertificateCopyData(certificate));
    return x509::parse(bytes_of(data.get()));
}

}  // namespace

Identity identity_with_key(SecKeyRef private_key, std::vector<x509::Certificate> chain) {
    if (chain.empty()) throw FormatError("an identity needs its certificate");
    if (!SecKeyIsAlgorithmSupported(private_key, kSecKeyOperationTypeSign,
                                    kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256)) {
        throw FormatError("the private key cannot make RSA signatures with SHA-256");
    }
    CFRetain(private_key);
    // Shared, so that copies of the Identity keep the key alive.
    const std::shared_ptr<const void> key(private_key, [](const void *held) { CFRelease(held); });
    return Identity{
        .chain = std::move(chain),
        .signature_size = SecKeyGetBlockSize(private_key),
        .sign =
            [key](std::span<const std::byte> message) {
                const auto data = own(CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(message.data()),
                                                   static_cast<CFIndex>(message.size())));
                CFErrorRef error = nullptr;
                const auto signature = own(SecKeyCreateSignature(static_cast<SecKeyRef>(const_cast<void *>(key.get())),
                                                                 kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
                                                                 data.get(), &error));
                if (!signature) {
                    const auto reason = describe(error);
                    if (error != nullptr) CFRelease(error);
                    throw FormatError("the private key did not sign: " + reason);
                }
                return bytes_of(signature.get());
            },
    };
}

Identity identity_from(SecIdentityRef identity, std::span<const der::Bytes> chain_candidates) {
    SecCertificateRef certificate = nullptr;
    if (const auto status = SecIdentityCopyCertificate(identity, &certificate); status != errSecSuccess) {
        throw FormatError(std::format("the identity has no certificate (OSStatus {})", status));
    }
    const auto owned_certificate = own(certificate);
    SecKeyRef key = nullptr;
    if (const auto status = SecIdentityCopyPrivateKey(identity, &key); status != errSecSuccess) {
        throw FormatError(std::format("the identity has no private key (OSStatus {})", status));
    }
    const auto owned_key = own(key);
    return identity_with_key(key, complete_chain(certificate_of(certificate), chain_candidates));
}

Identity identity_from_p12(std::span<const std::byte> p12, const std::string &password,
                           std::span<const der::Bytes> chain_candidates) {
    const auto data = own(CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(p12.data()),
                                       static_cast<CFIndex>(p12.size())));
    const auto passphrase = own(CFStringCreateWithCString(nullptr, password.c_str(), kCFStringEncodingUTF8));
    if (!passphrase) throw FormatError("the password is not UTF-8");
    const void *keys[] = {kSecImportExportPassphrase, kSecImportToMemoryOnly};
    const void *values[] = {passphrase.get(), kCFBooleanTrue};
    const auto options = own(CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks));
    CFArrayRef items = nullptr;
    const auto status = SecPKCS12Import(data.get(), options.get(), &items);
    if (status == errSecAuthFailed) throw FormatError("the password of the .p12 file is wrong");
    if (status != errSecSuccess) throw FormatError(std::format("the .p12 file cannot be read (OSStatus {})", status));
    const auto owned_items = own(items);
    if (CFArrayGetCount(items) == 0) throw FormatError("the .p12 file holds no identity");

    const auto first = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, 0));
    const auto identity = static_cast<SecIdentityRef>(const_cast<void *>(CFDictionaryGetValue(first, kSecImportItemIdentity)));
    if (identity == nullptr) throw FormatError("the .p12 file holds no identity");
    std::vector<der::Bytes> candidates(chain_candidates.begin(), chain_candidates.end());
    if (const auto chain = static_cast<CFArrayRef>(CFDictionaryGetValue(first, kSecImportItemCertChain))) {
        for (CFIndex index = 0; index < CFArrayGetCount(chain); ++index) {
            const auto certificate = static_cast<SecCertificateRef>(const_cast<void *>(CFArrayGetValueAtIndex(chain, index)));
            candidates.push_back(certificate_of(certificate).encoding);
        }
    }
    return identity_from(identity, candidates);
}

#if TARGET_OS_OSX
der::Bytes keychain_identity_as_p12(const x509::Certificate &certificate, const std::string &password) {
    // All identities, compared by their certificate: exact, unlike a search by name.
    const void *keys[] = {kSecClass, kSecMatchLimit, kSecReturnRef};
    const void *values[] = {kSecClassIdentity, kSecMatchLimitAll, kCFBooleanTrue};
    const auto query = own(CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks));
    CFTypeRef found = nullptr;
    const auto status = SecItemCopyMatching(query.get(), &found);
    if (status != errSecSuccess && status != errSecItemNotFound) {
        throw FormatError(std::format("the keychain cannot be searched (OSStatus {})", status));
    }
    const auto owned_found = own(found);
    SecIdentityRef match = nullptr;
    if (found != nullptr && CFGetTypeID(found) == CFArrayGetTypeID()) {
        const auto identities = static_cast<CFArrayRef>(found);
        for (CFIndex index = 0; index < CFArrayGetCount(identities) && match == nullptr; ++index) {
            const auto identity = static_cast<SecIdentityRef>(const_cast<void *>(CFArrayGetValueAtIndex(identities, index)));
            SecCertificateRef candidate = nullptr;
            if (SecIdentityCopyCertificate(identity, &candidate) != errSecSuccess) continue;
            const auto owned_candidate = own(candidate);
            const auto data = own(SecCertificateCopyData(candidate));
            if (bytes_of(data.get()) == certificate.encoding) match = identity;
        }
    }
    if (match == nullptr) {
        throw FormatError(std::format("no identity in the keychain has the certificate \"{}\" with its private key",
                                      certificate.common_name));
    }

    const auto passphrase = own(CFStringCreateWithCString(nullptr, password.c_str(), kCFStringEncodingUTF8));
    if (!passphrase) throw FormatError("the password is not UTF-8");
    SecItemImportExportKeyParameters parameters{};
    parameters.version = SEC_KEY_IMPORT_EXPORT_PARAMS_VERSION;
    parameters.passphrase = passphrase.get();
    CFDataRef exported = nullptr;
    if (const auto exporting = SecItemExport(match, kSecFormatPKCS12, 0, &parameters, &exported);
        exporting != errSecSuccess) {
        const bool refused = exporting == errSecUserCanceled || exporting == errSecAuthFailed;
        throw FormatError(std::format("the keychain did not export the identity{} (OSStatus {})",
                                      refused ? ": the export was not allowed" : "", exporting));
    }
    const auto owned_exported = own(exported);
    return bytes_of(exported);
}
#endif

}  // namespace apk::codesign
