#include "apkconv/elf.h"

#include "check.h"
#include "elf_builder.h"

#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace apk::testing;
using apk::elf::FormatError;
using apk::elf::SegmentType;

/// Laid out like an NDK static PIE, only smaller: every LOAD segment is aligned to 16 KiB.
ElfBuilder static_pie() {
    ElfBuilder builder;
    builder.segments({
        {pt_phdr, r, 0x40, 0x40, 5 * 56, 5 * 56, 8},
        {pt_load, r, 0, 0, 0x200, 0x200, 0x4000},
        {pt_load, r | x, 0x200, 0x4200, 0x80, 0x80, 0x4000},
        {pt_load, r | w, 0x280, 0x8280, 0x40, 0x1000, 0x4000},
        {pt_gnu_stack, r | w, 0, 0, 0, 0, 0},
    });
    return builder;
}

apk::elf::File read(const ElfBuilder &builder) {
    return apk::elf::read(builder.bytes);
}

TEST(reads_the_file_header_and_the_segments) {
    const auto file = read(static_pie());
    CHECK(file.type == apk::elf::FileType::shared);
    CHECK(file.machine == apk::elf::machine_aarch64);
    CHECK(file.entry == 0x4200);
    CHECK(file.interpreter.empty());
    CHECK(file.segments.size() == 5);
    CHECK(file.segments[0].type == SegmentType::phdr);
    const auto &text = file.segments[2];
    CHECK(text.type == SegmentType::load);
    CHECK(text.flags == (r | x));
    CHECK(text.offset == 0x200);
    CHECK(text.address == 0x4200);
    CHECK(text.file_size == 0x80);
    CHECK(text.memory_size == 0x80);
    CHECK(text.alignment == 0x4000);
    CHECK(file.segments[3].memory_size == 0x1000);
    CHECK(file.segments[4].type == SegmentType::gnu_stack);
}

TEST(reads_the_interpreter) {
    ElfBuilder builder;
    builder.segments({{pt_interp, r, 0x100, 0x100, 21, 21, 1}}).text(0x100, "/system/bin/linker64");
    CHECK(read(builder).interpreter == "/system/bin/linker64");
}

TEST(rejects_an_interpreter_without_terminating_zero) {
    ElfBuilder builder;
    builder.segments({{pt_interp, r, 0x100, 0x100, 4, 4, 1}}).text(0x100, "/sys");
    CHECK_THROWS(FormatError, read(builder), "no terminating zero");
}

TEST(rejects_files_that_are_not_64_bit_little_endian_elf) {
    const auto valid = static_pie();
    CHECK_THROWS(FormatError, apk::elf::read(std::span(valid.bytes).first(63)), "too few");
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint8_t>(0, 0x7e)), "not an ELF file");
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint8_t>(4, 1)), "32-bit");
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint8_t>(4, 3)), "unknown ELF class");
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint8_t>(5, 2)), "big-endian");
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint8_t>(6, 0)), "unknown ELF version");
}

TEST(rejects_a_program_header_table_outside_the_file) {
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint64_t>(32, 0x2f0)), "outside the file");
    // An offset that overflows once the size of the table is added.
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint64_t>(32, ~std::uint64_t{0} - 8)), "outside the file");
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint16_t>(54, 32)), "32 bytes instead of 56");
    CHECK_THROWS(FormatError, read(static_pie().put<std::uint16_t>(56, 0xffff)), "65534");
}

TEST(rejects_segments_outside_the_file) {
    ElfBuilder past_end;
    past_end.segments({{pt_load, r, 0x200, 0, 0x101, 0x101, 0x4000}});
    CHECK_THROWS(FormatError, read(past_end), "segment #0 (0x101 bytes at offset 0x200) lies outside the file");

    ElfBuilder overflowing;
    overflowing.segments({{pt_load, r, ~std::uint64_t{0} - 0xff, 0, 0x200, 0x200, 0x4000}});
    CHECK_THROWS(FormatError, read(overflowing), "lies outside the file");
}

TEST(rejects_malformed_load_segments) {
    ElfBuilder larger_in_file;
    larger_in_file.segments({{pt_load, r, 0, 0, 0x200, 0x100, 0x4000}});
    CHECK_THROWS(FormatError, read(larger_in_file), "more bytes in the file than in memory");

    ElfBuilder odd_alignment;
    odd_alignment.segments({{pt_load, r, 0, 0, 0x200, 0x200, 0x3000}});
    CHECK_THROWS(FormatError, read(odd_alignment), "alignment 0x3000, not a power of two");
}

