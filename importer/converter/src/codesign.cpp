#include "apkconv/codesign.h"

#include <CommonCrypto/CommonDigest.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <optional>

namespace apk::codesign {
namespace {

// Blob magic numbers, slots and flags of a code signature (XNU's osfmk/kern/cs_blobs.h).
constexpr std::uint32_t magic_requirement = 0xfade'0c00;
constexpr std::uint32_t magic_requirements = 0xfade'0c01;
constexpr std::uint32_t magic_code_directory = 0xfade'0c02;
constexpr std::uint32_t magic_embedded_signature = 0xfade'0cc0;
constexpr std::uint32_t magic_blob_wrapper = 0xfade'0b01;
constexpr std::uint32_t slot_code_directory = 0;
constexpr std::uint32_t slot_requirements = 2;
constexpr std::uint32_t slot_signature = 0x1'0000;
constexpr std::uint32_t code_directory_version = 0x2'0400;  // with the execution segment fields
constexpr std::uint32_t code_directory_header_size = 88;    // up to execSegFlags
constexpr std::uint32_t flag_adhoc = 0x2;
constexpr std::uint8_t hash_type_sha256 = 2;
constexpr std::size_t hash_size = CC_SHA256_DIGEST_LENGTH;
/// A CDHash as iOS and the cdhashes plist know it: the hash of the CodeDirectory, cut to 20 bytes.
constexpr std::size_t truncated_hash_size = 20;
/// codesign hashes arm64 code in 16 KiB pages.
constexpr std::uint8_t page_shift = 14;
constexpr std::uint64_t code_page_size = std::uint64_t{1} << page_shift;
/// Slot -1 for the Info.plist (none for a dylib, so zero) and slot -2 for the requirements.
constexpr std::uint32_t special_slots = 2;
/// The signature starts on this boundary behind the contents of __LINKEDIT, as with codesign.
constexpr std::uint64_t signature_alignment = 16;
/// Room codesign keeps for the CMS signature before it knows its size: 18,000 bytes with a
/// certificate, 8 bytes ad hoc (read off its output; the tests hold the signer to it). The header
/// records the room, so the same room makes the same CodeDirectory as codesign's.
constexpr std::uint64_t codesign_cms_room = 18000;
constexpr std::uint64_t codesign_adhoc_room = 8;
/// Segments are mapped in 16 KiB pages on arm64.
constexpr std::uint64_t segment_page_size = 16 * 1024;

// The requirement language (Security.framework's requirement.h).
constexpr std::uint32_t requirement_type_designated = 3;
constexpr std::uint32_t requirement_kind_expression = 1;
constexpr std::uint32_t op_ident = 2;
constexpr std::uint32_t op_and = 6;
constexpr std::uint32_t op_cert_field = 11;
constexpr std::uint32_t op_cert_generic = 14;
constexpr std::uint32_t op_apple_generic_anchor = 15;
constexpr std::uint32_t match_exists = 0;
constexpr std::uint32_t match_equal = 1;
constexpr std::int32_t leaf_certificate = 0;
constexpr std::int32_t first_intermediate = 1;
/// The extension that marks Apple's Worldwide Developer Relations CA, which issues Apple
/// Development certificates.
constexpr char wwdr_marker[] = "1.2.840.113635.100.6.2.1";

// The CMS signature.
constexpr char oid_signed_data[] = "1.2.840.113549.1.7.2";
constexpr char oid_data[] = "1.2.840.113549.1.7.1";
constexpr char oid_sha256[] = "2.16.840.1.101.3.4.2.1";
constexpr char oid_sha256_with_rsa[] = "1.2.840.113549.1.1.11";
constexpr char oid_content_type[] = "1.2.840.113549.1.9.3";
constexpr char oid_message_digest[] = "1.2.840.113549.1.9.4";
constexpr char oid_signing_time[] = "1.2.840.113549.1.9.5";
constexpr char oid_cdhashes_plist[] = "1.2.840.113635.100.9.1";
constexpr char oid_cdhashes[] = "1.2.840.113635.100.9.2";

using Digest = std::array<std::uint8_t, hash_size>;

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

Digest sha256(std::span<const std::byte> bytes) {
    Digest digest{};
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    return digest;
}

/// Writes the big-endian blobs of a code signature.
class BlobWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void u32(std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) u8(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value >> 32));
        u32(static_cast<std::uint32_t>(value));
    }
    void bytes(std::span<const std::byte> bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
    void string(std::string_view text) { bytes(der::bytes_of(text)); }
    /// A length-prefixed value of the requirement language, padded to 4 bytes.
    void data(std::span<const std::byte> value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(value);
        bytes_.resize(align_up(bytes_.size(), 4));
    }
    void zeros(std::size_t count) { bytes_.resize(bytes_.size() + count); }
    std::size_t size() const { return bytes_.size(); }
    der::Bytes take() { return std::move(bytes_); }

private:
    der::Bytes bytes_;
};

