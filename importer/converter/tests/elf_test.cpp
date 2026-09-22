#include "apkconv/elf.h"

#include "check.h"

#include <cstring>
#include <initializer_list>
#include <random>
#include <string_view>
#include <vector>

namespace {

using apk::elf::FormatError;
using apk::elf::SegmentType;

constexpr std::uint32_t pt_load = 1;
constexpr std::uint32_t pt_interp = 3;
constexpr std::uint32_t pt_phdr = 6;
constexpr std::uint32_t pt_gnu_stack = 0x6474'e551;
constexpr std::uint32_t r = 4;
constexpr std::uint32_t w = 2;
constexpr std::uint32_t x = 1;

struct ProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t address;
    std::uint64_t file_size;
    std::uint64_t memory_size;
    std::uint64_t alignment;
};

/// Writes ELF files field by field at the offsets from the ELF gABI, independent of the reader's structs.
class ElfBuilder {
public:
    std::vector<std::byte> bytes = std::vector<std::byte>(0x300);

    ElfBuilder() {
        const unsigned char ident[] = {0x7f, 'E', 'L', 'F', 2 /* 64-bit */, 1 /* little-endian */, 1 /* version */};
        std::memcpy(bytes.data(), ident, sizeof ident);
        put<std::uint16_t>(16, 3);       // e_type: ET_DYN
        put<std::uint16_t>(18, 183);     // e_machine: EM_AARCH64
        put<std::uint32_t>(20, 1);       // e_version
        put<std::uint64_t>(24, 0x4200);  // e_entry
        put<std::uint16_t>(52, 64);      // e_ehsize
        put<std::uint16_t>(54, 56);      // e_phentsize
    }

    template <typename T>
    ElfBuilder &put(std::size_t offset, T value) {
        std::memcpy(bytes.data() + offset, &value, sizeof value);
        return *this;
    }

    /// Writes the program header table right after the file header.
    ElfBuilder &segments(std::initializer_list<ProgramHeader> headers) {
        put<std::uint64_t>(32, 64);  // e_phoff
        put<std::uint16_t>(56, static_cast<std::uint16_t>(headers.size()));
        std::size_t at = 64;
        for (const auto &header : headers) {
            put(at, header.type).put(at + 4, header.flags).put(at + 8, header.offset).put(at + 16, header.address);
            put(at + 24, header.address);  // p_paddr
            put(at + 32, header.file_size).put(at + 40, header.memory_size).put(at + 48, header.alignment);
            at += 56;
        }
        return *this;
    }

    ElfBuilder &text(std::size_t offset, std::string_view text) {
        std::memcpy(bytes.data() + offset, text.data(), text.size());
        return *this;
    }
};

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
            {pt_gnu_stack, r | w, 0, 0, 0, 0, 0},
        })
        .text(0x2c0, "/system/bin/linker64");
    const auto original = builder.bytes;
    CHECK(read(builder).interpreter == "/system/bin/linker64");

    const auto attempt = [](const std::vector<std::byte> &bytes) {
        try {
            static_cast<void>(apk::elf::first_misaligned_load(apk::elf::read(bytes)));
        } catch (const FormatError &) {
        }
    };
    for (std::size_t size = 0; size <= original.size(); ++size) {
        attempt(std::vector(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(size)));
    }
    constexpr std::size_t headers_end = 64 + 6 * 56;
    std::mt19937_64 random(20260922);
    for (int round = 0; round < 20000; ++round) {
        auto bytes = original;
        for (int change = 0; change <= round % 4; ++change) {
            // Mostly the headers, where a wrong number can mislead the reader.
            const auto limit = change == 0 ? headers_end : bytes.size();
            bytes[random() % limit] = static_cast<std::byte>(random() & 0xff);
        }
        attempt(bytes);
    }
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