TEST(checks_the_16k_page_alignment) {
    CHECK(apk::elf::first_misaligned_load(read(static_pie())) == nullptr);

    ElfBuilder small_pages;  // 4 KiB, like NDK builds before r28
    small_pages.segments({
        {pt_load, r, 0, 0, 0x200, 0x200, 0x1000},
        {pt_load, r | x, 0x200, 0x1200, 0x80, 0x80, 0x1000},
    });
    const auto small = read(small_pages);
    CHECK(apk::elf::first_misaligned_load(small) == &small.segments[0]);
    CHECK(apk::elf::first_misaligned_load(small, 0x1000) == nullptr);

    ElfBuilder shifted_pages;  // aligned to 16 KiB, but address and file offset differ modulo 16 KiB
    shifted_pages.segments({
        {pt_load, r, 0, 0, 0x200, 0x200, 0x4000},
        {pt_load, r | x, 0x200, 0x5200, 0x80, 0x80, 0x4000},
    });
    const auto shifted = read(shifted_pages);
    CHECK(apk::elf::first_misaligned_load(shifted) == &shifted.segments[1]);
}

/// The sections of a small static PIE: code, data and bss, as the converter sees them.
ElfBuilder static_pie_with_sections() {
    auto builder = static_pie();
    builder.sections({
        {".rodata", sht_progbits, shf_alloc, 0x100, 0x100, 0x100},
        {".text", sht_progbits, shf_alloc | shf_execinstr, 0x4200, 0x200, 0x80},
        {".data", sht_progbits, shf_alloc | shf_write, 0x8280, 0x280, 0x40},
        {".bss", sht_nobits, shf_alloc | shf_write, 0x82c0, 0x2c0, 0xf40},
    });
    return builder;
}

TEST(reads_the_section_headers) {
    const auto sections = apk::elf::read_sections(static_pie_with_sections().bytes);
    CHECK(sections.size() == 6);  // the null section, four, .shstrtab
    CHECK(sections[0].name.empty());
    CHECK(sections[2].name == ".text");
    CHECK(sections[2].type == sht_progbits);
    CHECK(sections[2].flags == (shf_alloc | shf_execinstr));
    CHECK(sections[2].address == 0x4200);
    CHECK(sections[2].offset == 0x200);
    CHECK(sections[2].size == 0x80);
    CHECK(sections[4].name == ".bss");
    CHECK(sections[4].type == sht_nobits);
    CHECK(sections[5].name == ".shstrtab");
}

TEST(a_file_without_section_headers_has_no_sections) {
    CHECK(apk::elf::read_sections(static_pie().bytes).empty());
}