std::uint32_t big_endian(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) throw FormatError("the code signature is cut off");
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) value = (value << 8) | std::to_integer<std::uint32_t>(bytes[offset + index]);
    return value;
}

template <typename T>
T load(std::span<const std::byte> image, std::uint64_t offset) {
    if (offset > image.size() || image.size() - offset < sizeof(T)) throw FormatError("the Mach-O file is cut off");
    T value;
    std::memcpy(&value, image.data() + offset, sizeof value);
    return value;
}

template <typename T>
void store(std::vector<std::byte> &image, std::uint64_t offset, const T &value) {
    std::memcpy(image.data() + offset, &value, sizeof value);
}

// MARK: - Mach-O

struct SegmentCommand {
    std::uint64_t offset = 0;  // of the command in the file
    segment_command_64 command{};
    std::string_view name() const { return std::string_view(command.segname, strnlen(command.segname, 16)); }
};

struct Layout {
    mach_header_64 header{};
    std::uint64_t commands_end = 0;
    std::vector<SegmentCommand> segments;
    std::optional<std::uint64_t> signature_command;  // offset of LC_CODE_SIGNATURE
    std::uint64_t first_contents = UINT64_MAX;        // lowest file offset of a section's contents
};

bool is_universal(std::span<const std::byte> image) {
    if (image.size() < 4) return false;
    const auto magic = load<std::uint32_t>(image, 0);
    return magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
}

Layout parse(std::span<const std::byte> image) {
    if (is_universal(image)) throw FormatError("a universal binary; only thin Mach-O files are signed");
    if (image.size() < sizeof(mach_header_64) || load<std::uint32_t>(image, 0) != MH_MAGIC_64) {
        throw FormatError("not a 64-bit Mach-O file");
    }
    Layout layout{.header = load<mach_header_64>(image, 0)};
    if (layout.header.cputype != CPU_TYPE_ARM64) throw FormatError("not an arm64 Mach-O file");
    const std::uint64_t end = sizeof(mach_header_64) + std::uint64_t{layout.header.sizeofcmds};
    if (end > image.size()) throw FormatError("the load commands run past the end of the file");

    std::uint64_t offset = sizeof(mach_header_64);
    for (std::uint32_t index = 0; index < layout.header.ncmds; ++index) {
        if (end - offset < sizeof(load_command)) throw FormatError(std::format("load command {} is cut off", index));
        const auto command = load<load_command>(image, offset);
        if (command.cmdsize < sizeof(load_command) || command.cmdsize % 8 != 0 || command.cmdsize > end - offset) {
            throw FormatError(std::format("load command {} has a size ({:#x}) that does not fit", index, command.cmdsize));
        }
        if (command.cmd == LC_SEGMENT_64) {
            if (command.cmdsize < sizeof(segment_command_64)) throw FormatError("a segment load command is too small");
            const auto segment = load<segment_command_64>(image, offset);
            if (sizeof(segment_command_64) + std::uint64_t{segment.nsects} * sizeof(section_64) > command.cmdsize) {
                throw FormatError("a segment load command is too small for its sections");
            }
            for (std::uint32_t number = 0; number < segment.nsects; ++number) {
                const auto section = load<section_64>(image, offset + sizeof(segment_command_64) + number * sizeof(section_64));
                const auto type = section.flags & SECTION_TYPE;
                const bool zero_fill = type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL;
                if (!zero_fill && section.size != 0) layout.first_contents = std::min<std::uint64_t>(layout.first_contents, section.offset);
            }
            layout.segments.push_back({.offset = offset, .command = segment});
        } else if (command.cmd == LC_CODE_SIGNATURE) {
            if (command.cmdsize != sizeof(linkedit_data_command)) throw FormatError("LC_CODE_SIGNATURE has the wrong size");
            layout.signature_command = offset;
        }
        offset += command.cmdsize;
    }
    if (offset != end) throw FormatError("the load commands do not fill sizeofcmds");
    layout.commands_end = end;
    return layout;
}

