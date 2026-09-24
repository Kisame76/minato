// Checks the signer against codesign (ADR-0022):
//   - an ad-hoc signature is byte for byte the one codesign writes, for new and for replaced
//     signatures, small and large containers and the NDK fixtures;
//   - a signature with a certificate carries the chain, the team ID and the designated requirement
//     codesign shows, and its CMS signature verifies with the leaf's key (Security.framework's
//     CMSDecoder and SecKeyVerifySignature), with the signed attributes Apple's code signing uses;
//   - the chain is completed from other certificates, and what cannot be signed is refused.
//   - with an Apple Development certificate, the signature is codesign's: given a container that
//     codesign signed (from an Xcode device build with the fixtures), the signer rebuilds the same
//     CodeDirectory, requirements and signed attributes, so that codesign's own RSA signature fits
//     them, and codesign verifies the result against Apple's real chain. No private key needed.
// Usage: codesign_test <scratch directory> [<NDK fixtures directory> [<container signed by codesign>]]
#include "APKConvert/apkconv.h"
#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "apkconv/macho.h"
#include "apkconv/security_identity.h"

#include "check.h"
#include "test_identity.h"

#include <CommonCrypto/CommonDigest.h>
#include <Security/CMSDecoder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace apk;
using namespace apk::testing;
using namespace std::chrono;

std::filesystem::path scratch;
std::filesystem::path fixtures;
std::filesystem::path apple_signed;

constexpr char bundle_id[] = "io.github.kisame76.apkrunner";
const sys_seconds signing_time = sys_days{year{2026} / 9 / 23} + hours{2} + minutes{2} + seconds{28};

std::vector<std::byte> read_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    std::vector<std::byte> bytes(static_cast<std::size_t>(std::max<std::streamoff>(stream.tellg(), 0)));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

