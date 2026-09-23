// Checks the DER writer against encodings worked out by hand from X.690, and the reader on
// definite and indefinite lengths and on broken input.
#include "apkconv/der.h"

#include "check.h"

#include <chrono>
#include <cstring>
#include <initializer_list>

namespace {

using namespace apk::der;
using namespace std::chrono;
namespace der = apk::der;

Bytes hex(std::initializer_list<unsigned> values) {
    Bytes bytes;
    for (const auto value : values) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

Bytes filled(std::size_t size, unsigned value) {
    return Bytes(size, static_cast<std::byte>(value));
}

TEST(integers_take_the_fewest_bytes_and_stay_positive) {
    CHECK(integer(0) == hex({0x02, 0x01, 0x00}));
    CHECK(integer(127) == hex({0x02, 0x01, 0x7f}));
    CHECK(integer(128) == hex({0x02, 0x02, 0x00, 0x80}));
    CHECK(integer(256) == hex({0x02, 0x02, 0x01, 0x00}));
    CHECK(integer(0xffff'ffff'ffff'ffff) ==
          hex({0x02, 0x09, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}));
}

TEST(object_identifiers_encode_the_first_two_arcs_together) {
    CHECK(object_identifier("1.2.840.113549.1.7.2") ==
          hex({0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02}));
    CHECK(object_identifier("2.16.840.1.101.3.4.2.1") ==
          hex({0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01}));
    CHECK(object_identifier("1.2.840.113635.100.9.2") ==
          hex({0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x63, 0x64, 0x09, 0x02}));
    CHECK(object_identifier("2.5.4.3") == hex({0x06, 0x03, 0x55, 0x04, 0x03}));
    CHECK_THROWS(FormatError, object_identifier("1"), "object identifier");
    CHECK_THROWS(FormatError, object_identifier("1..2"), "object identifier");
    CHECK_THROWS(FormatError, object_identifier("3.1"), "object identifier");
    CHECK_THROWS(FormatError, object_identifier("1.40"), "object identifier");
    CHECK_THROWS(FormatError, object_identifier("1.2.x"), "object identifier");
}

TEST(lengths_use_the_short_form_up_to_127) {
    CHECK(octet_string(filled(127, 1)).size() == 2 + 127);
    CHECK(octet_string(filled(127, 1))[1] == std::byte{0x7f});
    const auto medium = octet_string(filled(128, 1));
    CHECK(medium.size() == 3 + 128);
    CHECK((Bytes(medium.begin(), medium.begin() + 3) == hex({0x04, 0x81, 0x80})));
    const auto large = octet_string(filled(256, 1));
    CHECK((Bytes(large.begin(), large.begin() + 4) == hex({0x04, 0x82, 0x01, 0x00})));
    const auto huge = octet_string(filled(65536, 1));
    CHECK((Bytes(huge.begin(), huge.begin() + 5) == hex({0x04, 0x83, 0x01, 0x00, 0x00})));
}

TEST(simple_types) {
    CHECK(null() == hex({0x05, 0x00}));
    CHECK(bit_string(hex({0xab})) == hex({0x03, 0x02, 0x00, 0xab}));
    CHECK(utf8_string("G3") == hex({0x0c, 0x02, 'G', '3'}));
    CHECK(printable_string("US") == hex({0x13, 0x02, 'U', 'S'}));
    CHECK(sequence({null(), integer(1)}) == hex({0x30, 0x05, 0x05, 0x00, 0x02, 0x01, 0x01}));
    CHECK(element(context(0), integer(2)) == hex({0xa0, 0x03, 0x02, 0x01, 0x02}));
    CHECK(concat({null(), null()}) == hex({0x05, 0x00, 0x05, 0x00}));
}

TEST(utc_time_is_what_codesign_writes) {
    const sys_seconds time = sys_days{year{2026} / 9 / 23} + hours{2} + minutes{2} + seconds{28};
    const auto encoded = utc_time(time);
    CHECK(encoded.size() == 15);
    CHECK(encoded[0] == std::byte{0x17});
    CHECK(std::memcmp(encoded.data() + 2, "260923020228Z", 13) == 0);
    CHECK_THROWS(FormatError, utc_time(sys_days{year{2050} / 1 / 1}), "UTCTime");
    CHECK_THROWS(FormatError, utc_time(sys_days{year{1949} / 12 / 31}), "UTCTime");
}

TEST(set_of_sorts_by_encoding) {
    // Sorted by the whole encoding: first by tag, then length, then contents.
    CHECK(set_of({octet_string(hex({2})), octet_string(hex({1})), integer(5)}) ==
          hex({0x31, 0x09, 0x02, 0x01, 0x05, 0x04, 0x01, 0x01, 0x04, 0x01, 0x02}));
    CHECK(set_of({octet_string(hex({0, 0})), octet_string(hex({0}))}) ==
          hex({0x31, 0x07, 0x04, 0x01, 0x00, 0x04, 0x02, 0x00, 0x00}));
    // [0] IMPLICIT SET OF, as for the certificates of a CMS signature.
    CHECK(set_of({null()}, context(0)) == hex({0xa0, 0x02, 0x05, 0x00}));
    CHECK(set_of({}) == hex({0x31, 0x00}));
}

TEST(reads_definite_lengths) {
    const auto encoded = sequence({integer(300), object_identifier("1.2.840.113549.1.7.2"), null()});
    const auto outer = read(encoded);
    CHECK(outer.tag == tag_sequence);
    CHECK(outer.constructed());
    CHECK(outer.encoding.size() == encoded.size());
    const auto items = children(outer);
    CHECK(items.size() == 3);
    CHECK(items[0].tag == tag_integer);
    CHECK((Bytes(items[0].contents.begin(), items[0].contents.end()) == hex({0x01, 0x2c})));
    CHECK(object_identifier_text(items[1]) == "1.2.840.113549.1.7.2");
    CHECK(items[2].contents.empty());
    CHECK(object_identifier_text(child(outer, 1, tag_object_identifier)) == "1.2.840.113549.1.7.2");
    CHECK_THROWS(FormatError, child(outer, 1, tag_integer), "tag");
    CHECK_THROWS(FormatError, child(outer, 3, tag_null), "no element");
    CHECK_THROWS(FormatError, children(items[0]), "primitive");

    // Long-form length.
    const auto long_string = octet_string(filled(300, 7));
    CHECK(read(long_string).contents.size() == 300);
    // Trailing bytes after the element are not part of it.
    auto with_tail = null();
    with_tail.push_back(std::byte{0x42});
    CHECK(read(with_tail).encoding.size() == 2);
}

TEST(reads_indefinite_lengths_like_codesign_writes_them) {
    // SEQUENCE (indefinite) { OID, [0] (indefinite) { SEQUENCE (indefinite) { INTEGER 1 } } }
    const auto encoded = hex({0x30, 0x80, 0x06, 0x03, 0x55, 0x04, 0x03, 0xa0, 0x80, 0x30, 0x80, 0x02, 0x01,
                              0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00});
    const auto outer = read(encoded);
    CHECK(outer.encoding.size() == encoded.size() - 2);
    CHECK(outer.contents.size() == outer.encoding.size() - 4);
    const auto items = children(outer);
    CHECK(items.size() == 2);
    CHECK(object_identifier_text(items[0]) == "2.5.4.3");
    CHECK(items[1].tag == context(0));
    const auto inner = child(child(items[1], 0, tag_sequence), 0, tag_integer);
    CHECK(inner.contents.size() == 1);
}

TEST(reads_times) {
    const sys_seconds time = sys_days{year{2027} / 9 / 22} + hours{3} + minutes{13} + seconds{55};
    CHECK(der::time(read(utc_time(time))) == time);
    const auto generalized = element(tag_generalized_time, bytes_of("20350209214036Z"));
    CHECK(der::time(read(generalized)) == sys_days{year{2035} / 2 / 9} + hours{21} + minutes{40} + seconds{36});
    // UTCTime years 50-99 are 1950-1999.
    CHECK(der::time(read(element(tag_utc_time, bytes_of("991231235959Z")))) ==
          sys_days{year{1999} / 12 / 31} + hours{23} + minutes{59} + seconds{59});
    CHECK_THROWS(FormatError, der::time(read(element(tag_utc_time, bytes_of("2609230202Z")))), "time");
    CHECK_THROWS(FormatError, der::time(read(element(tag_utc_time, bytes_of("260923020228+0100")))), "time");
    CHECK_THROWS(FormatError, der::time(read(integer(1))), "time");
}

TEST(refuses_broken_input) {
    CHECK_THROWS(FormatError, read({}), "empty");
    CHECK_THROWS(FormatError, read(hex({0x30})), "length");
    CHECK_THROWS(FormatError, read(hex({0x04, 0x05, 0x01})), "longer");
    CHECK_THROWS(FormatError, read(hex({0x04, 0x81})), "length");
    CHECK_THROWS(FormatError, read(hex({0x04, 0x89, 1, 2, 3, 4, 5, 6, 7, 8, 9})), "length");
    CHECK_THROWS(FormatError, read(hex({0x1f, 0x22, 0x00})), "tag");
    CHECK_THROWS(FormatError, read(hex({0x04, 0x80, 0x00, 0x00})), "indefinite");
    CHECK_THROWS(FormatError, read(hex({0x30, 0x80, 0x05, 0x00})), "end");
    CHECK_THROWS(FormatError, children(read(hex({0x30, 0x03, 0x05, 0x00, 0x05}))), "length");
    CHECK_THROWS(FormatError, object_identifier_text(read(hex({0x06, 0x02, 0x2a, 0x86}))), "object identifier");

    // Deep nesting stops at a limit instead of exhausting the stack.
    Bytes deep;
    for (int level = 0; level < 100; ++level) {
        deep.push_back(std::byte{0x30});
        deep.push_back(std::byte{0x80});
    }
    CHECK_THROWS(FormatError, read(deep), "deep");

    // Every prefix of a valid encoding is refused, never read past its end.
    const auto encoded = sequence({integer(300), octet_string(filled(200, 1)), null()});
    for (std::size_t size = 0; size < encoded.size(); ++size) {
        CHECK_THROWS(FormatError, read(std::span(encoded).first(size)), "");
    }
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