/// The arm64 slice of a universal binary, or the whole image when it is thin.
std::span<const std::byte> arm64_slice(std::span<const std::byte> image) {
    if (!is_universal(image)) return image;
    const auto magic = __builtin_bswap32(load<std::uint32_t>(image, 0));
    const bool wide = magic == FAT_MAGIC_64;
    const auto count = __builtin_bswap32(load<std::uint32_t>(image, 4));
    const std::uint64_t entry_size = wide ? sizeof(fat_arch_64) : sizeof(fat_arch);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint64_t at = sizeof(fat_header) + index * entry_size;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::int32_t cpu = 0;
        if (wide) {
            const auto arch = load<fat_arch_64>(image, at);
            cpu = static_cast<std::int32_t>(__builtin_bswap32(static_cast<std::uint32_t>(arch.cputype)));
            offset = __builtin_bswap64(arch.offset);
            size = __builtin_bswap64(arch.size);
        } else {
            const auto arch = load<fat_arch>(image, at);
            cpu = static_cast<std::int32_t>(__builtin_bswap32(static_cast<std::uint32_t>(arch.cputype)));
            offset = __builtin_bswap32(arch.offset);
            size = __builtin_bswap32(arch.size);
        }
        if (cpu != CPU_TYPE_ARM64) continue;
        if (offset > image.size() || size > image.size() - offset) throw FormatError("the arm64 slice is cut off");
        return image.subspan(offset, size);
    }
    throw FormatError("the universal binary has no arm64 slice");
}

// MARK: - Blobs

/// No requirements for an ad-hoc signature. With a certificate, the designated requirement codesign
/// derives for an Apple Development certificate:
///   identifier "<identifier>" and anchor apple generic and certificate leaf[subject.CN] = "<name>"
///   and certificate 1[field.1.2.840.113635.100.6.2.1] /* exists */
der::Bytes requirements_blob(const std::string &identifier, const Identity *identity) {
    BlobWriter out;
    out.u32(magic_requirements);
    if (identity == nullptr) {
        out.u32(12);
        out.u32(0);
        return out.take();
    }
    BlobWriter expression;
    expression.u32(op_and);
    expression.u32(op_ident);
    expression.data(der::bytes_of(identifier));
    expression.u32(op_and);
    expression.u32(op_apple_generic_anchor);
    expression.u32(op_and);
    expression.u32(op_cert_field);
    expression.u32(static_cast<std::uint32_t>(leaf_certificate));
    expression.data(der::bytes_of("subject.CN"));
    expression.u32(match_equal);
    expression.data(der::bytes_of(identity->chain.front().common_name));
    expression.u32(op_cert_generic);
    expression.u32(static_cast<std::uint32_t>(first_intermediate));
    const auto marker = der::object_identifier(wwdr_marker);
    expression.data(std::span(marker).subspan(2));  // the identifier's contents, without tag and length
    expression.u32(match_exists);
    const auto body = expression.take();

    const auto requirement_size = static_cast<std::uint32_t>(12 + body.size());
    out.u32(20 + requirement_size);
    out.u32(1);
    out.u32(requirement_type_designated);
    out.u32(20);
    out.u32(magic_requirement);
    out.u32(requirement_size);
    out.u32(requirement_kind_expression);
    out.bytes(body);
    return out.take();
}

std::uint64_t code_directory_size(const std::string &identifier, const std::string &team, std::uint64_t code_limit) {
    const auto code_slots = align_up(code_limit, code_page_size) / code_page_size;
    return code_directory_header_size + identifier.size() + 1 + (team.empty() ? 0 : team.size() + 1) +
           (special_slots + code_slots) * hash_size;
}