void write_file(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// Runs a shell command; its exit status and what it printed on stdout and stderr.
std::pair<int, std::string> run(const std::string &command) {
    std::string output;
    FILE *pipe = ::popen(("(" + command + ") 2>&1").c_str(), "r");
    if (pipe == nullptr) return {-1, "popen failed"};
    char buffer[4096];
    while (const auto count = std::fread(buffer, 1, sizeof buffer, pipe)) output.append(buffer, count);
    const int status = ::pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

std::string quoted(const std::filesystem::path &path) {
    return "'" + path.string() + "'";
}

/// A container laid out like the converter's: __TEXT with the header and `text_pages` pages of
/// code, one page of __DATA, and __LINKEDIT with the 16-byte string table at the end.
std::vector<std::byte> test_container(std::uint64_t text_pages, macho::Platform platform) {
    using macho::page_size;
    const std::uint64_t text_size = text_pages * page_size;
    macho::Dylib dylib{
        .install_name = "@rpath/codesign-test.dylib",
        .platform = platform,
        .version = 26u << 16,
        .uuid = {0x41, 0x50, 0x4b, 0x02, 1, 2, 0x40, 3, 0x80, 4, 5, 6, 7, 8, 9, static_cast<std::uint8_t>(text_pages)},
        .segments =
            {
                macho::Segment{.name = "__TEXT",
                               .address = 0,
                               .vm_size = text_size,
                               .file_offset = 0,
                               .file_size = text_size,
                               .protection = macho::protection_read | macho::protection_execute,
                               .sections = {macho::Section{.name = "__apk_info", .address = 0x800, .size = 16}}},
                macho::Segment{.name = "__DATA",
                               .address = text_size,
                               .vm_size = page_size,
                               .file_offset = text_size,
                               .file_size = page_size,
                               .protection = macho::protection_read | macho::protection_write,
                               .sections = {}},
                macho::Segment{.name = "__LINKEDIT",
                               .address = text_size + page_size,
                               .vm_size = page_size,
                               .file_offset = text_size + page_size,
                               .file_size = 16,
                               .protection = macho::protection_read,
                               .sections = {}},
            },
    };
    std::vector<std::byte> image(text_size + page_size + 16);
    // Contents that differ from page to page, so that every page hash counts.
    for (std::size_t index = 0x800; index < text_size + page_size; ++index) {
        image[index] = static_cast<std::byte>((index * 2654435761u) >> 13);
    }
    macho::write_header(dylib, image);
    return image;
}

std::vector<std::byte> signed_copy(std::vector<std::byte> image, const codesign::Options &options) {
    codesign::sign(image, options);
    return image;
}

/// What codesign makes of `image` with an ad-hoc signature and `identifier`.
std::vector<std::byte> codesigned(std::span<const std::byte> image, const std::string &identifier) {
    const auto path = scratch / "codesign-input.dylib";
    write_file(path, image);
    const auto [status, output] =
        run(std::format("codesign --force --sign - --timestamp=none --identifier {} {}", identifier, quoted(path)));
    if (status != 0) {
        std::fprintf(stderr, "codesign failed: %s\n", output.c_str());
        return {};
    }
    return read_file(path);
}

/// Where the first difference is, for a readable failure.
void check_same(std::span<const std::byte> ours, std::span<const std::byte> theirs, const std::string &what,
                std::source_location location = std::source_location::current()) {
    if (std::ranges::equal(ours, theirs)) return;
    std::size_t at = 0;
    while (at < ours.size() && at < theirs.size() && ours[at] == theirs[at]) ++at;
    const auto detail = std::format("{}: {} bytes vs codesign's {}, first difference at {:#x}", what, ours.size(),
                                    theirs.size(), at);
    fail(location, "not the same as codesign: ", detail.c_str());
}

std::array<std::uint8_t, 32> sha256(std::span<const std::byte> bytes) {
    std::array<std::uint8_t, 32> digest{};
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    return digest;
}

std::string hex(std::span<const std::uint8_t> bytes) {
    std::string text;
    for (const auto byte : bytes) text += std::format("{:02x}", byte);
    return text;
}

// MARK: - Ad hoc: exactly codesign

TEST(adhoc_signature_is_codesigns_byte_for_byte) {
    for (const auto platform : {macho::Platform::macos, macho::Platform::ios}) {
        // 1 and 3 pages; 600 pages make a CodeDirectory larger than a page, so __LINKEDIT grows.
        for (const std::uint64_t pages : {1u, 3u, 600u}) {
            const auto unsigned_image = test_container(pages, platform);
            for (const std::string identifier : {"x", bundle_id}) {
                const auto what = std::format("{} pages, platform {}, identifier {}", pages,
                                              static_cast<int>(platform), identifier);
                const auto theirs = codesigned(unsigned_image, identifier);
                CHECK(!theirs.empty());
                const auto ours = signed_copy(unsigned_image, {.identifier = identifier});
                check_same(ours, theirs, what);
                // Replacing a signature: ours over codesign's, and codesign's over ours.
                check_same(signed_copy(theirs, {.identifier = identifier}), theirs, what + ", replacing codesign's");
                check_same(codesigned(ours, identifier), theirs, what + ", codesign replacing ours");
            }
        }
    }
}

TEST(adhoc_signatures_of_the_fixtures_are_codesigns) {
    if (fixtures.empty() || !std::filesystem::exists(fixtures / "hello")) {
        std::printf("     (fixtures missing, skipped)\n");
        return;
    }
    for (const char *name : {"raw-hello", "hello", "hello-dyn", "libdl-plugin.so", "libtls-lib.so", "dl-test"}) {
        const auto elf = read_file(fixtures / name);
        const auto container = container::convert(elf, {.install_name = std::format("@rpath/{}.dylib", name),
                                                         .platform = macho::Platform::ios})
                                   .bytes;
        check_same(signed_copy(container, {.identifier = bundle_id}), codesigned(container, bundle_id), name);
    }
}

TEST(codesign_verifies_our_adhoc_signature) {
    const auto path = scratch / "adhoc.dylib";
    write_file(path, signed_copy(test_container(3, macho::Platform::macos), {.identifier = "adhoc-test"}));
    const auto [status, output] = run("codesign --verify --strict --verbose=2 " + quoted(path));
    CHECK(status == 0);
    CHECK(output.find("valid on disk") != std::string::npos);
    CHECK(output.find("satisfies its Designated Requirement") != std::string::npos);

    // The signature is the one read back.
    const auto signature = codesign::read_signature(read_file(path));
    CHECK(signature.identifier == "adhoc-test");
    CHECK(signature.team.empty());
    CHECK(signature.flags == 0x2);
    CHECK(signature.certificates.empty());
    CHECK(signature.leaf.empty());
    CHECK(signature.cms.empty());
}

// MARK: - With a certificate

struct Signed {
    TestChain chain;
    std::vector<std::byte> image;
    codesign::Signature signature;
};

Signed &signed_with_certificate() {
    static Signed result = [] {
        Signed made{.chain = make_test_chain(), .image = {}, .signature = {}};
        made.image = signed_copy(test_container(3, macho::Platform::ios),
                                 {.identifier = bundle_id, .identity = &made.chain.identity, .signing_time = signing_time});
        made.signature = codesign::read_signature(made.image);
        return made;
    }();
    return result;
}

TEST(certificate_signature_names_identifier_team_and_chain) {
    const auto &made = signed_with_certificate();
    CHECK(made.signature.identifier == bundle_id);
    CHECK(made.signature.team == test_team);
    CHECK(made.signature.flags == 0);
    CHECK(made.signature.certificates.size() == 3);
    // The certificate the CMS signature names as its signer, whatever the order of the chain.
    CHECK(made.signature.leaf == made.chain.leaf.encoding);
    CHECK(!made.signature.cms.empty());

    const auto path = scratch / "certificate.dylib";
    write_file(path, made.image);
    const auto [status, details] = run("codesign --display --verbose=4 --requirements - " + quoted(path));
    const auto cdhash = sha256(made.signature.code_directory);
    CHECK(status == 0);
    for (const auto &expected : {
             std::format("Identifier={}", bundle_id),
             std::string("CodeDirectory v=20400 size="),
             std::string("flags=0x0(none) hashes=5+2 location=embedded"),
             std::string("Hash type=sha256 size=32"),
             std::format("CDHash={}", hex(std::span(cdhash).first(20))),
             std::format("TeamIdentifier={}", test_team),
             std::format("designated => identifier \"{}\" and anchor apple generic and certificate "
                         "leaf[subject.CN] = \"{}\" and certificate 1[field.1.2.840.113635.100.6.2.1] /* exists */",
                         bundle_id, test_leaf_name),
         }) {
        if (details.find(expected) == std::string::npos) {
            fail(std::source_location::current(), "codesign does not show: ", expected.c_str());
            std::fprintf(stderr, "%s\n", details.c_str());
        }
    }
    // The whole chain is in the signature.
    auto carried = made.signature.certificates;
    std::vector<der::Bytes> chain{made.chain.leaf.encoding, made.chain.intermediate.encoding, made.chain.root.encoding};
    std::ranges::sort(carried);
    std::ranges::sort(chain);
    CHECK(carried == chain);
    // codesign finds the signature valid; only the designated requirement wants Apple's root.
    const auto [verify_status, verify_output] = run("codesign --verify --verbose=2 " + quoted(path));
    CHECK(verify_status != 0);
    if (verify_output.find("valid on disk") == std::string::npos ||
        verify_output.find("does not satisfy its designated Requirement") == std::string::npos) {
        fail(std::source_location::current(), "codesign --verify failed for another reason than the test root: ",
             verify_output.c_str());
    }
}

TEST(cms_signature_verifies_with_the_leaf_key) {
    const auto &made = signed_with_certificate();
    const auto &code_directory = made.signature.code_directory;

    CMSDecoderRef decoder = nullptr;
    CHECK(CMSDecoderCreate(&decoder) == errSecSuccess);
    CHECK(CMSDecoderUpdateMessage(decoder, made.signature.cms.data(), made.signature.cms.size()) == errSecSuccess);
    CHECK(CMSDecoderFinalizeMessage(decoder) == errSecSuccess);
    const auto content = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(code_directory.data()),
                                      static_cast<CFIndex>(code_directory.size()));
    CHECK(CMSDecoderSetDetachedContent(decoder, content) == errSecSuccess);
    std::size_t signers = 0;
    CHECK(CMSDecoderGetNumSigners(decoder, &signers) == errSecSuccess);
    CHECK(signers == 1);
    const auto policy = SecPolicyCreateBasicX509();
    CMSSignerStatus status = kCMSSignerUnsigned;
    CHECK(CMSDecoderCopySignerStatus(decoder, 0, policy, false, &status, nullptr, nullptr) == errSecSuccess);
    CHECK(status == kCMSSignerValid);
    CFAbsoluteTime time = 0;
    CHECK(CMSDecoderCopySignerSigningTime(decoder, 0, &time) == errSecSuccess);
    CHECK(static_cast<std::int64_t>(time + kCFAbsoluteTimeIntervalSince1970) ==
          signing_time.time_since_epoch().count());
    CFRelease(policy);
    CFRelease(content);
    CFRelease(decoder);
}

TEST(cms_signature_has_apples_signed_attributes) {
    const auto &made = signed_with_certificate();
    const auto digest = sha256(made.signature.code_directory);
    const auto content_info = der::read(made.signature.cms);
    CHECK(der::object_identifier_text(der::child(content_info, 0, der::tag_object_identifier)) == "1.2.840.113549.1.7.2");
    const auto signed_data = der::child(der::child(content_info, 1, der::context(0)), 0, der::tag_sequence);
    const auto parts = der::children(signed_data);
    CHECK(parts.size() == 5);
    CHECK(der::children(parts[3]).size() == 3);  // the certificates, [0] IMPLICIT SET OF
    const auto signer = der::child(parts[4], 0, der::tag_sequence);
    const auto fields = der::children(signer);
    CHECK(fields.size() == 6);
    CHECK(fields[3].tag == der::context(0));

    std::vector<std::string> names;
    for (const auto &attribute : der::children(fields[3])) {
        const auto name = der::object_identifier_text(der::child(attribute, 0, der::tag_object_identifier));
        const auto value = der::child(der::child(attribute, 1, der::tag_set), 0,
                                      der::children(der::child(attribute, 1, der::tag_set))[0].tag);
        names.push_back(name);
        if (name == "1.2.840.113549.1.9.4") {  // messageDigest: SHA-256 of the CodeDirectory
            CHECK(std::ranges::equal(value.contents, std::as_bytes(std::span(digest))));
        } else if (name == "1.2.840.113635.100.9.2") {  // CDHashes2: hash type and full hash
            CHECK(der::object_identifier_text(der::child(value, 0, der::tag_object_identifier)) == "2.16.840.1.101.3.4.2.1");
            CHECK(std::ranges::equal(der::child(value, 1, der::tag_octet_string).contents, std::as_bytes(std::span(digest))));
        } else if (name == "1.2.840.113635.100.9.1") {  // CDHashes: a plist with the truncated hash
            const std::string plist(reinterpret_cast<const char *>(value.contents.data()), value.contents.size());
            CHECK(plist.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC"));
            CHECK(plist.find("\t<key>cdhashes</key>\n\t<array>\n\t\t<data>\n\t\t") != std::string::npos);
            CHECK(plist.ends_with("=\n\t\t</data>\n\t</array>\n</dict>\n</plist>\n"));
        } else if (name == "1.2.840.113549.1.9.5") {  // signingTime
            CHECK(der::time(value) == signing_time);
        }
    }
    // In DER order, which here is the order codesign writes them in.
    CHECK((names == std::vector<std::string>{"1.2.840.113549.1.9.3", "1.2.840.113549.1.9.5", "1.2.840.113549.1.9.4",
                                             "1.2.840.113635.100.9.2", "1.2.840.113635.100.9.1"}));

    // The signature is over the attributes as a SET, with the leaf's key.
    auto attributes = der::Bytes(fields[3].encoding.begin(), fields[3].encoding.end());
    attributes[0] = std::byte{der::tag_set};
    const auto signature = fields[5].contents;
    const auto public_key = SecKeyCopyPublicKey(made.chain.key);
    const auto message = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(attributes.data()),
                                      static_cast<CFIndex>(attributes.size()));
    auto signature_data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(signature.data()),
                                       static_cast<CFIndex>(signature.size()));
    CHECK(SecKeyVerifySignature(public_key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, message,
                                signature_data, nullptr));
    // A changed signature does not verify: the check above means something.
    std::vector<std::byte> broken(signature.begin(), signature.end());
    broken[10] ^= std::byte{1};
    CFRelease(signature_data);
    signature_data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(broken.data()),
                                  static_cast<CFIndex>(broken.size()));
    CHECK(!SecKeyVerifySignature(public_key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, message,
                                 signature_data, nullptr));
    CFRelease(signature_data);
    CFRelease(message);
    CFRelease(public_key);
}

