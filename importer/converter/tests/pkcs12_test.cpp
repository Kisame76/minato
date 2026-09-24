// Checks the reader for unencrypted PKCS #12 files (ADR-0023) on files built here from RFC 7292:
// the layout SideStore 0.6.4 and later writes, other orders and extras, encrypted parts it must
// hand back to SecPKCS12Import, and broken input.
#include "apkconv/pkcs12.h"

#include "APKConvert/apkconv.h"

#include "check.h"
#include "test_identity.h"

#include <Security/Security.h>

#include <algorithm>
#include <random>

namespace {

using namespace apk;

const char *const data_oid = "1.2.840.113549.1.7.1";
const char *const encrypted_data_oid = "1.2.840.113549.1.7.6";
const char *const key_bag = "1.2.840.113549.1.12.10.1.1";
const char *const shrouded_key_bag = "1.2.840.113549.1.12.10.1.2";
const char *const cert_bag = "1.2.840.113549.1.12.10.1.3";
const char *const x509_certificate = "1.2.840.113549.1.9.22.1";

der::Bytes joined(const std::vector<der::Bytes> &parts) {
    der::Bytes all;
    for (const auto &part : parts) all.insert(all.end(), part.begin(), part.end());
    return all;
}

/// ContentInfo of type `type` whose [0] EXPLICIT OCTET STRING holds `inner`.
der::Bytes content_info(const char *type, const der::Bytes &inner) {
    return der::sequence({der::object_identifier(type), der::element(der::context(0), der::octet_string(inner))});
}

der::Bytes safe_bag(const char *type, const der::Bytes &value, const der::Bytes &attributes = {}) {
    if (attributes.empty()) return der::sequence({der::object_identifier(type), der::element(der::context(0), value)});
    return der::sequence({der::object_identifier(type), der::element(der::context(0), value), attributes});
}

der::Bytes safe_contents(const std::vector<der::Bytes> &bags) {
    return der::element(der::tag_sequence, joined(bags));
}

/// PrivateKeyInfo (PKCS #8) around `private_key` for the algorithm `algorithm`.
der::Bytes pkcs8(const der::Bytes &private_key, const char *algorithm = "1.2.840.113549.1.1.1") {
    return der::sequence({der::integer(0), der::sequence({der::object_identifier(algorithm), der::null()}),
                          der::octet_string(private_key)});
}

der::Bytes certificate_bag(const der::Bytes &certificate, const der::Bytes &attributes = {}) {
    return safe_bag(cert_bag,
                    der::sequence({der::object_identifier(x509_certificate),
                                   der::element(der::context(0), der::octet_string(certificate))}),
                    attributes);
}

/// PFX v3 whose AuthenticatedSafe holds `infos`, with `mac` if given.
der::Bytes pfx(const std::vector<der::Bytes> &infos, const der::Bytes &mac = {}) {
    const auto authenticated_safe = content_info(data_oid, der::element(der::tag_sequence, joined(infos)));
    if (mac.empty()) return der::sequence({der::integer(3), authenticated_safe});
    return der::sequence({der::integer(3), authenticated_safe, mac});
}

/// A localKeyID attribute set, as OpenSSL writes one.
der::Bytes local_key_id() {
    return der::set_of({der::sequence({der::object_identifier("1.2.840.113549.1.9.21"),
                                       der::set_of({der::octet_string(der::Bytes(20, std::byte{0x11}))})})});
}

/// The RSA private key of `key` as PKCS #1, what the reader must hand back.
der::Bytes pkcs1(SecKeyRef key) {
    const auto exported = SecKeyCopyExternalRepresentation(key, nullptr);
    const auto *begin = reinterpret_cast<const std::byte *>(CFDataGetBytePtr(exported));
    der::Bytes bytes(begin, begin + CFDataGetLength(exported));
    CFRelease(exported);
    return bytes;
}

const testing::TestChain &chain() {
    static const auto made = testing::make_test_chain();
    return made;
}

/// Another identity: its certificates carry a public key that does not belong to chain().key.
const testing::TestChain &foreign() {
    static const auto made = testing::make_test_chain();
    return made;
}

/// The layout SideStore 0.6.4 and later writes: key part, then certificate part, no MAC, no attributes.
der::Bytes sidestore_layout() {
    return pfx({content_info(data_oid, safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)))})),
                content_info(data_oid, safe_contents({certificate_bag(chain().leaf.encoding)}))});
}

bool reads_the_chain_leaf(const der::Bytes &file) {
    const auto contents = pkcs12::read_unencrypted(file);
    return contents.rsa_private_key == pkcs1(chain().key) && contents.certificate == chain().leaf.encoding;
}