der::Bytes code_directory(std::span<const std::byte> code, const std::string &identifier, const std::string &team,
                          std::uint32_t flags, const Digest &requirements, const segment_command_64 &text) {
    if (code.size() > UINT32_MAX) throw FormatError("the file is too large to sign (over 4 GiB)");
    const auto code_slots = static_cast<std::uint32_t>(align_up(code.size(), code_page_size) / code_page_size);
    const auto identifier_offset = code_directory_header_size;
    const auto strings_end = identifier_offset + identifier.size() + 1 + (team.empty() ? 0 : team.size() + 1);
    const auto team_offset = team.empty() ? 0 : identifier_offset + identifier.size() + 1;
    const auto hash_offset = strings_end + special_slots * hash_size;
    const auto length = code_directory_size(identifier, team, code.size());

    BlobWriter out;
    out.u32(magic_code_directory);
    out.u32(static_cast<std::uint32_t>(length));
    out.u32(code_directory_version);
    out.u32(flags);
    out.u32(static_cast<std::uint32_t>(hash_offset));
    out.u32(identifier_offset);
    out.u32(special_slots);
    out.u32(code_slots);
    out.u32(static_cast<std::uint32_t>(code.size()));  // codeLimit
    out.u8(static_cast<std::uint8_t>(hash_size));
    out.u8(hash_type_sha256);
    out.u8(0);  // platform: not a platform binary
    out.u8(page_shift);
    out.u32(0);  // spare2
    out.u32(0);  // scatterOffset: no scatter vector
    out.u32(static_cast<std::uint32_t>(team_offset));
    out.u32(0);  // spare3
    out.u64(0);  // codeLimit64: the 32-bit limit is enough
    out.u64(text.fileoff);  // execSegBase
    out.u64(text.filesize);  // execSegLimit
    out.u64(0);  // execSegFlags: a dylib is not the main binary
    out.string(identifier);
    out.u8(0);
    if (!team.empty()) {
        out.string(team);
        out.u8(0);
    }
    out.bytes(std::as_bytes(std::span(requirements)));  // slot -2
    out.zeros(hash_size);                               // slot -1: no Info.plist
    for (std::uint64_t offset = 0; offset < code.size(); offset += code_page_size) {
        const auto page = sha256(code.subspan(offset, std::min<std::uint64_t>(code_page_size, code.size() - offset)));
        out.bytes(std::as_bytes(std::span(page)));
    }
    if (out.size() != length) throw FormatError("the CodeDirectory came out with another size than planned");
    return out.take();
}

std::string base64(std::span<const std::uint8_t> bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const std::uint32_t group = (std::uint32_t{bytes[index]} << 16) |
                                    (index + 1 < bytes.size() ? std::uint32_t{bytes[index + 1]} << 8 : 0) |
                                    (index + 2 < bytes.size() ? std::uint32_t{bytes[index + 2]} : 0);
        text += alphabet[(group >> 18) & 63];
        text += alphabet[(group >> 12) & 63];
        text += index + 1 < bytes.size() ? alphabet[(group >> 6) & 63] : '=';
        text += index + 2 < bytes.size() ? alphabet[group & 63] : '=';
    }
    return text;
}

/// The cdhashes attribute, byte for byte as codesign writes it (an XML plist from CFPropertyList).
std::string cdhashes_plist(const Digest &digest) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n\t<key>cdhashes</key>\n\t<array>\n\t\t<data>\n\t\t" +
           base64(std::span(digest).first(truncated_hash_size)) +
           "\n\t\t</data>\n\t</array>\n</dict>\n</plist>\n";
}