TEST(signing_with_a_certificate_is_deterministic_and_replaceable) {
    auto &made = signed_with_certificate();
    const auto again = signed_copy(test_container(3, macho::Platform::ios),
                                   {.identifier = bundle_id, .identity = &made.chain.identity, .signing_time = signing_time});
    CHECK(again == made.image);
    // Ad hoc over a certificate signature is codesign's ad-hoc signature again.
    const auto unsigned_image = test_container(3, macho::Platform::ios);
    check_same(signed_copy(made.image, {.identifier = bundle_id}), codesigned(unsigned_image, bundle_id), "ad hoc over certificate");
    // And a certificate signature over an ad-hoc one is the same as over none.
    CHECK(signed_copy(codesigned(unsigned_image, bundle_id),
                      {.identifier = bundle_id, .identity = &made.chain.identity, .signing_time = signing_time}) ==
          made.image);
}

TEST(signer_checks_its_identity) {
    auto chain = make_test_chain();
    auto image = test_container(1, macho::Platform::ios);
    codesign::Identity wrong_size = chain.identity;
    wrong_size.signature_size = 128;
    CHECK_THROWS(codesign::FormatError, codesign::sign(image, {.identifier = "x", .identity = &wrong_size}), "signature");
    codesign::Identity no_chain = chain.identity;
    no_chain.chain.clear();
    CHECK_THROWS(codesign::FormatError, codesign::sign(image, {.identifier = "x", .identity = &no_chain}), "certificate");
    codesign::Identity no_team = chain.identity;
    no_team.chain[0].organizational_unit.clear();
    CHECK_THROWS(codesign::FormatError, codesign::sign(image, {.identifier = "x", .identity = &no_team}), "team");
    CHECK_THROWS(codesign::FormatError, codesign::sign(image, {.identifier = ""}), "identifier");
}

