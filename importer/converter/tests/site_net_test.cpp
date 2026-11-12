// The net under the converter's syscall rewrite: for every library, the `svc` words before the import equal the
// ones the import rewrote, kept as data or refused, address by address, so no site is lost without a word.
//
// The oracle is a plain scan of the executable sections for the words that decode as `svc #imm`; it shares nothing
// with the rewriter's scan. The converter's own account of each word (rewritten site, data kept verbatim, refused)
// must cover that set exactly, and the container must show it: a rewritten site is a branch to its veneer, a word
// kept as data is unchanged, and no other `svc` stands in the executable sections of the image.
//
// The synthetic libraries always run. With a directory of built libraries as the argument, every AArch64 ELF file in
// it runs too (the NDK fixtures of tools/build-fixtures.sh).
#include "apkconv/container.h"
#include "apkconv/elf.h"
#include "apkconv/rewrite.h"

#include "check.h"
#include "elf_builder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace apk::testing;
using Addresses = std::set<std::uint64_t>;

constexpr std::uint32_t svc_mask = 0xffe0'001f;
constexpr std::uint32_t svc = 0xd400'0001;
constexpr std::uint32_t nop = 0xd503'201f;
constexpr std::uint32_t ret = 0xd65f'03c0;

std::filesystem::path fixtures_dir;

std::uint32_t word_at(const std::vector<std::byte> &bytes, std::uint64_t offset) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

