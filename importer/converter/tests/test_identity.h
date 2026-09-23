// A throwaway certificate chain for the signing tests, made in memory: a root, an intermediate
// and a leaf with a team ID, like Apple Root CA → WWDR → Apple Development. One RSA key signs all
// three and is the leaf's private key; the tests only need signatures that verify, not trust.
#pragma once

#include "apkconv/certificate.h"
#include "apkconv/der.h"
#include "apkconv/security_identity.h"

#include <Security/Security.h>

#include <chrono>
#include <stdexcept>
#include <string>

namespace apk::testing {

inline constexpr char test_team[] = "TESTTEAM01";
inline constexpr char test_leaf_name[] = "APK-Runner Test Leaf";
inline constexpr char test_intermediate_name[] = "APK-Runner Test CA";
inline constexpr char test_root_name[] = "APK-Runner Test Root";

struct TestChain {
    SecKeyRef key = nullptr;  // owned, never released: the tests end with the process
    x509::Certificate root;
    x509::Certificate intermediate;
    x509::Certificate leaf;
    codesign::Identity identity;  // the leaf with its chain
};

inline der::Bytes test_name(const std::string &common_name, const std::string &unit = "") {
    std::vector<der::Bytes> attributes;
    const auto pair = [](const char *oid, const der::Bytes &value) {
        return der::set_of({der::sequence({der::object_identifier(oid), value})});
    };
    const auto country = pair("2.5.4.6", der::printable_string("US"));
    const auto organization = pair("2.5.4.10", der::utf8_string("APK-Runner Tests"));
    const auto name = pair("2.5.4.3", der::utf8_string(common_name));
    if (unit.empty()) return der::sequence({country, organization, name});
    return der::sequence({country, organization, pair("2.5.4.11", der::utf8_string(unit)), name});
}

inline der::Bytes signed_by(SecKeyRef key, const der::Bytes &to_be_signed) {
    const auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(to_be_signed.data()),
                                   static_cast<CFIndex>(to_be_signed.size()));
    CFErrorRef error = nullptr;
    const auto signature = SecKeyCreateSignature(key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, data, &error);
    CFRelease(data);
    if (signature == nullptr) throw std::runtime_error("SecKeyCreateSignature failed");
    const auto *begin = reinterpret_cast<const std::byte *>(CFDataGetBytePtr(signature));
    der::Bytes result(begin, begin + CFDataGetLength(signature));
    CFRelease(signature);
    return result;
}

/// An X.509 v3 certificate for `subject`, issued by `issuer` and signed with `key`.
inline x509::Certificate test_certificate(SecKeyRef key, std::uint64_t serial, const der::Bytes &issuer,
                                          const der::Bytes &subject, bool authority) {
    using namespace std::chrono;
    const auto public_key = SecKeyCopyPublicKey(key);
    const auto exported = SecKeyCopyExternalRepresentation(public_key, nullptr);  // PKCS #1 RSAPublicKey
    const auto *begin = reinterpret_cast<const std::byte *>(CFDataGetBytePtr(exported));
    const der::Bytes rsa_public_key(begin, begin + CFDataGetLength(exported));
    CFRelease(exported);
    CFRelease(public_key);

    const auto algorithm = der::sequence({der::object_identifier("1.2.840.113549.1.1.11"), der::null()});
    const auto key_info = der::sequence(
        {der::sequence({der::object_identifier("1.2.840.113549.1.1.1"), der::null()}), der::bit_string(rsa_public_key)});
    // basicConstraints, critical, cA for the root and the intermediate. The leaf may sign code, as
    // an Apple Development certificate: keyUsage digitalSignature and extKeyUsage codeSigning.
    const auto critical = der::element(0x01, der::bytes_of("\xff"));
    const auto constraints = der::sequence({der::object_identifier("2.5.29.19"), critical,
                                            der::octet_string(der::sequence({authority ? critical : der::Bytes{}}))});
    const auto key_usage = der::sequence({der::object_identifier("2.5.29.15"), critical,
                                          der::octet_string(der::element(der::tag_bit_string, der::bytes_of("\x07\x80")))});
    const auto code_signing = der::sequence({der::object_identifier("2.5.29.37"), critical,
                                             der::octet_string(der::sequence({der::object_identifier("1.3.6.1.5.5.7.3.3")}))});
    const auto extensions = authority ? der::sequence({constraints}) : der::sequence({constraints, key_usage, code_signing});
    const sys_seconds from = sys_days{year{2026} / 1 / 1};
    const sys_seconds until = sys_days{year{2036} / 1 / 1};
    const auto to_be_signed = der::sequence({
        der::element(der::context(0), der::integer(2)),
        der::integer(serial),
        algorithm,
        issuer,
        der::sequence({der::utc_time(from), der::utc_time(until)}),
        subject,
        key_info,
        der::element(der::context(3), extensions),
    });
    return x509::parse(der::sequence({to_be_signed, algorithm, der::bit_string(signed_by(key, to_be_signed))}));
}

inline TestChain make_test_chain() {
    const void *keys[] = {kSecAttrKeyType, kSecAttrKeySizeInBits};
    const int bits = 2048;
    const auto size = CFNumberCreate(nullptr, kCFNumberIntType, &bits);
    const void *values[] = {kSecAttrKeyTypeRSA, size};
    const auto parameters = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFErrorRef error = nullptr;
    SecKeyRef key = SecKeyCreateRandomKey(parameters, &error);
    CFRelease(parameters);
    CFRelease(size);
    if (key == nullptr) throw std::runtime_error("SecKeyCreateRandomKey failed");

    const auto root_name = test_name(test_root_name);
    const auto intermediate_name = test_name(test_intermediate_name);
    const auto leaf_name = test_name(test_leaf_name, test_team);
    TestChain chain;
    chain.key = key;
    chain.root = test_certificate(key, 1, root_name, root_name, true);
    chain.intermediate = test_certificate(key, 0x1234'5678'9abc, root_name, intermediate_name, true);
    chain.leaf = test_certificate(key, 0x7b38'09b4'5860'fdf9, intermediate_name, leaf_name, false);
    chain.identity = codesign::identity_with_key(key, {chain.leaf, chain.intermediate, chain.root});
    return chain;
}

}  // namespace apk::testing