// MARK: - An Apple Development certificate

/// The signer information of a CMS signature: the fields of its SignerInfo.
std::vector<der::Element> signer_fields(std::span<const std::byte> cms) {
    const auto content_info = der::read(cms);
    const auto signed_data = der::child(der::child(content_info, 1, der::context(0)), 0, der::tag_sequence);
    const auto parts = der::children(signed_data);
    return der::children(der::child(parts.back(), 0, der::tag_sequence));
}

TEST(signs_like_codesign_with_an_apple_certificate) {
    if (apple_signed.empty() || !std::filesystem::exists(apple_signed)) {
        std::printf("     (no container signed by codesign with an Apple Development certificate, skipped)\n");
        return;
    }
    const auto reference = read_file(apple_signed);
    const auto theirs = codesign::read_signature(reference);
    CHECK(!theirs.team.empty());
    CHECK(theirs.certificates.size() == 3);

    // codesign's signing time and RSA signature, and the chain it put in.
    const auto fields = signer_fields(theirs.cms);
    std::chrono::sys_seconds time{};
    for (const auto &attribute : der::children(fields[3])) {
        if (der::object_identifier_text(der::child(attribute, 0, der::tag_object_identifier)) == "1.2.840.113549.1.9.5") {
            time = der::time(der::children(der::child(attribute, 1, der::tag_set))[0]);
        }
    }
    const der::Bytes rsa_signature(fields[5].contents.begin(), fields[5].contents.end());
    // The signer is the Apple Development certificate of the team.
    CHECK(!theirs.leaf.empty());
    if (theirs.leaf.empty()) return;
    CHECK(x509::parse(theirs.leaf).organizational_unit == theirs.team);
    // The key is not at hand, but PKCS #1 v1.5 is deterministic: for the same signed attributes the
    // signature is the one codesign made.
    const codesign::Identity replay{
        .chain = codesign::complete_chain(x509::parse(theirs.leaf), theirs.certificates),
        .signature_size = rsa_signature.size(),
        .sign = [&](std::span<const std::byte>) { return rsa_signature; },
    };
    CHECK(replay.chain.size() == 3);

    auto ours = reference;
    codesign::sign(ours, {.identifier = theirs.identifier, .identity = &replay, .signing_time = time});
    const auto signature = codesign::read_signature(ours);
    CHECK(ours.size() == reference.size());
    CHECK(signature.code_directory == theirs.code_directory);
    CHECK(signature.requirements == theirs.requirements);
    const auto our_attributes = signer_fields(signature.cms)[3].encoding;
    CHECK(std::ranges::equal(our_attributes, fields[3].encoding));

    // Apple's verification with the real chain: the signature, the chain up to Apple Root CA and
    // the designated requirement. codesign writes the CMS with indefinite lengths, we in DER.
    const auto path = scratch / "apple-signed.dylib";
    write_file(path, ours);
    const auto [status, output] = run("codesign --verify --strict --verbose=2 " + quoted(path));
    CHECK(status == 0);
    CHECK(output.find("valid on disk") != std::string::npos);
    CHECK(output.find("satisfies its Designated Requirement") != std::string::npos);
}

