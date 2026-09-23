// DER, the ASN.1 encoding of certificates and of the CMS signature inside a code signature
// (ADR-0022). The writer builds what the signer needs; the reader takes certificates and signatures
// apart, including the indefinite lengths (BER) that codesign writes into its CMS signatures.
// Only low tag numbers (0-30), which is all X.509 and CMS use.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace apk::der {

using Bytes = std::vector<std::byte>;

// Universal tags, with the constructed bit where the type is always constructed.
inline constexpr std::uint8_t tag_integer = 0x02;
inline constexpr std::uint8_t tag_bit_string = 0x03;
inline constexpr std::uint8_t tag_octet_string = 0x04;
inline constexpr std::uint8_t tag_null = 0x05;
inline constexpr std::uint8_t tag_object_identifier = 0x06;
inline constexpr std::uint8_t tag_utf8_string = 0x0c;
inline constexpr std::uint8_t tag_printable_string = 0x13;
inline constexpr std::uint8_t tag_utc_time = 0x17;
inline constexpr std::uint8_t tag_generalized_time = 0x18;
inline constexpr std::uint8_t tag_sequence = 0x30;
inline constexpr std::uint8_t tag_set = 0x31;

/// The tag of a context-specific element [number], constructed unless it wraps a primitive IMPLICIT.
constexpr std::uint8_t context(unsigned number, bool constructed = true) {
    return static_cast<std::uint8_t>((constructed ? 0xa0u : 0x80u) | number);
}

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// MARK: - Writing

/// One element: tag, definite length, contents.
Bytes element(std::uint8_t tag, std::span<const std::byte> contents);

/// The encodings of `parts` one after another.
Bytes concat(std::initializer_list<std::span<const std::byte>> parts);

/// SEQUENCE of the already encoded `elements`, in their order.
Bytes sequence(std::initializer_list<std::span<const std::byte>> elements);

/// SET OF the already encoded `elements`, sorted as DER wants it (by their encodings), under `tag`:
/// tag_set, or e.g. context(0) for an [0] IMPLICIT SET OF.
Bytes set_of(std::vector<Bytes> elements, std::uint8_t tag = tag_set);

/// INTEGER of a non-negative value.
Bytes integer(std::uint64_t value);

/// OBJECT IDENTIFIER from dotted text, e.g. "1.2.840.113549.1.7.2". Throws FormatError when malformed.
Bytes object_identifier(std::string_view dotted);

Bytes octet_string(std::span<const std::byte> contents);
Bytes null();
/// BIT STRING of whole bytes (no unused bits).
Bytes bit_string(std::span<const std::byte> contents);
Bytes utf8_string(std::string_view text);
Bytes printable_string(std::string_view text);
/// UTCTime "YYMMDDHHMMSSZ", which covers 1950 to 2049. Throws FormatError outside of it.
Bytes utc_time(std::chrono::sys_seconds time);

/// Bytes of a string, for spans.
std::span<const std::byte> bytes_of(std::string_view text);

// MARK: - Reading

struct Element {
    std::uint8_t tag = 0;
    /// The contents: for an indefinite length the bytes up to, not including, the end-of-contents.
    std::span<const std::byte> contents;
    /// The whole element as encoded, from the tag to the end (with the end-of-contents octets).
    std::span<const std::byte> encoding;
    bool constructed() const { return (tag & 0x20) != 0; }
};

/// Reads the element at the start of `bytes`. Throws FormatError when it is malformed or longer
/// than `bytes`, when the tag number needs more than one byte, or when it nests too deeply.
Element read(std::span<const std::byte> bytes);

/// The elements inside a constructed element, in order. Throws FormatError when `element` is
/// primitive or its contents are not a list of whole elements.
std::vector<Element> children(const Element &element);

/// The child `index` of `element`, which must have `tag`. Throws FormatError otherwise.
Element child(const Element &element, std::size_t index, std::uint8_t tag);

/// An OBJECT IDENTIFIER element as dotted text.
std::string object_identifier_text(const Element &element);

/// The time of a UTCTime or GeneralizedTime element ("...Z" forms only, as X.509 requires).
std::chrono::sys_seconds time(const Element &element);

}  // namespace apk::der
