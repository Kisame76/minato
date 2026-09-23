#include "apkconv/certificate.h"

#include <format>

namespace apk::x509 {
namespace {

using der::Element;
using der::FormatError;

constexpr char common_name_oid[] = "2.5.4.3";
constexpr char organizational_unit_oid[] = "2.5.4.11";

/// The first value of attribute `oid` in a Name, or "" when the Name has none.
std::string attribute(const Element &name, std::string_view oid) {
    for (const auto &relative : der::children(name)) {
        if (relative.tag != der::tag_set) throw FormatError("a name holds something other than attribute sets");
        for (const auto &pair : der::children(relative)) {
            if (pair.tag != der::tag_sequence) throw FormatError("a name attribute is not a sequence");
            if (der::object_identifier_text(der::child(pair, 0, der::tag_object_identifier)) != oid) continue;
            const auto items = der::children(pair);
            if (items.size() != 2) throw FormatError("a name attribute has no single value");
            switch (items[1].tag) {
                case der::tag_utf8_string:
                case der::tag_printable_string:
                case 0x14:  // TeletexString, taken as Latin-1 like everyone does
                case 0x16:  // IA5String
                    return std::string(reinterpret_cast<const char *>(items[1].contents.data()),
                                       items[1].contents.size());
                default:
                    throw FormatError(std::format("a name attribute has string type {:#04x}", items[1].tag));
            }
        }
    }
    return {};
}

der::Bytes copy(std::span<const std::byte> bytes) {
    return der::Bytes(bytes.begin(), bytes.end());
}

}  // namespace

Certificate parse(std::span<const std::byte> encoding) {
    const auto certificate = der::read(encoding);
    if (certificate.tag != der::tag_sequence || certificate.encoding.size() != encoding.size()) {
        throw FormatError("not a certificate: no single sequence");
    }
    const auto to_be_signed = der::child(certificate, 0, der::tag_sequence);
    const auto fields = der::children(to_be_signed);
    // The version is optional: [0] EXPLICIT INTEGER, left out for version 1.
    const std::size_t at = !fields.empty() && fields[0].tag == der::context(0) ? 1 : 0;
    if (fields.size() < at + 6) throw FormatError("the certificate has too few fields");
    const auto &serial = fields[at];
    const auto &issuer = fields[at + 2];
    const auto &validity = fields[at + 3];
    const auto &subject = fields[at + 4];
    if (serial.tag != der::tag_integer || issuer.tag != der::tag_sequence || validity.tag != der::tag_sequence ||
        subject.tag != der::tag_sequence) {
        throw FormatError("the certificate's fields are not in X.509 order");
    }
    const auto times = der::children(validity);
    if (times.size() != 2) throw FormatError("the validity is not two times");
    return Certificate{
        .encoding = copy(encoding),
        .issuer = copy(issuer.encoding),
        .subject = copy(subject.encoding),
        .serial_number = copy(serial.encoding),
        .common_name = attribute(subject, common_name_oid),
        .organizational_unit = attribute(subject, organizational_unit_oid),
        .issuer_common_name = attribute(issuer, common_name_oid),
        .not_before = der::time(times[0]),
        .not_after = der::time(times[1]),
    };
}

}  // namespace apk::x509