bool is_branch(std::uint32_t word) {
    return (word & 0xfc00'0000) == 0x1400'0000;
}

/// What the converter scans: an allocated section of instructions that has bytes in the file.
bool holds_code(const apk::elf::Section &section) {
    constexpr auto both = shf_alloc | shf_execinstr;
    return (section.flags & both) == both && section.type != sht_nobits;
}

/// The oracle: the address of every word that decodes as svc #imm in a section that holds code.
Addresses svc_words(const std::vector<std::byte> &elf) {
    Addresses found;
    for (const auto &section : apk::container::layout_sections(elf)) {
        if (!holds_code(section)) continue;
        for (std::uint64_t at = 0; at + 4 <= section.size; at += 4) {
            if ((word_at(elf, section.offset + at) & svc_mask) == svc) found.insert(section.address + at);
        }
    }
    return found;
}

/// Where the ELF address lies in the file: the section that holds it.
std::uint64_t file_offset(const std::vector<std::byte> &elf, std::uint64_t address) {
    for (const auto &section : apk::container::layout_sections(elf)) {
        if (section.address <= address && address < section.address + section.size) {
            return section.offset + (address - section.address);
        }
    }
    throw std::runtime_error("an address in no section");
}

struct Account {
    Addresses rewritten, kept, refused;
};

Account account_of(const apk::rewrite::Findings &findings) {
    Account account;
    for (const auto &site : findings.sites) {
        if (site.kind == apk::rewrite::SiteKind::syscall) account.rewritten.insert(site.address);
    }
    account.kept.insert(findings.data_syscalls.begin(), findings.data_syscalls.end());
    account.refused.insert(findings.other_syscalls.begin(), findings.other_syscalls.end());
    return account;
}

/// Whether `name`'s library keeps the books: the three groups of the converter's account are disjoint, together they
/// are the oracle's set, and the container carries them out. Returns false when the converter refused the file.
bool check_no_site_lost(const std::string &name, const std::vector<std::byte> &elf) {
    const auto raw = svc_words(elf);
    apk::container::CodeScan scan;
    try {
        scan = apk::container::scan_code(elf);
    } catch (const apk::container::Incompatible &) {
        // No section headers or a broken layout: the converter refuses the file whole, so no site is rewritten.
        bool refused = false;
        try {
            apk::container::convert(elf, {.install_name = "@rpath/net.dylib"});
        } catch (const apk::container::Incompatible &) {
            refused = true;
        }
        CHECK(refused);
        return false;
    }
    const auto account = account_of(scan.findings);
    Addresses all = account.rewritten;
    all.insert(account.kept.begin(), account.kept.end());
    all.insert(account.refused.begin(), account.refused.end());
    // Disjoint: no word is counted twice, so the sums below are the counts of the report.
    CHECK(account.rewritten.size() + account.kept.size() + account.refused.size() == all.size());
    if (all != raw) {
        std::printf("%s: svc words before the import %zu, accounted for %zu\n", name.c_str(), raw.size(), all.size());
        for (const auto at : raw) {
            if (!all.contains(at)) std::printf("  lost: %#llx\n", static_cast<unsigned long long>(at));
        }
        for (const auto at : all) {
            if (!raw.contains(at)) std::printf("  invented: %#llx\n", static_cast<unsigned long long>(at));
        }
    }
    CHECK(all == raw);
    // An svc #0 is always a syscall: it is never data and never refused.
    for (const auto at : raw) {
        if (word_at(elf, file_offset(elf, at)) == svc) CHECK(account.rewritten.contains(at));
    }

    apk::container::Container container;
    try {
        container = apk::container::convert(elf, {.install_name = "@rpath/net.dylib"});
    } catch (const apk::container::Incompatible &error) {
        const std::string reason = error.what();
        if (!account.refused.empty()) {
            // The refusal names the first word and counts the rest, so the number refused can be read back.
            CHECK(reason.starts_with("svc #"));
            const auto more = account.refused.size() - 1;
            const auto counted = reason.find(" (and ");
            if (more == 0) {
                CHECK(counted == std::string::npos || reason.find(" more)") == std::string::npos);
            } else {
                CHECK(counted != std::string::npos);
                CHECK(reason.find(" (and " + std::to_string(more) + " more)") != std::string::npos);
            }
        }
        return false;
    }
    // A converted file has refused none.
    CHECK(account.refused.empty());
    CHECK(container.report.syscalls == account.rewritten.size());
    const auto image = container.report.image_offset;
    for (const auto at : account.rewritten) {
        CHECK(is_branch(word_at(container.bytes, image + file_offset(elf, at))));
    }
    for (const auto at : account.kept) {
        CHECK(word_at(container.bytes, image + file_offset(elf, at)) == word_at(elf, file_offset(elf, at)));
    }
    // And nothing else in the code of the image is an svc: a word the rewrite left in place is a site lost.
    std::size_t left = 0;
    for (const auto &section : apk::container::layout_sections(elf)) {
        if (!holds_code(section)) continue;
        for (std::uint64_t at = 0; at + 4 <= section.size; at += 4) {
            if ((word_at(container.bytes, image + section.offset + at) & svc_mask) == svc) ++left;
        }
    }
    CHECK(left == account.kept.size());
    return true;
}

constexpr std::int64_t dt_relr = 36, dt_relrsz = 35, dt_relrent = 37;
constexpr std::uint32_t sht_dynamic = 6, sht_relr = 19;

/// A small static PIE laid out like the NDK's, with `words` as its code at 0x200 (at most 40 words) and, when
/// `more` is not empty, a second executable section right behind it.
std::vector<std::byte> library(const std::vector<std::uint32_t> &words, const std::vector<std::uint32_t> &more = {}) {
    ElfBuilder builder;
    const std::uint64_t first = words.size() * 4;
    const std::uint64_t size = first + more.size() * 4;
    builder.put<std::uint64_t>(24, 0x200);
    builder
        .segments({
            {pt_phdr, r, 0x40, 0x40, 5 * 56, 5 * 56, 8},
            {pt_load, r | x, 0, 0, 0x200 + size, 0x200 + size, 0x4000},
            {pt_load, r | w, 0x2c0, 0x42c0, 0x40, 0x1000, 0x4000},
            {pt_dynamic, r | w, 0x2c0, 0x42c0, 0x40, 0x40, 8},
            {pt_gnu_stack, r | w, 0, 0, 0, 0, 0},
        })
        .put<std::uint64_t>(0x160, 0x4800)
        .dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {dt_relrent, 8}, {0, 0}});
    std::memcpy(builder.bytes.data() + 0x200, words.data(), static_cast<std::size_t>(first));
    if (!more.empty()) std::memcpy(builder.bytes.data() + 0x200 + first, more.data(), more.size() * 4);
    builder.sections({
        {".relr.dyn", sht_relr, shf_alloc, 0x160, 0x160, 8},
        {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, first},
        {".text.more", sht_progbits, shf_alloc | shf_execinstr, 0x200 + first, 0x200 + first, more.size() * 4},
        {".dynamic", sht_dynamic, shf_alloc | shf_write, 0x42c0, 0x2c0, 0x40},
        {".bss", sht_nobits, shf_alloc | shf_write, 0x4300, 0x300, 0xf80},
    });
    return builder.bytes;
}