TEST(rejects_malformed_section_headers) {
    const auto valid = static_pie_with_sections();
    const auto table = [&] {
        std::uint64_t offset;
        std::memcpy(&offset, valid.bytes.data() + 40, sizeof offset);
        return static_cast<std::size_t>(offset);
    }();

    auto outside = valid;
    outside.put<std::uint64_t>(40, outside.bytes.size() - 64);
    CHECK_THROWS(FormatError, apk::elf::read_sections(outside.bytes), "section header table lies outside the file");

    auto wrong_size = valid;
    wrong_size.put<std::uint16_t>(58, 40);
    CHECK_THROWS(FormatError, apk::elf::read_sections(wrong_size.bytes), "40 bytes instead of 64");

    auto bad_names = valid;
    bad_names.put<std::uint16_t>(62, 9);
    CHECK_THROWS(FormatError, apk::elf::read_sections(bad_names.bytes), "section name table");

    auto name_outside = valid;
    name_outside.put<std::uint32_t>(table + 2 * 64, 0x7fff'ffff);
    CHECK_THROWS(FormatError, apk::elf::read_sections(name_outside.bytes), "name of section #2");

    auto contents_outside = valid;
    contents_outside.put<std::uint64_t>(table + 2 * 64 + 32, 0x10'0000);  // sh_size of .text
    CHECK_THROWS(FormatError, apk::elf::read_sections(contents_outside.bytes), "section #2 (.text)");

    // NOBITS sections occupy no bytes of the file, whatever their size.
    auto large_bss = valid;
    large_bss.put<std::uint64_t>(table + 4 * 64 + 32, 0x10'0000);
    CHECK(apk::elf::read_sections(large_bss.bytes)[4].size == 0x10'0000);
}

TEST(reads_the_dynamic_table) {
    ElfBuilder builder;
    builder.segments({{pt_load, r | w, 0x200, 0x4200, 0x80, 0x80, 0x4000}, {pt_dynamic, r | w, 0x200, 0x4200, 0x40, 0x40, 8}})
        .dynamic(0x200, {{0x6fff'e000, 0x368}, {0x6fff'e001, 0x68}, {0, 0}, {0x1e, 0x8}});
    const auto file = read(builder);
    const auto entries = apk::elf::read_dynamic(file, builder.bytes);
    CHECK(entries.size() == 2);  // ends at DT_NULL, not at the end of the segment
    CHECK(entries[0].tag == 0x6fff'e000);
    CHECK(entries[0].value == 0x368);
    CHECK(entries[1].tag == 0x6fff'e001);
    CHECK(entries[1].value == 0x68);

    CHECK(apk::elf::read_dynamic(read(static_pie()), static_pie().bytes).empty());  // no PT_DYNAMIC
}

TEST(finds_the_load_segment_of_an_address_range) {
    const auto file = read(static_pie());
    CHECK(apk::elf::load_containing(file, 0x4200, 0x80) == &file.segments[2]);
    CHECK(apk::elf::load_containing(file, 0x8280, 0x1000) == &file.segments[3]);  // bss counts
    CHECK(apk::elf::load_containing(file, 0x4270, 0x20) == nullptr);              // runs past the end
    CHECK(apk::elf::load_containing(file, 0x3000, 1) == nullptr);                 // in no segment
    CHECK(apk::elf::load_containing(file, ~std::uint64_t{0}, 2) == nullptr);      // overflows
}

/// Whatever the bytes, read() either succeeds or throws FormatError. ASan and UBSan (macOS preset)
/// catch reads outside the file, which is why every attempt gets a buffer of exactly its size.
TEST(survives_truncated_and_corrupted_files) {
    ElfBuilder builder;
    builder
        .segments({
            {pt_phdr, r, 0x40, 0x40, 6 * 56, 6 * 56, 8},
            {pt_load, r, 0, 0, 0x200, 0x200, 0x4000},
            {pt_load, r | x, 0x200, 0x4200, 0x80, 0x80, 0x4000},
            {pt_load, r | w, 0x280, 0x8280, 0x40, 0x1000, 0x4000},
            {pt_interp, r, 0x2c0, 0x82c0, 21, 21, 1},
            {pt_dynamic, r | w, 0x280, 0x8280, 0x40, 0x40, 8},
        })
        .text(0x2c0, "/system/bin/linker64")
        .dynamic(0x280, {{0x6fff'e000, 0x100}, {0x6fff'e001, 0x10}, {0, 0}})
        .sections({
            {".text", sht_progbits, shf_alloc | shf_execinstr, 0x4200, 0x200, 0x80},
            {".dynamic", sht_progbits, shf_alloc | shf_write, 0x8280, 0x280, 0x40},
        });
    const auto original = builder.bytes;
    CHECK(read(builder).interpreter == "/system/bin/linker64");
    CHECK(apk::elf::read_sections(original).size() == 4);

    const auto attempt = [](const std::vector<std::byte> &bytes) {
        try {
            const auto file = apk::elf::read(bytes);
            static_cast<void>(apk::elf::first_misaligned_load(file));
            static_cast<void>(apk::elf::read_dynamic(file, bytes));
        } catch (const FormatError &) {
        }
        try {
            static_cast<void>(apk::elf::read_sections(bytes));
        } catch (const FormatError &) {
        }
    };
    for (std::size_t size = 0; size <= original.size(); ++size) {
        attempt(std::vector(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(size)));
    }
    constexpr std::size_t headers_end = 64 + 6 * 56;
    const std::size_t section_table = original.size() - 4 * 64;
    std::mt19937_64 random(20260922);
    for (int round = 0; round < 20000; ++round) {
        auto bytes = original;
        for (int change = 0; change <= round % 4; ++change) {
            // Mostly the headers, where a wrong number can mislead the reader: every other round the
            // program headers, else the section header table.
            std::size_t at;
            if (change == 0 && round % 2 == 0) {
                at = random() % headers_end;
            } else if (change == 0) {
                at = section_table + random() % (4 * 64);
            } else {
                at = random() % bytes.size();
            }
            bytes[at] = static_cast<std::byte>(random() & 0xff);
        }
        attempt(bytes);
    }
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
