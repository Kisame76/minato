#include "apkconv/der.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

namespace apk::der {
namespace {

/// Nesting a certificate or CMS signature never gets near; the reader refuses deeper input
/// instead of running out of stack.
constexpr int maximum_depth = 32;

void append_length(Bytes &out, std::size_t length) {
    if (length < 0x80) {
        out.push_back(static_cast<std::byte>(length));
        return;
    }
    std::byte digits[sizeof length];
    std::size_t count = 0;
    for (auto rest = length; rest != 0; rest >>= 8) digits[count++] = static_cast<std::byte>(rest & 0xff);
    out.push_back(static_cast<std::byte>(0x80 | count));
    while (count > 0) out.push_back(digits[--count]);
}

void append(Bytes &out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// The element at `bytes` and its size; `depth` counts the enclosing indefinite-length elements.
Element read_at(std::span<const std::byte> bytes, int depth) {
    if (depth > maximum_depth) throw FormatError("the encoding nests too deeply");
    if (bytes.empty()) throw FormatError("empty input where an element should be");
    const auto tag = std::to_integer<std::uint8_t>(bytes[0]);
    if ((tag & 0x1f) == 0x1f) throw FormatError(std::format("tag {:#04x} needs more than one byte", tag));
    if (bytes.size() < 2) throw FormatError("the element has no length");
    const auto first = std::to_integer<std::uint8_t>(bytes[1]);
    std::size_t header = 2;

    if (first == 0x80) {
        // Indefinite length (BER): the contents are elements up to an end-of-contents (00 00).
        if ((tag & 0x20) == 0) throw FormatError("a primitive element with an indefinite length");
        std::size_t offset = header;
        for (;;) {
            if (bytes.size() - offset >= 2 && bytes[offset] == std::byte{0} && bytes[offset + 1] == std::byte{0}) {
                return Element{.tag = tag,
                               .contents = bytes.subspan(header, offset - header),
                               .encoding = bytes.first(offset + 2)};
            }
            if (offset >= bytes.size()) throw FormatError("an indefinite length without its end-of-contents");
            offset += read_at(bytes.subspan(offset), depth + 1).encoding.size();
        }
    }

    std::size_t length = first;
    if (first > 0x80) {
        const std::size_t count = first & 0x7f;
        if (count > 4) throw FormatError(std::format("a length of {} bytes is too large", count));
        if (bytes.size() < header + count) throw FormatError("the length is cut off");
        length = 0;
        for (std::size_t index = 0; index < count; ++index) {
            length = (length << 8) | std::to_integer<std::size_t>(bytes[header + index]);
        }
        header += count;
    }
    if (length > bytes.size() - header) {
        throw FormatError(std::format("the element is longer ({} bytes) than what is left ({} bytes)", length,
                                      bytes.size() - header));
    }
    return Element{.tag = tag, .contents = bytes.subspan(header, length), .encoding = bytes.first(header + length)};
}

int two_digits(std::string_view text, std::size_t at) {
    int value = 0;
    const auto *begin = text.data() + at;
    const auto result = std::from_chars(begin, begin + 2, value);
    if (result.ec != std::errc{} || result.ptr != begin + 2) throw FormatError("the time has a non-digit");
    return value;
}

}  // namespace

Bytes element(std::uint8_t tag, std::span<const std::byte> contents) {
    Bytes out;
    out.reserve(contents.size() + 6);
    out.push_back(static_cast<std::byte>(tag));
    append_length(out, contents.size());
    append(out, contents);
    return out;
}

Bytes concat(std::initializer_list<std::span<const std::byte>> parts) {
    Bytes out;
    for (const auto &part : parts) append(out, part);
    return out;
}

Bytes sequence(std::initializer_list<std::span<const std::byte>> elements) {
    return element(tag_sequence, concat(elements));
}

Bytes set_of(std::vector<Bytes> elements, std::uint8_t tag) {
    std::ranges::sort(elements);
    Bytes contents;
    for (const auto &item : elements) append(contents, item);
    return element(tag, contents);
}

Bytes integer(std::uint64_t value) {
    Bytes contents;
    for (auto rest = value; rest != 0; rest >>= 8) contents.insert(contents.begin(), static_cast<std::byte>(rest & 0xff));
    // A leading bit would make the value negative, and zero still takes one byte.
    if (contents.empty() || (std::to_integer<unsigned>(contents.front()) & 0x80) != 0) {
        contents.insert(contents.begin(), std::byte{0});
    }
    return element(tag_integer, contents);
}

Bytes object_identifier(std::string_view dotted) {
    std::vector<std::uint64_t> arcs;
    for (std::size_t start = 0; start <= dotted.size();) {
        const auto end = std::min(dotted.find('.', start), dotted.size());
        std::uint64_t arc = 0;
        const auto *begin = dotted.data() + start;
        const auto result = std::from_chars(begin, dotted.data() + end, arc);
        if (end == start || result.ec != std::errc{} || result.ptr != dotted.data() + end) {
            throw FormatError(std::format("\"{}\" is not an object identifier", dotted));
        }
        arcs.push_back(arc);
        start = end + 1;
    }
    if (arcs.size() < 2 || arcs[0] > 2 || (arcs[0] < 2 && arcs[1] > 39)) {
        throw FormatError(std::format("\"{}\" is not an object identifier", dotted));
    }
    arcs[1] += arcs[0] * 40;
    Bytes contents;
    for (auto arc = arcs.begin() + 1; arc != arcs.end(); ++arc) {
        std::byte groups[10];
        std::size_t count = 0;
        auto rest = *arc;
        do {
            groups[count++] = static_cast<std::byte>(rest & 0x7f);
            rest >>= 7;
        } while (rest != 0);
        while (count > 1) contents.push_back(groups[--count] | std::byte{0x80});
        contents.push_back(groups[0]);
    }
    return element(tag_object_identifier, contents);
}

Bytes octet_string(std::span<const std::byte> contents) {
    return element(tag_octet_string, contents);
}

Bytes null() {
    return element(tag_null, {});
}

Bytes bit_string(std::span<const std::byte> contents) {
    Bytes bits{std::byte{0}};
    append(bits, contents);
    return element(tag_bit_string, bits);
}

Bytes utf8_string(std::string_view text) {
    return element(tag_utf8_string, bytes_of(text));
}

Bytes printable_string(std::string_view text) {
    return element(tag_printable_string, bytes_of(text));
}

Bytes utc_time(std::chrono::sys_seconds time) {
    const auto day = std::chrono::floor<std::chrono::days>(time);
    const std::chrono::year_month_day date{day};
    const int year = static_cast<int>(date.year());
    if (year < 1950 || year > 2049) throw FormatError(std::format("{} does not fit a UTCTime", year));
    const std::chrono::hh_mm_ss clock{time - day};
    const auto text = std::format("{:02}{:02}{:02}{:02}{:02}{:02}Z", year % 100, static_cast<unsigned>(date.month()),
                                  static_cast<unsigned>(date.day()), clock.hours().count(),
                                  clock.minutes().count(), clock.seconds().count());
    return element(tag_utc_time, bytes_of(text));
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

Element read(std::span<const std::byte> bytes) {
    return read_at(bytes, 0);
}

std::vector<Element> children(const Element &element) {
    if (!element.constructed()) {
        throw FormatError(std::format("element {:#04x} is primitive and has no elements inside", element.tag));
    }
    std::vector<Element> result;
    for (auto rest = element.contents; !rest.empty();) {
        result.push_back(read_at(rest, 1));
        rest = rest.subspan(result.back().encoding.size());
    }
    return result;
}

Element child(const Element &element, std::size_t index, std::uint8_t tag) {
    const auto items = children(element);
    if (index >= items.size()) {
        throw FormatError(std::format("no element {} in {:#04x}, which holds {}", index, element.tag, items.size()));
    }
    if (items[index].tag != tag) {
        throw FormatError(std::format("element {} of {:#04x} has tag {:#04x}, not {:#04x}", index, element.tag,
                                      items[index].tag, tag));
    }
    return items[index];
}

std::string object_identifier_text(const Element &element) {
    if (element.tag != tag_object_identifier || element.contents.empty()) {
        throw FormatError("not an object identifier");
    }
    std::string text;
    std::uint64_t arc = 0;
    bool first = true;
    for (std::size_t index = 0; index < element.contents.size(); ++index) {
        const auto value = std::to_integer<std::uint64_t>(element.contents[index]);
        if (arc > (std::uint64_t{1} << 56)) throw FormatError("an object identifier arc is too large");
        arc = (arc << 7) | (value & 0x7f);
        if ((value & 0x80) != 0) {
            if (index + 1 == element.contents.size()) throw FormatError("an object identifier ends inside an arc");
            continue;
        }
        if (first) {
            const auto top = std::min<std::uint64_t>(arc / 40, 2);
            text = std::format("{}.{}", top, arc - top * 40);
            first = false;
        } else {
            text += std::format(".{}", arc);
        }
        arc = 0;
    }
    return text;
}

std::chrono::sys_seconds time(const Element &element) {
    const std::string_view text(reinterpret_cast<const char *>(element.contents.data()), element.contents.size());
    std::size_t at = 0;
    int year = 0;
    if (element.tag == tag_utc_time && text.size() == 13) {
        year = two_digits(text, 0);
        year += year < 50 ? 2000 : 1900;
        at = 2;
    } else if (element.tag == tag_generalized_time && text.size() == 15) {
        year = two_digits(text, 0) * 100 + two_digits(text, 2);
        at = 4;
    } else {
        throw FormatError("not a time in the form X.509 uses (YYMMDDHHMMSSZ or YYYYMMDDHHMMSSZ)");
    }
    if (text.back() != 'Z') throw FormatError("the time is not in UTC");
    const int month = two_digits(text, at);
    const int day = two_digits(text, at + 2);
    const int hour = two_digits(text, at + 4);
    const int minute = two_digits(text, at + 6);
    const int second = two_digits(text, at + 8);
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok() || hour > 23 || minute > 59 || second > 59) throw FormatError("the time is not a valid date");
    return std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
           std::chrono::seconds{second};
}

}  // namespace apk::der