TEST(reads_the_layout_sidestore_writes) {
    CHECK(reads_the_chain_leaf(sidestore_layout()));
}

TEST(reads_the_certificate_part_before_the_key_part) {
    CHECK(reads_the_chain_leaf(pfx({content_info(data_oid, safe_contents({certificate_bag(chain().leaf.encoding)})),
                                    content_info(data_oid, safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)))}))})));
}

TEST(reads_both_bags_in_one_part_with_attributes_and_a_mac) {
    // OpenSSL's "-keypbe NONE -certpbe NONE" layout: localKeyID on both bags and a MAC, which the
    // reader does not need to check because nothing is encrypted.
    const auto mac = der::sequence({der::sequence({der::sequence({der::object_identifier("2.16.840.1.101.3.4.2.1"), der::null()}),
                                                   der::octet_string(der::Bytes(32))}),
                                    der::octet_string(der::Bytes(8)), der::integer(2048)});
    CHECK(reads_the_chain_leaf(pfx({content_info(data_oid,
                                                 safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)), local_key_id()),
                                                                certificate_bag(chain().leaf.encoding, local_key_id())}))},
                                   mac)));
}

TEST(picks_the_certificate_that_belongs_to_the_key) {
    CHECK(reads_the_chain_leaf(pfx({content_info(data_oid, safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)))})),
                                    content_info(data_oid, safe_contents({certificate_bag(foreign().leaf.encoding),
                                                                          certificate_bag(chain().leaf.encoding)}))})));
}

TEST(reads_bags_nested_in_a_safe_contents_bag) {
    const char *const safe_contents_bag = "1.2.840.113549.1.12.10.1.6";
    const auto nested = safe_bag(safe_contents_bag, safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)))}));
    CHECK(reads_the_chain_leaf(pfx({content_info(data_oid, safe_contents({nested})),
                                    content_info(data_oid, safe_contents({certificate_bag(chain().leaf.encoding)}))})));
}

TEST(stops_at_safe_contents_nested_too_deeply) {
    const char *const safe_contents_bag = "1.2.840.113549.1.12.10.1.6";
    auto bags = safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)))});
    for (int depth = 0; depth < 5; ++depth) bags = safe_contents({safe_bag(safe_contents_bag, bags)});
    const auto file = pfx({content_info(data_oid, bags),
                           content_info(data_oid, safe_contents({certificate_bag(chain().leaf.encoding)}))});
    CHECK_THROWS(der::FormatError, pkcs12::read_unencrypted(file), "nest too deeply");
}

TEST(skips_certificates_without_an_rsa_key) {
    // A certificate whose public key is EC: the reader passes over it and finds the leaf.
    auto ec = chain().leaf.encoding;
    const auto rsa_oid = der::object_identifier("1.2.840.113549.1.1.1");
    const auto ec_oid = der::object_identifier("1.2.840.10045.2.1");
    // The subjectPublicKeyInfo's algorithm is the only rsaEncryption OID that follows the subject
    // name; the signature algorithm is sha256WithRSAEncryption, another OID of the same length.
    const auto at = std::search(ec.begin(), ec.end(), rsa_oid.begin(), rsa_oid.end());
    CHECK(at != ec.end() && ec_oid.size() == rsa_oid.size() - 2);
    // Keep the length: an EC OID two bytes shorter, padded with a NULL into the same element.
    der::Bytes replacement = ec_oid;
    replacement.push_back(std::byte{0x05});
    replacement.push_back(std::byte{0x00});
    std::copy(replacement.begin(), replacement.end(), at);
    CHECK(reads_the_chain_leaf(pfx({content_info(data_oid, safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)))})),
                                    content_info(data_oid, safe_contents({certificate_bag(ec),
                                                                          certificate_bag(chain().leaf.encoding)}))})));
}

TEST(rejects_a_file_without_a_certificate_for_the_key) {
    const auto file = pfx({content_info(data_oid, safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)))})),
                           content_info(data_oid, safe_contents({certificate_bag(foreign().leaf.encoding)}))});
    CHECK_THROWS(der::FormatError, pkcs12::read_unencrypted(file), "no certificate for the private key");
}

TEST(hands_encrypted_keys_back) {
    const auto shrouded = pfx({content_info(data_oid, safe_contents({safe_bag(shrouded_key_bag, der::sequence({der::null()}))})),
                               content_info(data_oid, safe_contents({certificate_bag(chain().leaf.encoding)}))});
    CHECK_THROWS(pkcs12::EncryptedError, pkcs12::read_unencrypted(shrouded), "encrypted");
    const auto encrypted = pfx({content_info(data_oid, safe_contents({safe_bag(key_bag, pkcs8(pkcs1(chain().key)))})),
                                content_info(encrypted_data_oid, der::sequence({der::integer(0)}))});
    CHECK_THROWS(pkcs12::EncryptedError, pkcs12::read_unencrypted(encrypted), "encrypted");
}

