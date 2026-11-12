// The property of the data split itself: every byte inside a range the split proves to be data comes through a
// conversion unchanged. Run on the four libraries of the data-in-text fixture (data inside .text, once with symbols and
// three times stripped). The control: with the split switched off the same file is either refused or has at least one
// of those words rewritten, so that the comparison is not vacuous.
#include "apkconv/container.h"
#include "apkconv/data_ranges.h"
#include "apkconv/elf.h"

#include "check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path fixtures;
int libraries_found = 0;

std::vector<std::byte> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    return bytes;
}

/// The file offset of a loaded address, or SIZE_MAX when no segment holds it.
std::size_t file_offset(const apk::elf::File &file, std::uint64_t address) {
    for (const auto &segment : file.segments) {
        if (segment.type != apk::elf::SegmentType::load) continue;
        if (address >= segment.address && address < segment.address + segment.file_size) {
            return static_cast<std::size_t>(address - segment.address + segment.offset);
        }
    }
    return SIZE_MAX;
}

struct Comparison {
    std::uint64_t words = 0;
    std::uint64_t different = 0;
};

/// Compares the bytes of every data range of `scan` in `converted` with the input.
Comparison compare(const apk::elf::File &file, std::span<const std::byte> input,
                   const apk::container::Container &converted, const apk::data::DataRanges &data) {
    std::uint64_t min_vaddr = UINT64_MAX;
    for (const auto &segment : file.segments) {
        if (segment.type == apk::elf::SegmentType::load) min_vaddr = std::min(min_vaddr, segment.address);
    }
    min_vaddr &= ~(apk::elf::page_size_16k - 1);
    Comparison result;
    for (const auto &range : data.ranges) {
        for (std::uint64_t address = range.begin; address < range.end; address += 4) {
            const auto in = file_offset(file, address);
            const auto out = static_cast<std::size_t>(converted.report.image_offset + (address - min_vaddr));
            CHECK(in != SIZE_MAX);
            CHECK(in + 4 <= input.size());
            CHECK(out + 4 <= converted.bytes.size());
            ++result.words;
            if (std::memcmp(input.data() + in, converted.bytes.data() + out, 4) != 0) ++result.different;
        }
    }
    return result;
}

void check_library(const char *name) {
    const auto path = fixtures / name;
    if (!fs::exists(path)) {
        std::printf("missing: %s\n", path.c_str());
        return;
    }
    ++libraries_found;
    const auto input = read_file(path);
    const auto file = apk::elf::read(input);
    const auto scan = apk::container::scan_code(input);
    CHECK(!scan.data.ranges.empty());

    apk::container::Options options;
    options.install_name = "@rpath/data-split.dylib";
    const auto split = apk::container::convert(input, options);
    const auto kept = compare(file, input, split, scan.data);
    CHECK(kept.words > 0);
    CHECK(kept.different == 0);

    // Positive control: the data words, scanned as code, are refused or changed.
    options.data_split = false;
    bool refused = false;
    Comparison unsplit;
    try {
        const auto converted = apk::container::convert(input, options);
        unsplit = compare(file, input, converted, scan.data);
    } catch (const apk::container::Incompatible &) {
        refused = true;
    }
    CHECK(refused || unsplit.different > 0);
    std::printf("%s: %llu data words identical; without the split: %s\n", name,
                static_cast<unsigned long long>(kept.words),
                refused ? "refused" : (std::to_string(unsplit.different) + " words changed").c_str());
}

TEST(the_data_words_of_every_fixture_library_come_through_unchanged) {
    for (const char *name : {"libdit-sym.so", "libdit-strip.so", "libdit-rela.so", "libdit-relr.so"}) {
        check_library(name);
    }
}

// The same library with Android's packed relocations: the converter refuses it before the scan today (loud), but the
// scan reads the addends of the packed table, so a pointer that only a relocation holds still makes its stub
// reached: the data ranges and the sites are those of the plain RELA build of the same source.
TEST(a_library_with_packed_android_relocations_scans_as_the_plain_build_and_is_refused_by_convert) {
    const auto plain_path = fixtures / "libdit-rela.so";
    const auto packed_path = fixtures / "libdit-aps2.so";
    if (!fs::exists(plain_path) || !fs::exists(packed_path)) {
        std::printf("missing: libdit-rela.so or libdit-aps2.so\n");
        return;
    }
    ++libraries_found;
    const auto plain = read_file(plain_path);
    const auto packed = read_file(packed_path);
    const auto plain_scan = apk::container::scan_code(plain);
    const auto packed_scan = apk::container::scan_code(packed);
    CHECK(!packed_scan.data.ranges.empty());
    CHECK(packed_scan.data.ranges.size() == plain_scan.data.ranges.size());
    CHECK(packed_scan.data.literal_words == plain_scan.data.literal_words);
    CHECK(packed_scan.data.table_words == plain_scan.data.table_words);
    CHECK(apk::container::site_addresses(packed_scan).size() == apk::container::site_addresses(plain_scan).size());
    apk::container::Options options;
    options.install_name = "@rpath/data-split.dylib";
    bool refused = false;
    try {
        (void)apk::container::convert(packed, options);
    } catch (const apk::container::Incompatible &refusal) {
        refused = std::string(refusal.what()).find("packed Android relocations") != std::string::npos;
    }
    CHECK(refused);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 2) fixtures = argv[1];
    const int status = apk::testing::run_all();
    if (status == 0 && libraries_found == 0) return 77;
    return status;
}