// MARK: - Chains

TEST(chain_is_completed_up_to_the_root) {
    const auto chain = make_test_chain();
    const std::vector<der::Bytes> candidates{chain.root.encoding, test_certificate(chain.key, 9, test_name("Other"), test_name("Other"), true).encoding,
                                             chain.intermediate.encoding};
    const auto completed = codesign::complete_chain(chain.leaf, candidates);
    CHECK(completed.size() == 3);
    CHECK(completed[0].common_name == test_leaf_name);
    CHECK(completed[1].common_name == test_intermediate_name);
    CHECK(completed[2].common_name == test_root_name);
    CHECK(completed[0].organizational_unit == test_team);
    // A self-issued certificate is its own chain.
    CHECK(codesign::complete_chain(chain.root, {}).size() == 1);
    // Without the intermediate there is no chain.
    const std::vector<der::Bytes> without{chain.root.encoding};
    CHECK_THROWS(codesign::FormatError, codesign::complete_chain(chain.leaf, without), test_intermediate_name);
}

TEST(identity_from_p12_is_read_in_memory) {
    // macOS's LibreSSL writes the .p12, from the test chain's key and leaf.
    const auto chain = make_test_chain();
    const auto private_key = SecKeyCopyExternalRepresentation(chain.key, nullptr);  // PKCS #1 RSAPrivateKey
    const auto *begin = reinterpret_cast<const std::byte *>(CFDataGetBytePtr(private_key));
    write_file(scratch / "key.der", std::span(begin, static_cast<std::size_t>(CFDataGetLength(private_key))));
    CFRelease(private_key);
    write_file(scratch / "leaf.der", chain.leaf.encoding);
    const auto [status, output] = run(std::format(
        "cd {} && /usr/bin/openssl rsa -inform DER -in key.der -out key.pem && "
        "/usr/bin/openssl x509 -inform DER -in leaf.der -out leaf.pem && "
        "/usr/bin/openssl pkcs12 -export -inkey key.pem -in leaf.pem -passout pass:secret -out identity.p12",
        quoted(scratch)));
    if (status != 0) {
        fail(std::source_location::current(), "openssl could not write the .p12 file: ", output.c_str());
        return;
    }
    const auto p12 = read_file(scratch / "identity.p12");
    const std::vector<der::Bytes> candidates{chain.intermediate.encoding, chain.root.encoding};
    const auto identity = codesign::identity_from_p12(p12, "secret", candidates);
    CHECK(identity.chain.size() == 3);
    CHECK(identity.chain[0].organizational_unit == test_team);
    CHECK(identity.signature_size == 256);
    // It signs like the key it came from.
    auto image = test_container(1, macho::Platform::ios);
    codesign::sign(image, {.identifier = bundle_id, .identity = &identity, .signing_time = signing_time});
    auto expected = test_container(1, macho::Platform::ios);
    codesign::sign(expected, {.identifier = bundle_id, .identity = &chain.identity, .signing_time = signing_time});
    CHECK(image == expected);

    CHECK_THROWS(codesign::FormatError, codesign::identity_from_p12(p12, "wrong", candidates), "password");
    CHECK_THROWS(codesign::FormatError, codesign::identity_from_p12(p12, "secret", {}), test_intermediate_name);
    const std::vector<std::byte> garbage(64, std::byte{0x42});
    CHECK_THROWS(codesign::FormatError, codesign::identity_from_p12(garbage, "secret", {}), ".p12");
}

