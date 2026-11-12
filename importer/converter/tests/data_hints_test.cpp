// The hints that an x18 refusal carries without symbols (apkconv/data_hints.h): the FDEs of .eh_frame, the PC-relative
// loads of code, and the section layout, on a small library built field by field.
#include "apkconv/data_hints.h"

#include "elf_builder.h"
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace apk::testing;

constexpr std::uint32_t ret_x18 = 0xd65f'0240;

/// One RX segment 0..0x400 with .eh_frame_hdr (0xe0), .eh_frame (0x100, one CIE "zR" pcrel sdata4 and one FDE over
/// [0x200, 0x20c)), .text (0x200..0x240) and .rodata (0x300). The code: an `ldr x0, <0x230>` at 0x200 (a PC-relative
/// load of the 8 bytes at 0x230), a ret x18 at 0x208 (inside the FDE), ret x18 at 0x210 (in no FDE, never loaded) and
/// at 0x230 (in no FDE, loaded).
apk::testing::ElfBuilder library(bool with_eh_frame, std::uint64_t eh_frame_size = 0x2c) {
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    builder.put<std::uint32_t>(0x200, 0x5800'0180);  // ldr x0, #0x30: the 8 bytes at 0x230
    builder.put<std::uint32_t>(0x204, 0xd65f'03c0);  // ret
    builder.put<std::uint32_t>(0x208, ret_x18);
    builder.put<std::uint32_t>(0x210, ret_x18);
    builder.put<std::uint32_t>(0x230, ret_x18);
    if (with_eh_frame) {
        const unsigned char cie[] = {0x10, 0, 0, 0,        // length 16
                                     0, 0, 0, 0,           // CIE id
                                     1, 'z', 'R', 0,       // version, augmentation "zR"
                                     0x04, 0x78, 0x1e,     // code alignment 4, data alignment -8, return register 30
                                     0x01, 0x1b,           // augmentation length 1: R = pcrel | sdata4
                                     0, 0, 0};             // DW_CFA_nop padding
        const unsigned char fde[] = {0x10, 0, 0, 0,        // length 16
                                     0x18, 0, 0, 0,        // CIE pointer: back from here to the CIE
                                     0xe4, 0, 0, 0,        // pc_begin: 0x200 - 0x11c, pcrel
                                     0x0c, 0, 0, 0,        // pc_range 12
                                     0, 0, 0, 0};          // augmentation length 0, padding
        std::memcpy(builder.bytes.data() + 0x100, cie, sizeof cie);
        std::memcpy(builder.bytes.data() + 0x114, fde, sizeof fde);
    }
    builder.sections({
        {".eh_frame_hdr", sht_progbits, shf_alloc, 0xe0, 0xe0, 0x10},
        {".eh_frame", sht_progbits, shf_alloc, 0x100, 0x100, eh_frame_size},
        {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, 0x80},
        {".rodata", sht_progbits, shf_alloc, 0x300, 0x300, 0x20},
    });
    return builder;
}

apk::hints::DataHints hints_of(const ElfBuilder &builder, const std::vector<apk::rewrite::Unsupported> &uses) {
    const auto file = apk::elf::read(builder.bytes);
    const auto sections = apk::elf::read_sections(builder.bytes);
    return apk::hints::data_hints(file, builder.bytes, sections, uses);
}

TEST(counts_the_sites_outside_every_fde_and_the_ones_a_load_reads) {
    // 0x20c is the first word after the FDE, 0x238 the first after the 8 bytes that the load reads.
    const std::vector<apk::rewrite::Unsupported> uses = {{0x230, "x", ret_x18}, {0x210, "x", ret_x18},
                                                         {0x208, "x", ret_x18}, {0x20c, "x", ret_x18},
                                                         {0x238, "x", ret_x18}};
    const auto hints = hints_of(library(true), uses);
    CHECK(hints.eh_frame_hdr);
    CHECK(hints.eh_frame);
    CHECK(hints.fdes == 1);
    CHECK(hints.sites == 5);
    CHECK(hints.sites_outside_fdes == 4);
    CHECK(hints.sites_loaded_as_data == 1);
    CHECK(hints.executable_sections == 1);
    CHECK(hints.other_bytes_in_executable_segments == 0x10 + 0x2c + 0x20);
    CHECK(hints.sites_in_executable_sections == 5);
    CHECK(apk::hints::describe(hints) ==
          "; unwind: .eh_frame_hdr yes, .eh_frame yes, 1 FDEs, 4 of the 5 sites outside every FDE; PC-relative loads: "
          "1 of the 5 sites loaded from code; sections: 1 executable, 92 bytes of other sections in executable "
          "segments, 5 of the 5 sites in executable sections; refused words: "
          "0x208 (.text, inside an FDE, not loaded, after a return), "
          "0x20c (.text, outside every FDE, not loaded, after a return), "
          "0x210 (.text, outside every FDE, not loaded, after another instruction), "
          "0x230 (.text, outside every FDE, loaded from code, after another instruction), "
          "0x238 (.text, outside every FDE, not loaded, after another instruction)");
}

TEST(a_register_that_adr_set_reaches_a_load_only_within_a_short_straight_line) {
    auto builder = library(true);
    for (const std::size_t at : {0x208u, 0x210u, 0x230u}) {  // no return in the way
        builder.put<std::uint32_t>(at, 0xd503'201f);
    }
    builder.put<std::uint32_t>(0x200, 0x1000'0181);  // adr x1, 0x230
    builder.put<std::uint32_t>(0x204, 0xf940'0020);  // ldr x0, [x1]: the 8 bytes at 0x230
    builder.put<std::uint32_t>(0x250, 0xf940'0420);  // ldr x0, [x1, #8]: 20 words after the adr, too far
    const std::vector<apk::rewrite::Unsupported> uses = {{0x230, "x", ret_x18}, {0x238, "x", ret_x18}};
    CHECK(hints_of(builder, uses).sites_loaded_as_data == 1);
    // A branch in between forgets the register.
    builder.put<std::uint32_t>(0x204, 0xd503'201f);  // nop
    builder.put<std::uint32_t>(0x208, 0x1400'0001);  // b .+4
    builder.put<std::uint32_t>(0x20c, 0xf940'0020);  // ldr x0, [x1]
    CHECK(hints_of(builder, uses).sites_loaded_as_data == 0);
}

TEST(a_library_without_unwind_tables_says_so_and_counts_no_site_outside_an_fde) {
    const std::vector<apk::rewrite::Unsupported> uses = {{0x230, "x", ret_x18}};
    const auto hints = hints_of(library(false), uses);
    CHECK(hints.eh_frame);          // the section is there, but holds no entry
    CHECK(hints.fdes == 0);
    CHECK(hints.sites_outside_fdes == 1);  // with no FDE at all, no word is covered
}

TEST(garbled_unwind_tables_never_leave_the_bytes_and_never_throw) {
    const std::vector<apk::rewrite::Unsupported> uses = {{0x230, "x", ret_x18}, {0x210, "x", ret_x18}};
    srandom(7);
    for (int round = 0; round < 3000; ++round) {
        auto builder = library(true);
        for (int flips = 1 + round % 6; flips > 0; --flips) {
            builder.bytes[0x100 + static_cast<std::size_t>(random()) % 0x2c] = static_cast<std::byte>(random() & 0xff);
        }
        (void)hints_of(builder, uses);  // address sanitizer and the undefined-behavior one are the check
    }
}

/// The library with its CIE replaced by `cie` (its length field and body, 28 bytes in all) and one FDE over
/// [0x200, 0x20c) after it, found through the CIE pointer 0x20; its .eh_frame is 0x40 bytes long.
ElfBuilder library_with_cie(const std::vector<unsigned char> &cie) {
    auto builder = library(false, 0x40);
    const unsigned char fde[] = {0x10, 0, 0, 0,        // length 16
                                 0x20, 0, 0, 0,        // CIE pointer: back from here to the CIE at 0x100
                                 0xdc, 0, 0, 0,        // pc_begin: 0x200 - 0x124, pcrel
                                 0x0c, 0, 0, 0,        // pc_range 12
                                 0, 0, 0, 0};          // augmentation length 0, padding
    CHECK(cie.size() == 28);
    std::memcpy(builder.bytes.data() + 0x100, cie.data(), cie.size());
    std::memcpy(builder.bytes.data() + 0x11c, fde, sizeof fde);
    return builder;
}

TEST(a_nine_byte_data_alignment_with_the_sign_bit_in_its_last_byte_reads_without_undefined_behavior) {
    // The data alignment 80 80 80 80 80 80 80 80 40 is -2^63 once sign-extended: no negation may overflow.
    const auto builder = library_with_cie({0x18, 0, 0, 0, 0, 0, 0, 0,       // length 24, CIE id
                                           1, 'z', 'R', 0,                  // version, augmentation "zR"
                                           0x04,                            // code alignment
                                           0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x40,  // data alignment
                                           0x1e, 0x01, 0x1b,                // return register, augmentation, R
                                           0, 0, 0});
    CHECK(hints_of(builder, {}).fdes == 1);
}

TEST(a_personality_in_a_form_it_cannot_read_gives_up_on_its_cie_instead_of_reading_on) {
    // "zPR" with P = 0x08 (no such form): R must not be taken from the personality's first byte.
    const auto builder = library_with_cie({0x18, 0, 0, 0, 0, 0, 0, 0,       // length 24, CIE id
                                           1, 'z', 'P', 'R', 0,             // version, augmentation "zPR"
                                           0x04, 0x78, 0x1e,                // alignments, return register
                                           0x0a,                            // augmentation length 10
                                           0x08, 0, 0, 0, 0, 0, 0, 0, 0,    // P: form 0x08, its value
                                           0x1b,                            // R: pcrel | sdata4
                                           0});
    CHECK(hints_of(builder, {}).fdes == 0);
}

/// The code `words` from 0x200, then a ret x18 at 0x210 (no site is asked about it); how many of a site at 0x230
/// the loads of `words` read, which is 0 or 1.
std::size_t loaded_after(const std::vector<std::uint32_t> &words) {
    auto builder = library(true);
    for (std::size_t at = 0x200; at < 0x240; at += 4) builder.put<std::uint32_t>(at, 0xd503'201f);  // nop
    std::size_t at = 0x200;
    for (const auto word : words) {
        builder.put<std::uint32_t>(at, word);
        at += 4;
    }
    return hints_of(builder, {{0x230, "x", ret_x18}}).sites_loaded_as_data;
}

TEST(a_load_that_reads_the_adr_target_through_a_register_nothing_wrote_counts_once) {
    CHECK(loaded_after({0x1000'0181, 0xf940'0020}) == 1);  // adr x1, 0x230; ldr x0, [x1]
}

TEST(the_second_register_of_a_load_pair_is_forgotten) {
    // adr x1, 0x230; ldp x0, x1, [x2]; ldr x0, [x1]: x1 holds loaded data by then.
    CHECK(loaded_after({0x1000'0181, 0xa940'0440, 0xf940'0020}) == 0);
}

TEST(a_writeback_changes_the_base_register_it_forgets) {
    // adr x1, 0x230; ldr x0, [x1], #8; ldr x0, [x1]: the second load reads 0x238.
    CHECK(loaded_after({0x1000'0181, 0xf840'8420, 0xf940'0020}) == 0);
    // The same with a pre-index store pair: adr x1; stp x2, x3, [x1, #16]!; ldr x0, [x1].
    CHECK(loaded_after({0x1000'0181, 0xa981'0c22, 0xf940'0020}) == 0);
    // And a post-indexed ld1: adr x1; ld1 {v0.16b}, [x1], #16; ldr x0, [x1].
    CHECK(loaded_after({0x1000'0181, 0x4cdf'7020, 0xf940'0020}) == 0);
}

TEST(register_31_is_never_tracked) {
    // adr xzr, 0x230 sets nothing, and sp as the base of a load is not xzr.
    CHECK(loaded_after({0x1000'019f, 0xf940'03e0}) == 0);
    // add sp, x1, #0 after an adr does not make sp known either.
    CHECK(loaded_after({0x1000'0181, 0x9100'003f, 0xf940'03e0}) == 0);
}

TEST(the_status_register_of_a_store_exclusive_is_forgotten) {
    // adr x1, 0x230; stxr w1, x2, [x3]; ldr x0, [x1].
    CHECK(loaded_after({0x1000'0181, 0xc801'7c62, 0xf940'0020}) == 0);
}

/// The NDK's hello fixture (musl, static, LLVM-built): a real library whose .text holds 229 `svc #0` words that are
/// all instructions, 189 of them after a $d mapping symbol (so no mapping-symbol split would do). The unwind tables
/// must cover them all, and every function start must be a covered address; without the fixture the test is skipped.
std::filesystem::path fixtures;

TEST(reads_the_unwind_tables_of_a_real_library) {
    const auto path = fixtures / "hello";
    if (!std::filesystem::exists(path)) {
        std::printf("skipped: no hello fixture\n");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    const auto file = apk::elf::read(bytes);
    const auto sections = apk::elf::read_sections(bytes);
    std::vector<apk::rewrite::Unsupported> svcs;
    for (const auto &section : sections) {
        if (section.name != ".text") continue;
        for (std::uint64_t at = 0; at + 4 <= section.size; at += 4) {
            std::uint32_t word;
            std::memcpy(&word, bytes.data() + section.offset + at, 4);
            if (word == 0xd400'0001) svcs.push_back({section.address + at, "svc", word});
        }
    }
    CHECK(svcs.size() == 229);
    const auto hints = apk::hints::data_hints(file, bytes, sections, svcs);
    CHECK(hints.eh_frame);
    CHECK(hints.eh_frame_hdr);
    CHECK(hints.fdes > 500);
    CHECK(hints.sites_outside_fdes == 0);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 2) fixtures = argv[1];
    return apk::testing::run_all();
}