/// The CMS signature over the CodeDirectory whose SHA-256 is `digest`: SignedData without the
/// content (detached), with the chain and the signed attributes codesign writes, in DER. With
/// `placeholder` the signature is zeros of the right size, which gives the size in advance.
der::Bytes cms_signature(const Digest &digest, const Identity &identity, std::chrono::sys_seconds time,
                         bool placeholder) {
    const auto hash = std::as_bytes(std::span(digest));
    const auto sha256_algorithm = der::sequence({der::object_identifier(oid_sha256), der::null()});
    const auto attribute = [](const char *oid, const der::Bytes &value) {
        return der::sequence({der::object_identifier(oid), der::set_of({value})});
    };
    const auto signed_attributes = der::set_of({
        attribute(oid_content_type, der::object_identifier(oid_data)),
        attribute(oid_signing_time, der::utc_time(time)),
        attribute(oid_message_digest, der::octet_string(hash)),
        attribute(oid_cdhashes, der::sequence({der::object_identifier(oid_sha256), der::octet_string(hash)})),
        attribute(oid_cdhashes_plist, der::octet_string(der::bytes_of(cdhashes_plist(digest)))),
    });
    // The key signs the attributes as a SET; in the SignerInfo they are [0] IMPLICIT.
    const auto signature = placeholder ? der::Bytes(identity.signature_size) : identity.sign(signed_attributes);
    if (signature.size() != identity.signature_size) {
        throw FormatError(std::format("the key made a signature of {} bytes instead of {}", signature.size(),
                                      identity.signature_size));
    }
    auto implicit_attributes = signed_attributes;
    implicit_attributes[0] = static_cast<std::byte>(der::context(0));

    const auto &leaf = identity.chain.front();
    const auto signer = der::sequence({
        der::integer(1),
        der::sequence({leaf.issuer, leaf.serial_number}),
        sha256_algorithm,
        implicit_attributes,
        der::sequence({der::object_identifier(oid_sha256_with_rsa), der::null()}),
        der::octet_string(signature),
    });
    std::vector<der::Bytes> certificates;
    for (const auto &certificate : identity.chain) certificates.push_back(certificate.encoding);
    const auto signed_data = der::sequence({
        der::integer(1),
        der::set_of({sha256_algorithm}),
        der::sequence({der::object_identifier(oid_data)}),
        der::set_of(std::move(certificates), der::context(0)),
        der::set_of({signer}),
    });
    return der::sequence({der::object_identifier(oid_signed_data), der::element(der::context(0), signed_data)});
}

void check_identity(const Identity &identity) {
    if (identity.chain.empty()) throw FormatError("the identity has no certificate");
    const auto &leaf = identity.chain.front();
    if (leaf.organizational_unit.empty()) throw FormatError("the certificate names no team (no subject OU)");
    if (leaf.common_name.empty()) throw FormatError("the certificate has no common name (subject CN)");
    if (!identity.sign || identity.signature_size == 0) throw FormatError("the identity cannot make a signature");
}

}  // namespace