// MARK: - Handing the identity over to the iPhone (tools/hand-over-certificate.sh)

TEST(describes_serial_number_and_validity) {
    const auto chain = make_test_chain();
    const auto describe = [](const x509::Certificate &certificate) {
        apkconv_certificate described{};
        char error[256] = "";
        CHECK(apkconv_describe_certificate(certificate.encoding.data(), certificate.encoding.size(), &described, error,
                                           sizeof error) == 0);
        return described;
    };
    const auto leaf = describe(chain.leaf);
    CHECK(std::string_view(leaf.serial_number) == "7B3809B45860FDF9");
    CHECK(leaf.not_before == 1'767'225'600);  // 2026-01-01
    CHECK(leaf.not_after == 2'082'758'400);   // 2036-01-01
    CHECK(std::string_view(describe(chain.intermediate).serial_number) == "123456789ABC");
    CHECK(std::string_view(describe(chain.root).serial_number) == "01");
    // DER puts a zero byte in front of a serial whose top bit is set, to keep it positive.
    const auto high = test_certificate(chain.key, 0x8000'0000, test_name(test_root_name), test_name("High Serial"), false);
    CHECK(std::string_view(describe(high).serial_number) == "80000000");
}

TEST(names_the_certificates_of_a_signature) {
    const auto chain = make_test_chain();
    const auto path = scratch / "chain-names.dylib";
    write_file(path, signed_copy(test_container(1, macho::Platform::macos), {.identifier = bundle_id, .identity = &chain.identity}));
    char names[1024] = "";
    char error[256] = "";
    CHECK(apkconv_signing_chain(path.c_str(), names, sizeof names, error, sizeof error) == 0);
    std::vector<std::string> lines;
    for (std::string_view rest = names; !rest.empty();) {
        const auto end = rest.find('\n');
        lines.emplace_back(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    }
    std::ranges::sort(lines);
    // Sorted: the signature carries them as a DER SET OF, in the order of their encodings.
    CHECK((lines == std::vector<std::string>{test_intermediate_name, test_leaf_name, test_root_name}));

    const auto adhoc = scratch / "chain-names-adhoc.dylib";
    write_file(adhoc, signed_copy(test_container(1, macho::Platform::macos), {.identifier = bundle_id}));
    std::strcpy(names, "unset");
    CHECK(apkconv_signing_chain(adhoc.c_str(), names, sizeof names, error, sizeof error) == 0);
    CHECK(names[0] == '\0');
}

TEST(signer_from_a_key_and_its_certificate) {
    if (fixtures.empty() || !std::filesystem::exists(fixtures / "raw-hello")) {
        std::printf("     (no fixtures, skipped)\n");
        return;
    }
    const auto chain = make_test_chain();
    // The app's own executable in the app: signed with the same chain, which completes the leaf's.
    const auto chain_source = scratch / "chain-source.dylib";
    write_file(chain_source, signed_copy(test_container(1, macho::Platform::macos), {.identifier = bundle_id, .identity = &chain.identity}));

    char error[512] = "";
    auto *signer = apkconv_signer_create_with_key(chain.key, chain.leaf.encoding.data(), chain.leaf.encoding.size(),
                                                  chain_source.c_str(), bundle_id, error, sizeof error);
    CHECK(signer != nullptr);
    if (signer == nullptr) {
        std::fprintf(stderr, "apkconv_signer_create_with_key: %s\n", error);
        return;
    }
    const auto container = scratch / "raw-hello-with-key.dylib";
    CHECK(apkconv_convert(signer, (fixtures / "raw-hello").c_str(), container.c_str(), nullptr, error, sizeof error) == 0);
    apkconv_signer_free(signer);
    const auto signature = codesign::read_signature(read_file(container));
    CHECK(signature.identifier == bundle_id);
    CHECK(signature.team == test_team);
    CHECK(signature.leaf == chain.leaf.encoding);
    CHECK(signature.certificates.size() == 3);

    // A certificate of another team: iOS would not load what it signs.
    const auto other = test_certificate(chain.key, 99, test_name(test_intermediate_name), test_name(test_leaf_name, "OTHERTEAM1"), false);
    error[0] = '\0';
    CHECK(apkconv_signer_create_with_key(chain.key, other.encoding.data(), other.encoding.size(), chain_source.c_str(),
                                         bundle_id, error, sizeof error) == nullptr);
    CHECK(std::string_view(error).find("OTHERTEAM1") != std::string_view::npos);
    CHECK(std::string_view(error).find(test_team) != std::string_view::npos);
}

TEST(keychain_export_refuses_a_certificate_without_an_identity) {
    // The test chain is in no keychain. Looking for it only reads the keychain, nothing asks the user.
    const auto chain = make_test_chain();
    CHECK_THROWS(codesign::FormatError, codesign::keychain_identity_as_p12(chain.leaf, "secret"), test_leaf_name);
}

// MARK: - Refusals

TEST(refuses_what_it_cannot_sign) {
    const codesign::Options options{.identifier = "x"};
    std::vector<std::byte> empty;
    CHECK_THROWS(codesign::FormatError, codesign::sign(empty, options), "Mach-O");
    std::vector<std::byte> text(4096, std::byte{'A'});
    CHECK_THROWS(codesign::FormatError, codesign::sign(text, options), "Mach-O");

    // The file goes on behind __LINKEDIT.
    auto trailing = test_container(1, macho::Platform::macos);
    trailing.resize(trailing.size() + 64);
    CHECK_THROWS(codesign::FormatError, codesign::sign(trailing, options), "__LINKEDIT");

    // No room behind the load commands: a section starts right there.
    auto crowded = test_container(1, macho::Platform::macos);
    const auto header = reinterpret_cast<mach_header_64 *>(crowded.data());
    const auto commands_end = sizeof(mach_header_64) + header->sizeofcmds;
    auto *first = reinterpret_cast<segment_command_64 *>(crowded.data() + sizeof(mach_header_64));
    auto *section = reinterpret_cast<section_64 *>(first + 1);
    section->offset = static_cast<std::uint32_t>(commands_end + 8);
    CHECK_THROWS(codesign::FormatError, codesign::sign(crowded, options), "room");

    // A load command that runs past the others.
    auto broken = test_container(1, macho::Platform::macos);
    reinterpret_cast<segment_command_64 *>(broken.data() + sizeof(mach_header_64))->cmdsize = 0x7fff'fff0;
    CHECK_THROWS(codesign::FormatError, codesign::sign(broken, options), "load command");

    // Nothing to read without a signature.
    CHECK_THROWS(codesign::FormatError, codesign::read_signature(test_container(1, macho::Platform::macos)), "no code signature");
}

TEST(survives_broken_signatures) {
    // Cut off and randomly changed signatures are refused with an exception, never read past their
    // end (the test runs under ASan and UBSan). The app reads its own executable this way.
    const auto &made = signed_with_certificate();
    const auto &image = made.image;
    const auto command = [&] {
        const auto header = reinterpret_cast<const mach_header_64 *>(image.data());
        std::size_t offset = sizeof(mach_header_64);
        for (std::uint32_t index = 0; index < header->ncmds; ++index) {
            const auto *load = reinterpret_cast<const load_command *>(image.data() + offset);
            if (load->cmd == LC_CODE_SIGNATURE) return *reinterpret_cast<const linkedit_data_command *>(load);
            offset += load->cmdsize;
        }
        return linkedit_data_command{};
    }();
    CHECK(command.dataoff != 0);
    const auto attempt = [](std::span<const std::byte> bytes) {
        try {
            static_cast<void>(codesign::read_signature(bytes));
        } catch (const std::exception &) {
        }
    };
    for (std::size_t size = command.dataoff; size < image.size(); size += 7) attempt(std::span(image).first(size));
    std::uint32_t state = 0x2545'f491;
    for (int round = 0; round < 3000; ++round) {
        auto broken = image;
        for (int change = 0; change < 1 + round % 4; ++change) {
            state = state * 1'664'525u + 1'013'904'223u;
            const auto at = command.dataoff + state % (image.size() - command.dataoff);
            broken[at] = static_cast<std::byte>(state >> 24);
        }
        attempt(broken);
        try {
            codesign::sign(broken, {.identifier = "x"});
        } catch (const std::exception &) {
        }
    }
}

TEST(reads_the_arm64_slice_of_a_universal_binary) {
    const auto slice = signed_copy(test_container(1, macho::Platform::macos), {.identifier = "universal"});
    // fat_header and two fat_archs, big-endian: an x86_64 slice of junk first, then ours.
    std::vector<std::byte> universal(3 * macho::page_size + slice.size());
    const auto put = [&](std::size_t at, std::uint32_t value) {
        const std::uint32_t big = __builtin_bswap32(value);
        std::memcpy(universal.data() + at, &big, 4);
    };
    put(0, FAT_MAGIC);
    put(4, 2);
    put(8, CPU_TYPE_X86_64);
    put(12, 3);
    put(16, static_cast<std::uint32_t>(macho::page_size));
    put(20, 64);
    put(24, 14);
    put(28, CPU_TYPE_ARM64);
    put(32, 0);
    put(36, static_cast<std::uint32_t>(3 * macho::page_size));
    put(40, static_cast<std::uint32_t>(slice.size()));
    put(44, 14);
    std::memcpy(universal.data() + 3 * macho::page_size, slice.data(), slice.size());
    CHECK(codesign::read_signature(universal).identifier == "universal");
    // The signer itself only takes thin files.
    CHECK_THROWS(codesign::FormatError, codesign::sign(universal, {.identifier = "x"}), "thin");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: codesign_test <scratch directory> [<fixtures directory>]\n");
        return 2;
    }
    scratch = argv[1];
    if (argc > 2) fixtures = argv[2];
    if (argc > 3) apple_signed = argv[3];
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    return run_all();
}