TEST(rejects_keys_that_are_not_rsa) {
    const auto file = pfx({content_info(data_oid, safe_contents({safe_bag(key_bag, pkcs8(der::sequence({der::integer(1)}), "1.2.840.10045.2.1"))})),
                           content_info(data_oid, safe_contents({certificate_bag(chain().leaf.encoding)}))});
    CHECK_THROWS(der::FormatError, pkcs12::read_unencrypted(file), "not an RSA key");
}

TEST(needs_exactly_one_private_key) {
    const auto key = safe_bag(key_bag, pkcs8(pkcs1(chain().key)));
    const auto certificate = content_info(data_oid, safe_contents({certificate_bag(chain().leaf.encoding)}));
    CHECK_THROWS(der::FormatError, pkcs12::read_unencrypted(pfx({content_info(data_oid, safe_contents({key, key})), certificate})),
                 "more than one private key");
    CHECK_THROWS(der::FormatError, pkcs12::read_unencrypted(pfx({certificate})), "no private key");
}

TEST(rejects_files_that_are_not_pkcs12) {
    CHECK_THROWS(der::FormatError, pkcs12::read_unencrypted(der::sequence({der::integer(2)})), "PKCS #12");
    const auto signed_data = der::sequence({der::integer(3), content_info("1.2.840.113549.1.7.2", der::sequence({}))});
    CHECK_THROWS(der::FormatError, pkcs12::read_unencrypted(signed_data), "public-key protected");
}

TEST(the_c_interface_hands_over_key_and_certificate) {
    const auto file = sidestore_layout();
    der::Bytes key(4096), certificate(4096);
    std::size_t key_length = 0, certificate_length = 0;
    char error[256] = "";
    CHECK(apkconv_read_unencrypted_pkcs12(file.data(), file.size(), key.data(), key.size(), &key_length, certificate.data(),
                                          certificate.size(), &certificate_length, error, sizeof error) == 0);
    key.resize(key_length);
    certificate.resize(certificate_length);
    CHECK(key == pkcs1(chain().key));
    CHECK(certificate == chain().leaf.encoding);
}

TEST(the_c_interface_says_how_large_a_buffer_it_needs) {
    const auto file = sidestore_layout();
    const auto needed = pkcs1(chain().key).size();
    der::Bytes key(needed - 1), certificate(4096);
    std::size_t key_length = 0, certificate_length = 0;
    char error[256] = "";
    CHECK(apkconv_read_unencrypted_pkcs12(file.data(), file.size(), key.data(), key.size(), &key_length, certificate.data(),
                                          certificate.size(), &certificate_length, error, sizeof error) == -1);
    CHECK(key_length == needed);
    CHECK(error[0] != '\0');
}

TEST(the_c_interface_hands_encrypted_files_back) {
    const auto shrouded = pfx({content_info(data_oid, safe_contents({safe_bag(shrouded_key_bag, der::sequence({der::null()}))})),
                               content_info(data_oid, safe_contents({certificate_bag(chain().leaf.encoding)}))});
    der::Bytes key(4096), certificate(4096);
    std::size_t key_length = 0, certificate_length = 0;
    char error[256] = "";
    CHECK(apkconv_read_unencrypted_pkcs12(shrouded.data(), shrouded.size(), key.data(), key.size(), &key_length,
                                          certificate.data(), certificate.size(), &certificate_length, error,
                                          sizeof error) == 1);
}

TEST(every_truncation_is_rejected) {
    const auto file = sidestore_layout();
    for (std::size_t length = 0; length < file.size(); ++length) {
        const std::span<const std::byte> prefix(file.data(), length);
        try {
            static_cast<void>(pkcs12::read_unencrypted(prefix));
            testing::fail(std::source_location::current(), "a truncated file was read");
            return;
        } catch (const der::FormatError &) {
        }
    }
}

TEST(survives_random_damage) {
    const auto file = sidestore_layout();
    std::mt19937_64 random(0x5eed'1234);
    for (int round = 0; round < 20000; ++round) {
        auto damaged = file;
        damaged[random() % damaged.size()] = static_cast<std::byte>(random());
        try {
            static_cast<void>(pkcs12::read_unencrypted(damaged));
        } catch (const der::FormatError &) {
        }
    }
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