void sign(std::vector<std::byte> &image, const Options &options) {
    if (options.identifier.empty()) throw FormatError("a signature needs an identifier");
    const Identity *const identity = options.identity;
    if (identity != nullptr) check_identity(*identity);

    auto layout = parse(image);
    if (layout.header.filetype != MH_DYLIB) throw FormatError("only dylibs are signed, and this is none");
    const auto text = std::ranges::find(layout.segments, "__TEXT", &SegmentCommand::name);
    const auto linkedit = std::ranges::find(layout.segments, "__LINKEDIT", &SegmentCommand::name);
    if (text == layout.segments.end() || linkedit == layout.segments.end()) {
        throw FormatError("the file needs a __TEXT and a __LINKEDIT segment");
    }
    for (const auto &segment : layout.segments) {
        if (segment.command.fileoff > linkedit->command.fileoff) throw FormatError("__LINKEDIT is not the last segment");
    }
    const auto linkedit_end = linkedit->command.fileoff + linkedit->command.filesize;

    // The signature goes behind the contents of __LINKEDIT, and replaces the old one there.
    std::uint64_t code_limit = 0;
    if (layout.signature_command) {
        const auto command = load<linkedit_data_command>(image, *layout.signature_command);
        if (std::uint64_t{command.dataoff} + command.datasize != linkedit_end || linkedit_end != image.size() ||
            command.dataoff < linkedit->command.fileoff) {
            throw FormatError("the old signature is not at the end of __LINKEDIT and of the file");
        }
        code_limit = command.dataoff;
    } else {
        if (linkedit_end != image.size()) throw FormatError("__LINKEDIT does not end where the file ends");
        code_limit = align_up(linkedit_end, signature_alignment);
        const auto room_end = layout.commands_end + sizeof(linkedit_data_command);
        if (room_end > layout.first_contents || room_end > image.size() ||
            std::any_of(image.begin() + static_cast<std::ptrdiff_t>(layout.commands_end),
                        image.begin() + static_cast<std::ptrdiff_t>(room_end), [](std::byte b) { return b != std::byte{0}; })) {
            throw FormatError("no room for LC_CODE_SIGNATURE behind the load commands");
        }
    }

    // Everything's size is known before anything is hashed: the header, which the first page
    // hash covers, records the size of the signature.
    const auto time = options.signing_time != std::chrono::sys_seconds{}
                          ? options.signing_time
                          : std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::string team = identity != nullptr ? identity->chain.front().organizational_unit : "";
    const auto requirements = requirements_blob(options.identifier, identity);
    const auto directory_size = code_directory_size(options.identifier, team, code_limit);
    const auto cms_size = identity != nullptr ? cms_signature(Digest{}, *identity, time, true).size() : 0;
    const auto superblob_size = 12 + 3 * 8 + directory_size + requirements.size() + 8 + cms_size;
    const auto cms_room = identity != nullptr ? std::max<std::uint64_t>(codesign_cms_room, cms_size) : codesign_adhoc_room;
    const auto data_size = align_up(12 + 3 * 8 + directory_size + requirements.size() + 8 + cms_room, signature_alignment);
    if (code_limit + data_size > UINT32_MAX) throw FormatError("the file is too large to sign (over 4 GiB)");

    if (!layout.signature_command) {
        const linkedit_data_command command{.cmd = LC_CODE_SIGNATURE, .cmdsize = sizeof(linkedit_data_command)};
        store(image, layout.commands_end, command);
        layout.signature_command = layout.commands_end;
        layout.header.ncmds += 1;
        layout.header.sizeofcmds += sizeof(linkedit_data_command);
        store(image, 0, layout.header);
    }
    auto command = load<linkedit_data_command>(image, *layout.signature_command);
    command.dataoff = static_cast<std::uint32_t>(code_limit);
    command.datasize = static_cast<std::uint32_t>(data_size);
    store(image, *layout.signature_command, command);
    auto &segment = linkedit->command;
    segment.filesize = code_limit + data_size - segment.fileoff;
    // Whole pages over the contents, as codesign sets it: growing for a large signature, and
    // shrinking again when a smaller one replaces it.
    segment.vmsize = align_up(segment.filesize, segment_page_size);
    store(image, linkedit->offset, segment);
    image.resize(code_limit);
    image.resize(code_limit + data_size);

    const auto directory = code_directory(std::span(image).first(code_limit), options.identifier, team,
                                          identity != nullptr ? 0 : flag_adhoc, sha256(requirements), text->command);
    const auto cms = identity != nullptr ? cms_signature(sha256(directory), *identity, time, false) : der::Bytes{};
    if (cms.size() != cms_size) throw FormatError("the CMS signature came out with another size than planned");

    BlobWriter superblob;
    superblob.u32(magic_embedded_signature);
    superblob.u32(static_cast<std::uint32_t>(superblob_size));
    superblob.u32(3);
    std::uint32_t offset = 12 + 3 * 8;
    superblob.u32(slot_code_directory);
    superblob.u32(offset);
    offset += static_cast<std::uint32_t>(directory.size());
    superblob.u32(slot_requirements);
    superblob.u32(offset);
    offset += static_cast<std::uint32_t>(requirements.size());
    superblob.u32(slot_signature);
    superblob.u32(offset);
    superblob.bytes(directory);
    superblob.bytes(requirements);
    superblob.u32(magic_blob_wrapper);
    superblob.u32(static_cast<std::uint32_t>(8 + cms.size()));
    superblob.bytes(cms);
    const auto blob = superblob.take();
    std::ranges::copy(blob, image.begin() + static_cast<std::ptrdiff_t>(code_limit));
}

namespace {

/// The certificate among `certificates` that the SignerInfo in `signed_data_parts` (the elements
/// of a SignedData) names by issuer and serial number; empty when there is none, or when it names
/// the certificate by its key identifier, which codesign never does.
der::Bytes signing_certificate(const std::vector<der::Element> &signed_data_parts,
                               const std::vector<der::Bytes> &certificates) {
    // signerInfos is the last element, a SET; each SignerInfo starts with its version and the signer.
    if (signed_data_parts.empty() || signed_data_parts.back().tag != der::tag_set) return {};
    const auto signer_infos = der::children(signed_data_parts.back());
    if (signer_infos.empty() || signer_infos.front().tag != der::tag_sequence) return {};
    const auto fields = der::children(signer_infos.front());
    if (fields.size() < 2 || fields[1].tag != der::tag_sequence) return {};
    const auto issuer_and_serial = der::children(fields[1]);
    if (issuer_and_serial.size() != 2) return {};
    for (const auto &encoding : certificates) {
        const auto certificate = x509::parse(encoding);
        if (std::ranges::equal(certificate.issuer, issuer_and_serial[0].encoding) &&
            std::ranges::equal(certificate.serial_number, issuer_and_serial[1].encoding)) {
            return encoding;
        }
    }
    return {};
}

}  // namespace