constexpr std::uint32_t mov_x8_93 = 0xd280'0ba8;       // mov x8, #93
constexpr std::uint32_t svc_1 = 0xd400'0021;           // svc #1
constexpr std::uint32_t ldr_x0_literal_8 = 0x5800'0040;  // ldr x0, pc+8

TEST(counts_every_plain_syscall) {
    std::vector<std::uint32_t> code;
    for (int index = 0; index < 7; ++index) {
        code.push_back(index % 2 == 0 ? svc : nop);
        code.push_back(svc);
    }
    code.push_back(ret);
    const auto elf = library(code);
    CHECK(svc_words(elf).size() == 11);
    CHECK(check_no_site_lost("plain", elf));
}

TEST(counts_a_raw_syscall_with_an_immediate) {
    const auto elf = library({mov_x8_93, svc_1, svc, ret});
    CHECK(svc_words(elf).size() == 2);
    CHECK(check_no_site_lost("raw", elf));
}

TEST(counts_an_svc_that_a_literal_load_reads_as_data) {
    // ldr x0, =pool; ret; pool: svc #1 (the load reads it, nothing runs it), then an svc #0 for the converter to take.
    const auto elf = library({ldr_x0_literal_8, ret, svc_1, svc, ret});
    CHECK(svc_words(elf).size() == 2);
    CHECK(check_no_site_lost("literal", elf));
}

TEST(counts_the_svc_it_refuses_and_names_how_many) {
    const std::vector<std::uint32_t> one = {nop, svc_1, ret};
    CHECK(!check_no_site_lost("one refused", library(one)));
    const std::vector<std::uint32_t> three = {svc_1, svc, 0xd400'0041, nop, 0xd400'0061, ret};
    CHECK(!check_no_site_lost("three refused", library(three)));
}

TEST(counts_an_svc_that_is_the_last_word_of_its_section) {
    const auto elf = library({nop, svc});
    CHECK(svc_words(elf).size() == 1);
    CHECK(check_no_site_lost("last word", elf));
}

TEST(counts_an_svc_zero_that_a_literal_load_names) {
    // The converter takes every svc #0 for a syscall, also one that code loads as data: it is rewritten and counted.
    const auto elf = library({ldr_x0_literal_8, ret, svc, ret});
    CHECK(svc_words(elf).size() == 1);
    CHECK(check_no_site_lost("literal svc 0", elf));
}

TEST(counts_the_syscalls_of_every_executable_section) {
    const auto elf = library({svc, ret}, {svc, mov_x8_93, svc_1, svc, ret});
    CHECK(svc_words(elf).size() == 4);
    CHECK(check_no_site_lost("two sections", elf));
}

TEST(sees_a_library_without_an_svc) {
    CHECK(check_no_site_lost("none", library({nop, ret})));
}

TEST(refuses_a_file_without_section_headers_whole) {
    auto elf = library({svc, ret});
    // e_shnum and e_shoff to zero: nothing to scan.
    const std::uint16_t zero16 = 0;
    const std::uint64_t zero64 = 0;
    std::memcpy(elf.data() + 40, &zero64, sizeof zero64);
    std::memcpy(elf.data() + 60, &zero16, sizeof zero16);
    CHECK(!check_no_site_lost("no sections", elf));
}

TEST(every_built_library_keeps_its_books) {
    if (fixtures_dir.empty() || !std::filesystem::is_directory(fixtures_dir)) {
        std::printf("note: no built libraries, only the synthetic ones ran\n");
        return;
    }
    std::size_t files = 0, converted = 0, with_svc = 0;
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::directory_iterator(fixtures_dir)) {
        if (entry.is_regular_file()) paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    for (const auto &path : paths) {
        std::ifstream stream(path, std::ios::binary);
        std::vector<std::byte> bytes(std::filesystem::file_size(path));
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream || bytes.size() < 20 || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0) continue;
        std::uint16_t machine;
        std::memcpy(&machine, bytes.data() + 18, sizeof machine);
        if (machine != 183) continue;
        ++files;
        const auto raw = svc_words(bytes);
        if (!raw.empty()) ++with_svc;
        if (check_no_site_lost(path.filename().string(), bytes)) ++converted;
    }
    std::printf("note: %zu libraries, %zu with an svc, %zu converted\n", files, with_svc, converted);
    CHECK(files > 0);
    CHECK(with_svc > 0);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1) fixtures_dir = argv[1];
    return apk::testing::run_all();
}
