#include "apkconv/pkcs12.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>

namespace apk::pkcs12 {
namespace {

using der::FormatError;

constexpr std::string_view data_oid = "1.2.840.113549.1.7.1";
constexpr std::string_view signed_data_oid = "1.2.840.113549.1.7.2";
constexpr std::string_view encrypted_data_oid = "1.2.840.113549.1.7.6";
constexpr std::string_view key_bag_oid = "1.2.840.113549.1.12.10.1.1";
constexpr std::string_view shrouded_key_bag_oid = "1.2.840.113549.1.12.10.1.2";
constexpr std::string_view cert_bag_oid = "1.2.840.113549.1.12.10.1.3";
constexpr std::string_view safe_contents_bag_oid = "1.2.840.113549.1.12.10.1.6";
constexpr std::string_view x509_certificate_oid = "1.2.840.113549.1.9.22.1";
constexpr std::string_view rsa_encryption_oid = "1.2.840.113549.1.1.1";

/// SafeContents may hold SafeContents; more than this is not a real file.
constexpr int maximum_nesting = 4;

struct Found {
    std::vector<der::Bytes> private_keys;  // PKCS #1
    std::vector<der::Bytes> certificates;
};

der::Bytes copy(std::span<const std::byte> bytes) {
    return {bytes.begin(), bytes.end()};
}

/// `bytes` read as exactly one element.
der::Element whole(std::span<const std::byte> bytes, std::uint8_t tag, const char *what) {
    const auto element = der::read(bytes);
    if (element.tag != tag || element.encoding.size() != bytes.size()) {
        throw FormatError(std::format("PKCS #12: {} is not one element of the expected type", what));
    }
    return element;
}

/// The contents of a [0] EXPLICIT OCTET STRING, the form of ContentInfo's content.
std::span<const std::byte> explicit_octets(const der::Element &wrapper, const char *what) {
    if (wrapper.tag != der::context(0)) throw FormatError(std::format("PKCS #12: {} has no [0] content", what));
    const auto inner = der::children(wrapper);
    if (inner.size() != 1 || inner[0].tag != der::tag_octet_string) {
        throw FormatError(std::format("PKCS #12: {} does not hold one OCTET STRING", what));
    }
    return inner[0].contents;
}

/// An INTEGER's magnitude without the leading zero byte DER adds to keep it positive.
std::span<const std::byte> magnitude(const der::Element &integer) {
    auto bytes = integer.contents;
    while (bytes.size() > 1 && bytes.front() == std::byte{0}) bytes = bytes.subspan(1);
    return bytes;
}

/// PrivateKeyInfo (PKCS #8) → the RSAPrivateKey (PKCS #1) inside it.
der::Bytes rsa_key_from_pkcs8(const der::Element &info) {
    if (info.tag != der::tag_sequence) throw FormatError("PKCS #12: the key bag is not PKCS #8");
    const auto fields = der::children(info);
    if (fields.size() < 3 || fields[0].tag != der::tag_integer || fields[1].tag != der::tag_sequence ||
        fields[2].tag != der::tag_octet_string) {
        throw FormatError("PKCS #12: the key bag is not PKCS #8");
    }
    const auto algorithm = der::children(fields[1]);
    if (algorithm.empty() || algorithm[0].tag != der::tag_object_identifier) {
        throw FormatError("PKCS #12: the key's algorithm is unreadable");
    }
    if (der::object_identifier_text(algorithm[0]) != rsa_encryption_oid) {
        throw FormatError("PKCS #12: the private key is not an RSA key");
    }
    // RSAPrivateKey: version, modulus, publicExponent, privateExponent, prime1, prime2, exponent1,
    // exponent2, coefficient.
    const auto key = whole(fields[2].contents, der::tag_sequence, "the RSA key");
    const auto parts = der::children(key);
    if (parts.size() < 9 || !std::all_of(parts.begin(), parts.begin() + 9,
                                         [](const der::Element &part) { return part.tag == der::tag_integer; })) {
        throw FormatError("PKCS #12: the RSA key is incomplete");
    }
    return copy(fields[2].contents);
}

/// CertBag → the X.509 certificate, or nothing for other certificate types.
std::optional<der::Bytes> certificate_from_bag(const der::Element &bag) {
    if (bag.tag != der::tag_sequence) throw FormatError("PKCS #12: a certificate bag is not a sequence");
    const auto fields = der::children(bag);
    if (fields.size() != 2 || fields[0].tag != der::tag_object_identifier) {
        throw FormatError("PKCS #12: a certificate bag has an unexpected form");
    }
    if (der::object_identifier_text(fields[0]) != x509_certificate_oid) return std::nullopt;
    return copy(explicit_octets(fields[1], "a certificate bag"));
}

void read_safe_contents(std::span<const std::byte> encoded, Found &found, int depth);

void read_bag(const der::Element &bag, Found &found, int depth) {
    if (bag.tag != der::tag_sequence) throw FormatError("PKCS #12: a safe bag is not a sequence");
    const auto fields = der::children(bag);
    if (fields.size() < 2 || fields.size() > 3 || fields[0].tag != der::tag_object_identifier ||
        fields[1].tag != der::context(0)) {
        throw FormatError("PKCS #12: a safe bag has an unexpected form");
    }
    const auto values = der::children(fields[1]);
    if (values.size() != 1) throw FormatError("PKCS #12: a safe bag does not hold one value");
    const auto type = der::object_identifier_text(fields[0]);
    if (type == key_bag_oid) {
        found.private_keys.push_back(rsa_key_from_pkcs8(values[0]));
    } else if (type == shrouded_key_bag_oid) {
        throw EncryptedError("PKCS #12: the private key is encrypted");
    } else if (type == cert_bag_oid) {
        if (auto certificate = certificate_from_bag(values[0])) found.certificates.push_back(std::move(*certificate));
    } else if (type == safe_contents_bag_oid) {
        read_safe_contents(values[0].encoding, found, depth + 1);
    }
    // CRL and secret bags do not matter for signing.
}

void read_safe_contents(std::span<const std::byte> encoded, Found &found, int depth) {
    if (depth > maximum_nesting) throw FormatError("PKCS #12: safe contents nest too deeply");
    for (const auto &bag : der::children(whole(encoded, der::tag_sequence, "a safe contents"))) {
        read_bag(bag, found, depth);
    }
}

/// One ContentInfo of the AuthenticatedSafe.
void read_part(const der::Element &info, Found &found) {
    if (info.tag != der::tag_sequence) throw FormatError("PKCS #12: a part is not a sequence");
    const auto fields = der::children(info);
    if (fields.empty() || fields[0].tag != der::tag_object_identifier) throw FormatError("PKCS #12: a part has no type");
    const auto type = der::object_identifier_text(fields[0]);
    if (type == encrypted_data_oid) throw EncryptedError("PKCS #12: a part is encrypted");
    if (type != data_oid) throw FormatError(std::format("PKCS #12: a part has the unsupported type {}", type));
    if (fields.size() != 2) throw FormatError("PKCS #12: a data part has no content");
    read_safe_contents(explicit_octets(fields[1], "a data part"), found, 0);
}

/// The RSA modulus in a certificate's subjectPublicKeyInfo; nothing when it holds another kind of key.
std::optional<der::Bytes> certificate_modulus(const der::Bytes &certificate) {
    const auto fields = der::children(whole(certificate, der::tag_sequence, "a certificate"));
    if (fields.empty() || fields[0].tag != der::tag_sequence) throw FormatError("PKCS #12: a certificate is not X.509");
    // tbsCertificate: [0] version (optional), serialNumber, signature, issuer, validity, subject,
    // subjectPublicKeyInfo, ...
    const auto tbs = der::children(fields[0]);
    const std::size_t at = !tbs.empty() && tbs[0].tag == der::context(0) ? 1 : 0;
    if (tbs.size() < at + 6 || tbs[at + 5].tag != der::tag_sequence) {
        throw FormatError("PKCS #12: a certificate has no public key");
    }
    const auto key_info = der::children(tbs[at + 5]);
    if (key_info.size() != 2 || key_info[0].tag != der::tag_sequence || key_info[1].tag != der::tag_bit_string) {
        throw FormatError("PKCS #12: a certificate's public key is unreadable");
    }
    const auto algorithm = der::children(key_info[0]);
    if (algorithm.empty() || algorithm[0].tag != der::tag_object_identifier ||
        der::object_identifier_text(algorithm[0]) != rsa_encryption_oid) {
        return std::nullopt;
    }
    // The BIT STRING starts with the number of unused bits, then holds RSAPublicKey.
    const auto bits = key_info[1].contents;
    if (bits.empty() || bits.front() != std::byte{0}) throw FormatError("PKCS #12: a certificate's RSA key is unreadable");
    const auto public_key = der::children(whole(bits.subspan(1), der::tag_sequence, "a certificate's RSA key"));
    if (public_key.size() != 2 || public_key[0].tag != der::tag_integer) {
        throw FormatError("PKCS #12: a certificate's RSA key is unreadable");
    }
    return copy(magnitude(public_key[0]));
}

}  // namespace

UnencryptedContents read_unencrypted(std::span<const std::byte> pfx) {
    const auto top = der::read(pfx);
    if (top.tag != der::tag_sequence || top.encoding.size() != pfx.size()) {
        throw FormatError("not a PKCS #12 file: no single sequence");
    }
    const auto fields = der::children(top);
    if (fields.size() < 2 || fields.size() > 3 || fields[0].tag != der::tag_integer || fields[1].tag != der::tag_sequence) {
        throw FormatError("not a PKCS #12 file: unexpected fields");
    }
    if (fields[0].contents.size() != 1 || fields[0].contents[0] != std::byte{3}) {
        throw FormatError("not a PKCS #12 file: the version is not 3");
    }
    const auto authenticated_safe = der::children(fields[1]);
    if (authenticated_safe.size() != 2 || authenticated_safe[0].tag != der::tag_object_identifier) {
        throw FormatError("PKCS #12: the authenticated safe is unreadable");
    }
    const auto type = der::object_identifier_text(authenticated_safe[0]);
    if (type == signed_data_oid) throw FormatError("PKCS #12: public-key protected files are not supported");
    if (type != data_oid) throw FormatError("PKCS #12: the authenticated safe is not data");

    Found found;
    const auto parts = whole(explicit_octets(authenticated_safe[1], "the authenticated safe"), der::tag_sequence,
                             "the authenticated safe");
    for (const auto &part : der::children(parts)) read_part(part, found);

    if (found.private_keys.empty()) throw FormatError("PKCS #12: no private key");
    if (found.private_keys.size() > 1) throw FormatError("PKCS #12: more than one private key");
    auto &key = found.private_keys.front();
    const auto modulus = magnitude(der::children(der::read(key))[1]);
    for (auto &certificate : found.certificates) {
        const auto candidate = certificate_modulus(certificate);
        if (candidate && std::ranges::equal(*candidate, modulus)) return {std::move(key), std::move(certificate)};
    }
    throw FormatError("PKCS #12: no certificate for the private key");
}

}  // namespace apk::pkcs12