Signature read_signature(std::span<const std::byte> file) {
    const auto image = arm64_slice(file);
    const auto layout = parse(image);
    if (!layout.signature_command) throw FormatError("the file has no code signature");
    const auto command = load<linkedit_data_command>(image, *layout.signature_command);
    if (command.dataoff > image.size() || command.datasize > image.size() - command.dataoff) {
        throw FormatError("the code signature lies outside the file");
    }
    const auto superblob = image.subspan(command.dataoff, command.datasize);
    if (big_endian(superblob, 0) != magic_embedded_signature) throw FormatError("the code signature has a wrong magic");
    const auto length = big_endian(superblob, 4);
    const auto count = big_endian(superblob, 8);
    if (length > superblob.size() || count > (length - 12) / 8) throw FormatError("the code signature's index is broken");
    const auto blobs = superblob.first(length);

    Signature signature;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto type = big_endian(blobs, 12 + std::size_t{8} * index);
        const std::size_t offset = big_endian(blobs, 16 + std::size_t{8} * index);
        if (offset > blobs.size() - 8) throw FormatError("a blob of the code signature lies outside of it");
        const auto blob_length = big_endian(blobs, offset + 4);
        if (blob_length < 8 || blob_length > blobs.size() - offset) throw FormatError("a blob of the code signature is cut off");
        const auto blob = blobs.subspan(offset, blob_length);
        if (type == slot_code_directory) {
            if (big_endian(blob, 0) != magic_code_directory) throw FormatError("slot 0 holds no CodeDirectory");
            signature.code_directory.assign(blob.begin(), blob.end());
        } else if (type == slot_requirements) {
            signature.requirements.assign(blob.begin(), blob.end());
        } else if (type == slot_signature) {
            if (big_endian(blob, 0) != magic_blob_wrapper) throw FormatError("the CMS slot holds no blob wrapper");
            signature.cms.assign(blob.begin() + 8, blob.end());
        }
    }
    if (signature.code_directory.empty()) throw FormatError("the code signature has no CodeDirectory");

    const std::span<const std::byte> directory = signature.code_directory;
    const auto text_at = [&](std::uint32_t offset) {
        if (offset >= directory.size()) throw FormatError("a CodeDirectory string lies outside of it");
        const auto *begin = reinterpret_cast<const char *>(directory.data()) + offset;
        const auto size = strnlen(begin, directory.size() - offset);
        if (size == directory.size() - offset) throw FormatError("a CodeDirectory string has no end");
        return std::string(begin, size);
    };
    signature.flags = big_endian(directory, 12);
    signature.identifier = text_at(big_endian(directory, 20));
    if (big_endian(directory, 8) >= 0x2'0200 && big_endian(directory, 48) != 0) {
        signature.team = text_at(big_endian(directory, 48));
    }

    if (!signature.cms.empty()) {
        const auto content_info = der::read(signature.cms);
        const auto signed_data = der::child(der::child(content_info, 1, der::context(0)), 0, der::tag_sequence);
        const auto parts = der::children(signed_data);
        for (const auto &part : parts) {
            if (part.tag != der::context(0)) continue;
            for (const auto &certificate : der::children(part)) {
                signature.certificates.emplace_back(certificate.encoding.begin(), certificate.encoding.end());
            }
        }
        signature.leaf = signing_certificate(parts, signature.certificates);
    }
    return signature;
}

std::vector<x509::Certificate> complete_chain(const x509::Certificate &leaf, std::span<const der::Bytes> candidates) {
    std::vector<x509::Certificate> known;
    for (const auto &candidate : candidates) known.push_back(x509::parse(candidate));
    std::vector<x509::Certificate> chain{leaf};
    while (!chain.back().self_issued()) {
        if (chain.size() > 8) throw FormatError("the certificate chain is longer than 8 certificates");
        const auto issuer = std::ranges::find(known, chain.back().issuer, &x509::Certificate::subject);
        if (issuer == known.end()) {
            throw FormatError(std::format("the certificate of \"{}\", which issued \"{}\", is missing",
                                          chain.back().issuer_common_name, chain.back().common_name));
        }
        chain.push_back(*issuer);
    }
    return chain;
}

}  // namespace apk::codesign
