// Checks the scanner and the stubs of the rewriter (ADR-0010, ADR-0019) against code that the host
// assembler and linker produced (rewrite_reference.S): every emitted word, including branch and
// page offsets, must match what the real toolchain writes for the same instructions.
#include "apkconv/rewrite.h"

#include "apkconv/a64.h"
#include "check.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
extern const std::uint32_t ref_thread_pointer_read_x5[];
extern const std::uint32_t ref_thread_pointer_write_x7[];
extern const std::uint32_t ref_thread_pointer_write_x16[];
extern const std::uint32_t ref_thread_pointer_write_xzr[];
extern const std::uint32_t ref_syscall_veneer[];
extern const std::uint32_t ref_site[];
extern const std::uint32_t ref_after_site[];
extern const std::uint32_t ref_patterns[];
extern const std::uint32_t ref_patterns_end[];
extern const std::uint32_t ref_x18[];
extern const std::uint32_t ref_x18_end[];
extern const std::uint32_t ref_x18_island_read_write[];
extern const std::uint32_t ref_x18_island_read[];
extern const std::uint32_t ref_x18_island_write[];
extern const std::uint32_t ref_x18_islands[];
extern const std::uint32_t ref_x18_moves_sp[];
extern const std::uint32_t ref_x18_below_sp[];
extern const std::uint32_t ref_x18_branches[];
extern const std::uint32_t ref_x18_thread_pointer[];
extern const std::uint32_t ref_x18_without_field[];
extern const std::uint32_t ref_x18_unsupported_end[];
extern const std::uint32_t ref_x18_ll_sc[];
extern const std::uint32_t ref_x18_ll_sc_end[];
extern const std::uint32_t ref_x18_window_exit[];
extern const std::uint32_t ref_x18_window_input[];
extern const std::uint32_t ref_x18_window_exit_next[];
extern const std::uint32_t ref_x18_window_base[];
extern const std::uint32_t ref_x18_window_flags_read[];
extern const std::uint32_t ref_x18_window_skipped_write[];
extern const std::uint32_t ref_x18_window_status[];
extern const std::uint32_t ref_x18_window_end[];
extern const std::uint32_t ref_x18_window_clrex_only[];
extern const std::uint32_t ref_x18_window_clrex_only_end[];
extern const std::uint32_t ref_x18_window_unread[];
extern const std::uint32_t ref_x18_window_unread_end[];
extern const std::uint32_t ref_x18_island_cbz[];
extern const std::uint32_t ref_x18_island_tbz[];
extern const std::uint32_t ref_x18_island_indirect_br[];
extern const std::uint32_t ref_x18_island_indirect_br_end[];
extern const std::uint32_t ref_x18_island_indirect_blr[];
extern const std::uint32_t ref_x18_island_indirect_blr_end[];
extern const std::uint32_t ref_x18_site_adrp[];
extern const std::uint32_t ref_x18_island_adrp[];
extern const std::uint32_t ref_x18_site_adr[];
extern const std::uint32_t ref_x18_island_adr[];
extern const std::uint32_t ref_x18_site_ldr[];
extern const std::uint32_t ref_x18_island_ldr[];
extern const std::uint32_t ref_x18_site_ldr_w[];
extern const std::uint32_t ref_x18_island_ldr_w[];
extern const std::uint32_t ref_x18_site_ldrsw[];
extern const std::uint32_t ref_x18_island_ldrsw[];
extern const std::uint32_t ref_x18_jump_function[];
extern const std::uint32_t ref_x18_jump_site[];
extern const std::uint32_t ref_x18_jump_case0[];
extern const std::uint32_t ref_x18_jump_case1[];
extern const std::uint32_t ref_x18_jump_case2[];
extern const std::uint32_t ref_x18_jump_function_end[];
extern const std::int32_t ref_x18_jump_table[];
extern const std::uint32_t ref_x18_island_jump[];
extern const std::uint32_t ref_x18_island_jump_end[];
extern std::uint64_t ref_syscall_slot;
}

namespace {

using apk::rewrite::Site;
using apk::rewrite::SiteKind;

std::uint64_t address_of(const void *pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

std::uint32_t word(const std::vector<std::byte> &bytes, std::size_t index) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + 4 * index, sizeof value);
    return value;
}

namespace oracle {
// A brute-force step-D soundness check (defined after the fixtures below): under the real limits scan()
// islands no x18 use on a live LL/SC path EXCEPT as a no-save island (which spills nothing, touching only
// the TSD granule C3b keeps the monitor across) over register(s) proven dead -- any other island on a live
// path, and a no-save island whose register is not dead, is unsound. scan_words() runs it on every fixture
// (ADR-0020 step D and C7, the lead's condition 2 and step-7 claim).
struct Toggles {
    bool call_return;   // ON: a call or return (BL, BLR, RET) ends the monitor path (the C6 limit)
    bool section_exit;  // ON: the section end, or a branch out of it, pairs nothing (one output section)
};
std::vector<std::uint64_t> flags(const std::uint32_t *begin, const std::uint32_t *end, std::uint64_t base, Toggles t);
}  // namespace oracle

/// Scans the words from `begin` to `end` as if they sat at `address`.
apk::rewrite::Findings scan_words(const std::uint32_t *begin, const std::uint32_t *end, std::uint64_t address,
                                  std::source_location location = std::source_location::current()) {
    const std::span code(reinterpret_cast<const std::byte *>(begin), 4 * static_cast<std::size_t>(end - begin));
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, address, findings);
    // The step-D oracle also runs on every fixture: under the real limits scan() must island no x18 use
    // on a live LL/SC path (ADR-0020 step D). flags() re-scans with apk::rewrite::scan, not scan_words.
    if (const auto unsound = oracle::flags(begin, end, address, {true, true}); !unsound.empty()) {
        apk::testing::fail(location, "the step-D oracle flagged an unsound x18 island at ",
                           std::format("{:#x}", unsound.front()).c_str());
    }
    return findings;
}

/// Names the word at `address` of the words scanned from `begin` at `base`, for a failure message.
std::string describe(const std::uint32_t *begin, std::uint64_t base, std::uint64_t address) {
    return std::format("{:#010x} (word {})", begin[(address - base) / 4], (address - base) / 4);
}

/// Emits the stub of `site` as if the site sat at `site_address` and the stub at `stub_address`.
std::pair<std::uint32_t, std::vector<std::byte>> emit_at(const Site &site, std::uint64_t site_address,
                                                         std::uint64_t stub_address) {
    std::vector<std::byte> stub(apk::rewrite::stub_size(site));
    const auto patch =
        apk::rewrite::emit(site, site_address, stub_address, address_of(&ref_syscall_slot), stub);
    return {patch, stub};
}

TEST(scan_finds_the_three_patterns_and_nothing_else) {
    const auto size = address_of(ref_patterns_end) - address_of(ref_patterns);
    const std::span code(reinterpret_cast<const std::byte *>(ref_patterns), size);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x1000, findings);

    const std::vector<Site> expected = {
        {0x1000, SiteKind::thread_pointer_read, 3},
        {0x1004, SiteKind::thread_pointer_write, 9},
        {0x1008, SiteKind::syscall, 0},
        {0x100c, SiteKind::thread_pointer_read, 30},
        {0x1010, SiteKind::thread_pointer_write, 31},
    };
    CHECK(findings.sites == expected);
    CHECK(findings.other_syscalls == std::vector<std::uint64_t>{0x1014});
}

// A guest mrs TPIDRRO_EL0 reads the Darwin TSD base the x18 islands use as scratch (slot 11). Android never reads
// it (XNU/Linux leave it zero at EL0), so it is 0 over the corpus; the scan lists it so the converter refuses the
// file. The destination register is caught too when it is x18: that form writes x18, so without the dedicated
// check it would be taken for an ordinary x18 write and islanded, reading the real base into the guest's x18.
TEST(scan_lists_every_tpidrro_read_so_the_file_is_refused) {
    const std::uint32_t words[] = {
        0xd53b'd065,  // mrs x5,  TPIDRRO_EL0
        0xd53b'd072,  // mrs x18, TPIDRRO_EL0 (writes x18: still a read of the base, refuse, not an x18 island)
    };
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x2000, findings);
    CHECK(findings.tpidrro_reads == (std::vector<std::uint64_t>{0x2000, 0x2004}));
    CHECK(findings.sites.empty());       // never an x18 island
    CHECK(findings.x18_writes.empty());  // the x18-destination form is not counted as an x18 write
}

// A TPIDRRO read inside a load-/store-exclusive window with an x18 use is refused too. The window machinery
// would otherwise copy the read into a relocated island and run it, handing the guest the TSD base, so the
// check must fire wherever the read sits, not only on instructions the per-use scan still reaches.
TEST(a_tpidrro_read_inside_an_exclusive_window_is_still_refused) {
    const std::uint32_t words[] = {
        0xc85f'7c01,  // ldxr x1, [x0]
        0x9100'0432,  // add x18, x1, #1   (an x18 use: the window is a relocation candidate)
        0xd53b'd065,  // mrs x5, TPIDRRO_EL0   (inside the window)
        0xc802'7c12,  // stxr w2, x18, [x0]
    };
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x3000, findings);
    CHECK(findings.tpidrro_reads == (std::vector<std::uint64_t>{0x3008}));
}

TEST(scan_sorts_x18_into_reads_writes_and_register_dumps) {
    const auto size = address_of(ref_x18_end) - address_of(ref_x18);
    const std::span code(reinterpret_cast<const std::byte *>(ref_x18), size);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x2000, findings);

    CHECK(findings.x18_writes == (std::vector<std::uint64_t>{0x2000, 0x2004}));
    CHECK(findings.x18_reads == (std::vector<std::uint64_t>{0x2008, 0x200c, 0x2028, 0x2034}));
    CHECK(findings.x18_saves == (std::vector<std::uint64_t>{0x2014, 0x2020}));
    // Each of them runs in an island with another register (ADR-0020), the register dumps too.
    std::vector<std::uint64_t> islands;
    for (const auto &site : findings.sites) {
        CHECK(site.kind == SiteKind::x18);
        islands.push_back(site.address);
    }
    CHECK(islands == (std::vector<std::uint64_t>{0x2000, 0x2004, 0x2008, 0x200c, 0x2014, 0x2020, 0x2028, 0x2034}));
    CHECK(findings.x18_unsupported.empty());
}

TEST(a_register_dump_at_the_edge_of_the_code_is_a_read) {
    // stp x18, x19, [x0, #0x90] as the first and as the last word: one neighbour is missing.
    const std::uint32_t words[] = {0xa909'4c12, 0xa90a'5414, 0xa908'4410, 0xa909'4c12};
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x3000, findings);
    CHECK(findings.x18_reads == (std::vector<std::uint64_t>{0x3000, 0x300c}));
    CHECK(findings.x18_saves.empty());
}

TEST(scan_rejects_code_that_is_not_word_aligned) {
    const std::vector<std::byte> code(8);
    apk::rewrite::Findings findings;
    CHECK_THROWS(apk::rewrite::RewriteError, apk::rewrite::scan(code, 0x1002, findings), "aligned");
    CHECK_THROWS(apk::rewrite::RewriteError, apk::rewrite::scan(std::span(code).first(6), 0x1000, findings),
                 "aligned");
}

// svc #imm (imm != 0): Linux ignores the immediate and takes the number from x8, so a reachable one is a syscall;
// a data word in .text can also decode as svc #imm. The converter veneers it only on the raw-syscall shape (x8 set
// by a move-immediate, MOV-register or load in straight-line code just before it), keeps it verbatim when it is an
// LDR-literal target, and otherwise refuses (ADR-0019).
TEST(scan_veneers_a_reachable_svc_imm_as_a_syscall) {
    const std::uint32_t words[] = {0xd2800548 /* movz x8, #42 */, 0xd4024681 /* svc #0x1234 */};
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x1000, findings);
    CHECK(findings.other_syscalls.empty() && findings.data_syscalls.empty());
    CHECK(std::ranges::any_of(findings.sites, [](const apk::rewrite::Site &s) {
        return s.kind == apk::rewrite::SiteKind::syscall && s.address == 0x1004;
    }));
}

TEST(scan_keeps_a_svc_imm_that_is_a_literal_target_as_data) {
    const std::uint32_t words[] = {0x58000040 /* ldr x0, .+8 */, 0xd503201f /* nop */, 0xd4024681 /* svc #0x1234 */};
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x1000, findings);
    CHECK(findings.data_syscalls == (std::vector<std::uint64_t>{0x1008}));
    CHECK(findings.other_syscalls.empty());
}

TEST(scan_refuses_a_lone_svc_imm) {
    const std::uint32_t words[] = {0xd4024681 /* svc #0x1234 */};
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x1000, findings);
    CHECK(findings.other_syscalls == (std::vector<std::uint64_t>{0x1000}));
    CHECK(findings.data_syscalls.empty());
}

TEST(scan_refuses_a_svc_imm_with_a_branch_after_the_x8_write) {
    const std::uint32_t words[] = {0xd2800548 /* movz x8, #42 */, 0x14000001 /* b .+4 */, 0xd4024681 /* svc #0x1234 */};
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x1000, findings);
    CHECK(findings.other_syscalls == (std::vector<std::uint64_t>{0x1008}));
}

// Only a move-immediate, the MOV-register alias or a load into x8 proves the number: an instruction that merely
// writes x8 (here add x8, x9, #1) does not, as a random data word decodes as some x8 writer about one time in eight.
TEST(scan_refuses_a_svc_imm_whose_x8_write_is_not_a_syscall_number_setter) {
    const std::uint32_t words[] = {0x91000528 /* add x8, x9, #1 */, 0xd4024681 /* svc #0x1234 */};
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x1000, findings);
    CHECK(findings.other_syscalls == (std::vector<std::uint64_t>{0x1004}));
    CHECK(findings.data_syscalls.empty());
}

// The final branch of an island goes back to the instruction after the site. Placing the island
// so that this branch lands on the `b _ref_after_site` of the reference veneer (its 6th word) makes
// the linker's branch the expected value.
TEST(the_thread_pointer_read_island_matches_the_assembler) {
    const Site site{0, SiteKind::thread_pointer_read, 5};
    const auto stub_address = address_of(ref_syscall_veneer) + 5 * 4 - 3 * 4;
    const auto [patch, stub] = emit_at(site, address_of(ref_site), stub_address);
    CHECK(stub.size() == 16);
    for (std::size_t index = 0; index < 3; ++index) CHECK(word(stub, index) == ref_thread_pointer_read_x5[index]);
    CHECK(word(stub, 3) == ref_syscall_veneer[5]);  // b _ref_after_site
}

TEST(a_thread_pointer_read_into_xzr_becomes_a_nop) {
    const Site site{0, SiteKind::thread_pointer_read, 31};
    CHECK(apk::rewrite::stub_size(site) == 0);
    const auto [patch, stub] = emit_at(site, 0x4000, 0x1000);
    CHECK(patch == 0xd503'201f);
}

TEST(the_thread_pointer_write_islands_match_the_assembler) {
    const struct {
        std::uint8_t reg;
        const std::uint32_t *reference;
    } cases[] = {{7, ref_thread_pointer_write_x7},
                 {16, ref_thread_pointer_write_x16},
                 {31, ref_thread_pointer_write_xzr}};
    for (const auto &test : cases) {
        const Site site{0, SiteKind::thread_pointer_write, test.reg};
        const auto [patch, stub] = emit_at(site, address_of(ref_site), address_of(ref_syscall_veneer));
        CHECK(stub.size() == 24);
        for (std::size_t index = 0; index < 5; ++index) CHECK(word(stub, index) == test.reference[index]);
        CHECK(word(stub, 5) == ref_syscall_veneer[5]);
    }
}

TEST(the_syscall_veneer_and_the_patched_site_match_the_linker) {
    const Site site{0, SiteKind::syscall, 0};
    const auto [patch, stub] = emit_at(site, address_of(ref_site), address_of(ref_syscall_veneer));
    CHECK(stub.size() == 24);
    for (std::size_t index = 0; index < 6; ++index) CHECK(word(stub, index) == ref_syscall_veneer[index]);
    CHECK(patch == ref_site[0]);  // b _ref_syscall_veneer
}

// x18 islands (ADR-0020): placed where the reference island is, so that the final branch back to
// the instruction after the site is the one the linker resolved.
TEST(the_x18_islands_match_the_assembler) {
    const struct {
        std::uint32_t instruction;
        const std::uint32_t *reference;
        std::size_t words;
    } cases[] = {
        {0x8b01'0252, ref_x18_island_read_write, 12},  // add x18, x18, x1
        {0xf900'0012, ref_x18_island_read, 7},          // str x18, [x0]
        {0xaa13'03f2, ref_x18_island_write, 9},         // mov x18, x19
    };
    for (const auto &test : cases) {
        const Site site{0, SiteKind::x18, 18, test.instruction};
        const auto [patch, stub] = emit_at(site, address_of(ref_site), address_of(test.reference));
        CHECK(stub.size() == 4 * test.words);
        if (stub.size() != 4 * test.words) continue;
        for (std::size_t index = 0; index < test.words; ++index) CHECK(word(stub, index) == test.reference[index]);
    }
}

// (b) ADR-0020 C7: the no-save island for a read use loads the guest x18 from TSD slot 11 into the dead
// value register, runs the substituted use, and branches back -- no red-zone save, no store-back. Its
// layout tells reconstruction there is no base register and no store word, so it reads neither.
TEST(the_no_save_island_loads_x18_into_the_dead_register_and_runs_the_use) {
    const Site site{0, SiteKind::x18_nosave, 5, 0x8b12'0023};  // add x3, x1, x18, value register x5

    const auto layout = apk::rewrite::island_layout(site);
    CHECK(layout.kind == APK_ISLAND_SINGLE_NOSAVE);
    CHECK(layout.words == 5);                       // mrs, bic, ldr, the use, the branch back
    CHECK(layout.value == 5);
    CHECK(layout.base == APK_ISLAND_NO_REGISTER);   // no second register
    CHECK(layout.store == 0);                       // a read never stores x18 back
    CHECK(layout.commit == 3);                      // the use is word 3 (mrs 0, bic 1, ldr 2)
    CHECK(layout.resume == 1);                      // after the commit, on to site + 4

    const auto [patch, stub] = emit_at(site, address_of(ref_site), address_of(ref_x18));
    CHECK(stub.size() == 5 * 4);
    if (stub.size() == 5 * 4) {
        CHECK(word(stub, 0) == 0xd53b'd065u);                   // mrs x5, TPIDRRO_EL0
        CHECK(word(stub, 2) == 0xf940'2ca5u);                   // ldr x5, [x5, #88]  (TSD slot 11)
        CHECK(word(stub, 3) == 0x8b05'0023u);                   // add x3, x1, x5  (x18 -> x5)
        CHECK((word(stub, 4) & 0xfc00'0000u) == 0x1400'0000u);  // B back to the site
    }
    CHECK((patch & 0xfc00'0000u) == 0x1400'0000u);              // the site branches to the island
}

// (b) ADR-0020 C7: a write use's no-save island adds a store-back. After running the use in the dead
// value register it loads the TSD base into a second dead register and stores the new x18 to slot 11, so
// the guest's thread pointer stays coherent. `base` and `store` tell reconstruction where the store-back
// is; between the commit and the store x18 lives in the value register.
TEST(the_no_save_island_for_a_write_use_stores_x18_back_through_a_second_dead_register) {
    const Site site{0, SiteKind::x18_nosave, 5, 0x8b03'0032, 6};  // add x18, x1, x3; value x5, base x6

    const auto layout = apk::rewrite::island_layout(site);
    CHECK(layout.kind == APK_ISLAND_SINGLE_NOSAVE);
    CHECK(layout.words == 8);                       // mrs, bic, ldr, the use, mrs, bic, str, the branch back
    CHECK(layout.value == 5);
    CHECK(layout.base == 6);                        // the second dead register, the store-back TSD base
    CHECK(layout.commit == 3);                      // the use is word 3 (mrs 0, bic 1, ldr 2)
    CHECK(layout.store == 6);                        // str x5, [x6, #88] is word 6 (mrs 4, bic 5)
    CHECK(layout.resume == 1);                       // after the commit, on to site + 4

    const auto [patch, stub] = emit_at(site, address_of(ref_site), address_of(ref_x18));
    CHECK(stub.size() == 8 * 4);
    if (stub.size() == 8 * 4) {
        CHECK(word(stub, 0) == 0xd53b'd065u);                   // mrs x5, TPIDRRO_EL0
        CHECK(word(stub, 2) == 0xf940'2ca5u);                   // ldr x5, [x5, #88]  (TSD slot 11)
        CHECK(word(stub, 3) == 0x8b03'0025u);                   // add x5, x1, x3  (x18 -> x5)
        CHECK(word(stub, 4) == 0xd53b'd066u);                   // mrs x6, TPIDRRO_EL0
        CHECK(word(stub, 6) == 0xf900'2cc5u);                   // str x5, [x6, #88]  (back to slot 11)
        CHECK((word(stub, 7) & 0xfc00'0000u) == 0x1400'0000u);  // B back to the site
    }
    CHECK((patch & 0xfc00'0000u) == 0x1400'0000u);              // the site branches to the island
}

// ADR-0020 option (c): the entry island of a renamed function loads the guest x18 from TSD slot 11 into the rename
// register (here x9), runs the function's first instruction (a plain prologue, unchanged), and branches back to
// begin + 4, so the renamed body that follows sees the guest x18 in x9.
TEST(the_c_entry_island_loads_x18_then_runs_the_first_instruction) {
    const std::uint32_t first = 0xa9bf'7bfd;  // stp x29, x30, [sp, #-16]! -- a normal prologue, not pc-relative
    std::vector<std::byte> stub(apk::rewrite::c_entry_stub_size(first));
    const auto patch = apk::rewrite::emit_c_entry(9, first, address_of(ref_site), address_of(ref_x18), stub);
    CHECK(stub.size() == 5 * 4);
    if (stub.size() == 5 * 4) {
        CHECK(word(stub, 0) == 0xd53b'd069u);                   // mrs x9, TPIDRRO_EL0
        CHECK(word(stub, 2) == 0xf940'2d29u);                   // ldr x9, [x9, #88]  (TSD slot 11)
        CHECK(word(stub, 3) == first);                          // the first instruction, unchanged
        CHECK((word(stub, 4) & 0xfc00'0000u) == 0x1400'0000u);  // B back to begin + 4
    }
    CHECK((patch & 0xfc00'0000u) == 0x1400'0000u);              // begin branches to the island
}

// A pc-relative first instruction cannot run in the island (its PC would change), so emit refuses it; the gate keeps
// such a function out of (c).
TEST(the_c_entry_island_refuses_a_pc_relative_first_instruction) {
    std::vector<std::byte> stub(apk::rewrite::c_entry_stub_size(0));
    CHECK_THROWS(apk::rewrite::RewriteError,
                 apk::rewrite::emit_c_entry(9, 0x9000'0009u /* adrp x9, . */, address_of(ref_site),
                                            address_of(ref_x18), stub),
                 "pc-relative");
}

// ADR-0020 option (c): the exit island of a renamed function stores the register back to TSD slot 11 before the
// function returns, so the architectural x18 is current when control leaves. x16 (the TSD base scratch) is preserved
// around the store with a push/pop, so the return itself runs with every register as the function left it.
TEST(the_c_exit_island_stores_x18_before_a_return) {
    const std::uint32_t ret = 0xd65f'03c0;  // ret x30
    std::vector<std::byte> stub(apk::rewrite::c_exit_stub_size(ret));
    const auto patch = apk::rewrite::emit_c_exit(9, ret, address_of(ref_site), address_of(ref_x18), stub);
    CHECK(stub.size() == 6 * 4);
    if (stub.size() == 6 * 4) {
        CHECK(word(stub, 1) == 0xd53b'd070u);  // mrs x16, TPIDRRO_EL0
        CHECK(word(stub, 3) == 0xf900'2e09u);  // str x9, [x16, #88]  (back to TSD slot 11)
        CHECK(word(stub, 5) == ret);           // the original ret, unchanged, after x16 is restored
    }
    CHECK((patch & 0xfc00'0000u) == 0x1400'0000u);  // the ret site branches to the island
}

// The exit island carries an indirect tail branch through unchanged, after the store.
TEST(the_c_exit_island_carries_an_indirect_tail_branch) {
    const std::uint32_t br = 0xd61f'0120;  // br x9
    std::vector<std::byte> stub(apk::rewrite::c_exit_stub_size(br));
    apk::rewrite::emit_c_exit(10, br, address_of(ref_site), address_of(ref_x18), stub);
    CHECK(word(stub, 3) == 0xf900'2e0au);  // str x10, [x16, #88]
    CHECK(word(stub, 5) == br);            // the original br, unchanged
}

// A direct branch (a tail b or a conditional) has a displacement that must be relocated, which this island does not
// do, so emit refuses it -- those take the tail/conditional-exit island.
TEST(the_c_exit_island_refuses_a_direct_branch) {
    std::vector<std::byte> stub(apk::rewrite::c_exit_stub_size(0));
    CHECK_THROWS(apk::rewrite::RewriteError,
                 apk::rewrite::emit_c_exit(9, 0x1400'0004u /* b .+16 */, address_of(ref_site), address_of(ref_x18),
                                           stub),
                 "direct");
}

std::uint64_t branch_target_at(const std::vector<std::byte> &stub, std::size_t index, std::uint64_t stub_address);

// ADR-0020 option (c): a renamed function that leaves through an unconditional tail B stores the register back to
// slot 11, then branches to the original target (relocated, so it still reaches it from the island).
TEST(the_c_branch_exit_island_relocates_an_unconditional_tail_branch) {
    const std::uint32_t tail = 0x1400'0010;  // b .+0x40: a tail branch out of the function
    std::vector<std::byte> stub(apk::rewrite::c_branch_exit_stub_size(tail));
    const auto patch = apk::rewrite::emit_c_branch_exit(9, tail, address_of(ref_site), address_of(ref_x18), stub);
    CHECK(stub.size() == 6 * 4);
    if (stub.size() == 6 * 4) {
        CHECK(word(stub, 1) == 0xd53b'd070u);  // mrs x16, TPIDRRO_EL0
        CHECK(word(stub, 3) == 0xf900'2e09u);  // str x9, [x16, #88]  (back to slot 11 before leaving)
        CHECK(branch_target_at(stub, 5, address_of(ref_x18)) == address_of(ref_site) + 0x40);  // the original target
    }
    CHECK((patch & 0xfc00'0000u) == 0x1400'0000u);  // the branch site goes to the island
}

// A conditional exit: the store runs on both paths (harmless not taken), then the conditional skips two words ahead
// to a wide branch to its target; not taken, a branch back into the function after the site.
TEST(the_c_branch_exit_island_splits_a_conditional_exit) {
    const std::uint32_t cond = 0xb400'0209;  // cbz x9, .+0x40: taken leaves the function
    std::vector<std::byte> stub(apk::rewrite::c_branch_exit_stub_size(cond));
    const auto patch = apk::rewrite::emit_c_branch_exit(9, cond, address_of(ref_site), address_of(ref_x18), stub);
    CHECK(stub.size() == 8 * 4);
    if (stub.size() == 8 * 4) {
        CHECK(word(stub, 3) == 0xf900'2e09u);              // str x9, [x16, #88]: stored on both paths
        CHECK(((word(stub, 5) >> 5) & 0x7ffffu) == 2u);    // the cbz now skips two words ahead (to the taken branch)
        CHECK(branch_target_at(stub, 6, address_of(ref_x18)) == address_of(ref_site) + 4);     // not taken: after site
        CHECK(branch_target_at(stub, 7, address_of(ref_x18)) == address_of(ref_site) + 0x40);  // taken: the target
    }
    CHECK((patch & 0xfc00'0000u) == 0x1400'0000u);
}

// A BL is a call, not an exit (its sync goes around the call, step 3d), so the branch exit island refuses it.
TEST(the_c_branch_exit_island_refuses_a_call) {
    std::vector<std::byte> stub(apk::rewrite::c_branch_exit_stub_size(0x9400'0004u));
    CHECK_THROWS(apk::rewrite::RewriteError,
                 apk::rewrite::emit_c_branch_exit(9, 0x9400'0004u /* bl .+16 */, address_of(ref_site),
                                                  address_of(ref_x18), stub),
                 "call");
}

// ADR-0020 option (c): a call inside a renamed function stores the register to slot 11 before the call (a callee
// that reads the guest x18 sees it current), then reloads it after (the caller-saved register may be clobbered, and
// the callee may have updated x18). A direct BL is relocated to its target.
TEST(the_c_call_island_stores_before_and_reloads_after_a_direct_call) {
    const std::uint32_t call = 0x9400'0010;  // bl .+0x40
    std::vector<std::byte> stub(apk::rewrite::c_call_stub_size(call));
    const auto patch = apk::rewrite::emit_c_call(9, call, address_of(ref_site), address_of(ref_x18), stub);
    CHECK(stub.size() == 10 * 4);
    if (stub.size() == 10 * 4) {
        CHECK(word(stub, 1) == 0xd53b'd070u);  // mrs x16 (store: TSD base)
        CHECK(word(stub, 3) == 0xf900'2e09u);  // str x9, [x16, #88]  (slot 11 <- x9, before the call)
        CHECK((word(stub, 5) & 0xfc00'0000u) == 0x9400'0000u);  // still a BL (a call, not a plain branch)
        CHECK(branch_target_at(stub, 5, address_of(ref_x18)) == address_of(ref_site) + 0x40);  // its original target
        CHECK(word(stub, 6) == 0xd53b'd069u);  // mrs x9 (reload: TSD base in x9 itself)
        CHECK(word(stub, 8) == 0xf940'2d29u);  // ldr x9, [x9, #88]  (x9 <- slot 11, after the call)
        CHECK(branch_target_at(stub, 9, address_of(ref_x18)) == address_of(ref_site) + 4);  // back after the call
    }
    CHECK((patch & 0xfc00'0000u) == 0x1400'0000u);  // the call site goes to the island
}

// An indirect call (BLR) is carried through verbatim, between the same store and reload.
TEST(the_c_call_island_carries_an_indirect_call_verbatim) {
    const std::uint32_t call = 0xd63f'0140;  // blr x10
    std::vector<std::byte> stub(apk::rewrite::c_call_stub_size(call));
    apk::rewrite::emit_c_call(9, call, address_of(ref_site), address_of(ref_x18), stub);
    CHECK(word(stub, 3) == 0xf900'2e09u);  // str x9, [x16, #88] before
    CHECK(word(stub, 5) == call);          // the blr, unchanged
    CHECK(word(stub, 8) == 0xf940'2d29u);  // ldr x9, [x9, #88] after
}

// Only a BL or BLR is a call; anything else is not this island's job, so emit refuses it.
TEST(the_c_call_island_refuses_a_non_call) {
    std::vector<std::byte> stub(apk::rewrite::c_call_stub_size(0));
    CHECK_THROWS(apk::rewrite::RewriteError,
                 apk::rewrite::emit_c_call(9, 0xd65f'03c0u /* ret */, address_of(ref_site), address_of(ref_x18), stub),
                 "BL or a BLR");
}

TEST(scan_puts_x18_instructions_into_islands_and_names_the_forms_it_cannot) {
    const std::uint32_t code[] = {
        0x8b01'0252,  // add x18, x18, x1: island
        0xb400'0052,  // cbz x18, #+8: a two-way island (X2)
        0x9000'0012,  // adrp x18, #0: recomputed in the island (X2)
        0xd61f'0240,  // br x18: an indirect island (ends no jump table) (X2e)
        0x9100'025f,  // mov sp, x18: moves sp, which the island needs unmoved
        0xf81f'0ff2,  // str x18, [sp, #-16]!: writes below sp and moves it
        0xc85f'7c12,  // ldxr x18, [x0]: a lone load-exclusive (no stxr follows), so an island (X2e/C3)
        0x9100'0421,  // add x1, x1, #1: no x18
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(bytes, 0x1000, findings);
    const std::vector<Site> expected{{0x1000, SiteKind::x18, 18, 0x8b01'0252},
                                     {0x1004, SiteKind::x18, 18, 0xb400'0052},
                                     {0x1008, SiteKind::x18, 18, 0x9000'0012},
                                     {0x100c, SiteKind::x18, 18, 0xd61f'0240},   // br x18: indirect island
                                     {0x1018, SiteKind::x18, 18, 0xc85f'7c12}};  // lone ldxr x18: single island
    CHECK(findings.sites == expected);
    CHECK(findings.x18_unsupported.size() == 2);
    const std::uint64_t first_unsupported[] = {0x1010, 0x1014};
    for (std::size_t index = 0; index < findings.x18_unsupported.size() && index < 2; ++index) {
        CHECK(findings.x18_unsupported[index].address == first_unsupported[index]);
    }
}

// Register 31 is sp or xzr depending on the form. Only moving sp and addressing below it clash
// with the red-zone slots of the island; everything else next to sp or xzr goes into islands.
TEST(x18_next_to_sp_or_xzr_goes_into_islands) {
    const auto findings = scan_words(ref_x18_islands, ref_x18_moves_sp, 0x1000);
    for (const auto &entry : findings.x18_unsupported) {
        apk::testing::fail(std::source_location::current(), "refused: ",
                           (describe(ref_x18_islands, 0x1000, entry.address) + ": " + entry.reason).c_str());
    }
    const auto words = static_cast<std::size_t>(ref_x18_moves_sp - ref_x18_islands);
    CHECK(findings.sites.size() == words);
    for (std::size_t index = 0; index < findings.sites.size(); ++index) {
        CHECK(findings.sites[index].kind == SiteKind::x18);
        CHECK(findings.sites[index].address == 0x1000 + 4 * index);
    }
}

TEST(forms_no_island_handles_are_refused_with_the_reason) {
    const struct {
        const std::uint32_t *begin;
        const std::uint32_t *end;
        const char *reason;
    } groups[] = {
        {ref_x18_moves_sp, ref_x18_below_sp, "moves the stack pointer"},
        {ref_x18_below_sp, ref_x18_branches, "below the stack pointer"},
        {ref_x18_branches, ref_x18_thread_pointer, "branch"},
        {ref_x18_thread_pointer, ref_x18_without_field, "thread pointer"},
        {ref_x18_without_field, ref_x18_unsupported_end, "field"},
    };
    for (const auto &group : groups) {
        const auto findings = scan_words(group.begin, group.end, 0x1000);
        for (const auto &site : findings.sites) {
            apk::testing::fail(std::source_location::current(), "not refused: ",
                               describe(group.begin, 0x1000, site.address).c_str());
        }
        CHECK(findings.x18_unsupported.size() == static_cast<std::size_t>(group.end - group.begin));
        for (const auto &entry : findings.x18_unsupported) {
            if (std::string_view(entry.reason).find(group.reason) == std::string_view::npos) {
                apk::testing::fail(std::source_location::current(), "unexpected reason: ",
                                   (describe(group.begin, 0x1000, entry.address) + ": " + entry.reason).c_str());
            }
        }
    }
}

/// Real target of the B at word `index` of a stub that sits at `stub_address`.
std::uint64_t branch_target_at(const std::vector<std::byte> &stub, std::size_t index, std::uint64_t stub_address) {
    const auto instruction = word(stub, index);
    const auto immediate = static_cast<std::int64_t>(static_cast<std::int32_t>(instruction << 6) >> 6);
    return stub_address + 4 * index + 4 * static_cast<std::uint64_t>(immediate);
}

// A conditional branch that uses x18 runs in a two-way island (ADR-0020 X2): load the guest x18
// into the scratch, test it, and restore the scratch on both the taken and the not-taken exit.
TEST(the_conditional_branch_islands_match_the_assembler) {
    const struct {
        std::uint32_t instruction;  // the branch, with x18 and a +0x40 displacement
        const std::uint32_t *reference;
    } cases[] = {{0xb400'0212, ref_x18_island_cbz}, {0xb708'0212, ref_x18_island_tbz}};
    for (const auto &test : cases) {
        const Site site{0, SiteKind::x18, 18, test.instruction};
        const auto stub_address = address_of(test.reference);
        const auto [patch, stub] = emit_at(site, address_of(ref_site), stub_address);
        CHECK(stub.size() == 9 * 4);
        if (stub.size() != 9 * 4) continue;
        // Setup, the retargeted branch and the two restores are the same wherever the island sits.
        for (const std::size_t index : {0u, 1u, 2u, 3u, 4u, 5u, 7u}) CHECK(word(stub, index) == test.reference[index]);
        // The two exits: back is the instruction after the site, taken is the branch's own target.
        CHECK(branch_target_at(stub, 6, stub_address) == address_of(ref_site) + 4);
        CHECK(branch_target_at(stub, 8, stub_address) == address_of(ref_site) + 0x40);
    }
}

// A CMPBR on x18 runs in the same two-way island as CBZ/CBNZ (step C C2a): read guest x18 into the
// value scratch, run the compare-and-branch with x18 renamed, leave by the taken and not-taken exits.
// The host assembler has no CMPBR, so the island is checked by decoding, not against a reference; its
// value scratch is x0, as for cbz x18, so the setup and the two restores match the cbz reference.
TEST(a_cmpbr_on_x18_is_a_two_way_island) {
    const std::uint32_t cmpbr = 0xf4c6'0212;  // cmpbr cbeq x18, x6, .+0x40
    const auto findings = scan_words(&cmpbr, &cmpbr + 1, 0x1000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.sites.size() == 1);
    const Site site{0, SiteKind::x18, 18, cmpbr};
    CHECK(apk::rewrite::island_layout(site).value == 0);  // value is x0, as for the cbz island
    const auto stub_address = address_of(ref_x18_island_cbz);
    const auto [patch, stub] = emit_at(site, address_of(ref_site), stub_address);
    CHECK(stub.size() == 9 * 4);
    if (stub.size() != 9 * 4) return;
    for (const std::size_t index : {0u, 1u, 2u, 3u, 5u, 7u}) CHECK(word(stub, index) == ref_x18_island_cbz[index]);
    // Word 4: the cmpbr with x18 (Rt) renamed to x0, retargeted three words ahead to skip to the restore.
    const auto branch = word(stub, 4);
    CHECK(apk::a64::register_use(branch).reads == ((1u << 0) | (1u << 6)));  // x0 (was x18) and x6
    CHECK(((branch >> 25) & 0x3f) == 0b111010);                             // still a CMPBR
    CHECK((static_cast<std::int32_t>(branch << 18) >> 23) * 4 == 3 * 4);     // +12: skip to word 7
    CHECK(branch_target_at(stub, 6, stub_address) == address_of(ref_site) + 4);     // not taken: after the site
    CHECK(branch_target_at(stub, 8, stub_address) == address_of(ref_site) + 0x40);  // taken: the branch's target
}

// x18 as the second operand (Rm) of an asymmetric compare: the rename must keep the operand order,
// so x0 (the value scratch) lands in Rm and x1 stays in Rt. A swap would invert the comparison.
TEST(a_cmpbr_with_x18_as_the_second_operand_keeps_the_operand_order) {
    const std::uint32_t cmpbr = 0xf412'0201;  // cbgt x1, x18, .+0x40
    const auto findings = scan_words(&cmpbr, &cmpbr + 1, 0x1000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.sites.size() == 1);
    const Site site{0, SiteKind::x18, 18, cmpbr};
    CHECK(apk::rewrite::island_layout(site).value == 0);  // value is x0, distinct from the other operand x1
    const auto stub_address = address_of(ref_x18_island_cbz);
    const auto [patch, stub] = emit_at(site, address_of(ref_site), stub_address);
    CHECK(stub.size() == 9 * 4);
    if (stub.size() != 9 * 4) return;
    for (const std::size_t index : {0u, 1u, 2u, 3u, 5u, 7u}) CHECK(word(stub, index) == ref_x18_island_cbz[index]);
    // Word 4: x18 was the Rm operand; it renames to x0 while x1 stays in Rt. Fields, not just the read set.
    const auto branch = word(stub, 4);
    CHECK(apk::a64::register_use(branch).reads == ((1u << 1) | (1u << 0)));  // x1 (Rt) and x0 (was x18, Rm)
    CHECK((branch & 0x1f) == 1);                                             // Rt still x1, not swapped
    CHECK(((branch >> 16) & 0x1f) == 0);                                     // Rm now x0 (was x18)
    CHECK(((branch >> 25) & 0x3f) == 0b111010);                             // still a CMPBR
    CHECK((static_cast<std::int32_t>(branch << 18) >> 23) * 4 == 3 * 4);     // +12: skip to word 7
    CHECK(branch_target_at(stub, 6, stub_address) == address_of(ref_site) + 4);     // not taken: after the site
    CHECK(branch_target_at(stub, 8, stub_address) == address_of(ref_site) + 0x40);  // taken: the branch's target
}

// A PC-relative instruction on x18 runs in an island that computes the same address for its own
// place (ADR-0020 X2). Site and island are both assembled and linked, so every word of the island,
// the recomputed page and the branch back included, must be what the linker resolved.
TEST(the_pc_relative_islands_match_the_assembler) {
    const struct {
        const std::uint32_t *site;
        const std::uint32_t *reference;
        std::size_t words;
    } cases[] = {
        {ref_x18_site_adrp, ref_x18_island_adrp, 9},    // adrp: the page only
        {ref_x18_site_adr, ref_x18_island_adr, 10},     // adr: page and offset
        {ref_x18_site_ldr, ref_x18_island_ldr, 11},     // ldr x from a literal
        {ref_x18_site_ldr_w, ref_x18_island_ldr_w, 11}, // ldr w
        {ref_x18_site_ldrsw, ref_x18_island_ldrsw, 11}, // ldrsw
    };
    for (const auto &test : cases) {
        // The scanner puts the site into an island.
        const auto findings = scan_words(test.site, test.site + 1, 0x1000);
        CHECK(findings.x18_unsupported.empty());
        CHECK(findings.sites.size() == 1);

        const Site site{0, SiteKind::x18, 18, test.site[0]};
        const auto [patch, stub] = emit_at(site, address_of(test.site), address_of(test.reference));
        CHECK(stub.size() == 4 * test.words);
        if (stub.size() != 4 * test.words) continue;
        for (std::size_t index = 0; index < test.words; ++index) {
            if (word(stub, index) != test.reference[index]) {
                apk::testing::fail(std::source_location::current(), "word differs: ",
                                   std::format("{:#010x} at {}: {:#010x} expected", word(stub, index), index,
                                               test.reference[index]).c_str());
            }
        }
    }
}

TEST(two_way_and_indirect_branches_are_islands_but_other_register_branches_are_refused) {
    const std::uint32_t code[] = {
        0xb400'0212,  // cbz x18, .+8: two-way island
        0xb500'0212,  // cbnz x18: two-way island
        0x3618'0212,  // tbz w18, #3: two-way island
        0xb708'0212,  // tbnz x18, #33: two-way island
        0xf4c6'0212,  // cmpbr cbeq x18, x6, .+0x40: two-way island (step C C2a)
        0xd61f'0240,  // br x18: indirect island (step C C2b)
        0xd63f'0240,  // blr x18: indirect island (step C C2b)
        0xd65f'0240,  // ret x18: refused, the island cannot follow it
        0xd71f'0a40,  // braa x18, x0: refused (pointer authentication)
        0xd71f'0812,  // braa x0, x18: refused (x18 as the modifier)
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    std::vector<std::uint64_t> islands;
    for (const auto &s : findings.sites) islands.push_back(s.address);
    CHECK(islands == (std::vector<std::uint64_t>{0x1000, 0x1004, 0x1008, 0x100c, 0x1010, 0x1014, 0x1018}));
    CHECK(findings.x18_unsupported.size() == 3);
    for (const auto &entry : findings.x18_unsupported) {
        CHECK(std::string_view(entry.reason).find("register branch") != std::string_view::npos);
    }
}

// A br x18 that ends no jump table, and a blr x18, run in an indirect island (ADR-0020 X2e): the
// island loads the guest x18 into x16 (an ABI scratch register at a branch) and branches to it.
// ret x18 and the pointer-authenticated forms the island still cannot follow stay refused.
TEST(a_br_or_blr_x18_that_is_no_jump_table_is_an_indirect_island) {
    const std::uint32_t code[] = {
        0xd61f'0240,  // br x18:  indirect island (no jump table precedes it)
        0xd63f'0240,  // blr x18: indirect island
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.jump_tables.empty());
    CHECK(findings.sites.size() == 2);
    for (const auto &site : findings.sites) {
        CHECK(site.kind == SiteKind::x18);
        CHECK(apk::rewrite::island_layout(site).kind == APK_ISLAND_INDIRECT);
    }
}

// The emitted indirect islands match the assembler and the linker: BR x18 is five words ending in a
// br x16, and BLR x18 is eight words that also save x30 and recompute the guest return address so
// the callee returns into the guest, not the island (ADR-0020 X2e).
TEST(the_indirect_islands_match_the_assembler) {
    const struct {
        std::uint32_t instruction;
        const std::uint32_t *reference;
        const std::uint32_t *reference_end;
    } cases[] = {
        {0xd61f'0240, ref_x18_island_indirect_br, ref_x18_island_indirect_br_end},    // br x18
        {0xd63f'0240, ref_x18_island_indirect_blr, ref_x18_island_indirect_blr_end},  // blr x18
    };
    for (const auto &test : cases) {
        const auto findings = scan_words(&test.instruction, &test.instruction + 1, 0x1000);
        CHECK(findings.sites.size() == 1);
        const Site site{0, SiteKind::x18, 18, test.instruction};
        const auto words = static_cast<std::size_t>(test.reference_end - test.reference);
        const auto [patch, stub] = emit_at(site, address_of(ref_site), address_of(test.reference));
        CHECK(stub.size() == 4 * words);
        if (stub.size() != 4 * words) continue;
        for (std::size_t index = 0; index < words; ++index) {
            if (word(stub, index) != test.reference[index]) {
                apk::testing::fail(std::source_location::current(), "word differs: ",
                                   std::format("{:#010x} at {}: {:#010x} expected", word(stub, index), index,
                                               test.reference[index]).c_str());
            }
        }
        const auto delta = (address_of(test.reference) - address_of(ref_site)) / 4;
        CHECK(patch == (0x1400'0000u | (static_cast<std::uint32_t>(delta) & 0x03ff'ffff)));  // b site -> stub
    }
}

// Class 1 (step C C3): a load-exclusive with no store-exclusive on any path is not an LL/SC window.
// No store-exclusive's forward progress can be broken, so its x18 uses — the load-exclusive itself
// included — run in ordinary single-instruction islands, not refused.
TEST(a_lone_load_exclusive_is_not_a_window_and_its_x18_uses_are_islands) {
    const std::uint32_t code[] = {
        0xc85f'7c12,  // ldxr x18, [x0]: writes x18, and no store-exclusive follows it
        0x9100'0642,  // add x2, x18, #1: reads x18
        0xd503'201f,  // nop
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.exclusive_regions.empty());
    CHECK(findings.in_place_windows.empty());
    std::vector<std::uint64_t> sites;
    for (const auto &site : findings.sites) sites.push_back(site.address);
    CHECK(sites == (std::vector<std::uint64_t>{0x1000, 0x1004}));  // the ldxr (writes x18) and the add (reads x18)
}

// A store-exclusive physically before its paired load-exclusive, reached by a backward branch, is a
// real LL/SC pair that a forward-only scan would miss (step C C3 review). The load-exclusive must not
// count as lone: an x18 use between them would be islanded inside a live pair and could livelock. It
// stays refused: under step D the use goes through the C6 monitor-escape, which follows the backward
// branch to the store, so it is refused there (not by the window_unclosed blanket, which step D drops).
TEST(a_store_exclusive_before_its_load_exclusive_by_a_back_edge_is_not_lone) {
    const std::uint32_t code[] = {
        0x1400'0003,  // 0x1000 b 0x100c
        0xc802'7c03,  // 0x1004 stxr w2, x3, [x0]: the store-exclusive, before its ldxr
        0x3400'0082,  // 0x1008 cbz w2, 0x1018
        0xc85f'7c01,  // 0x100c ldxr x1, [x0]: the load-exclusive
        0x8b12'0023,  // 0x1010 add x3, x1, x18: the x18 use, dynamically between the ldxr and the stxr
        0x17ff'fffc,  // 0x1014 b 0x1004: backward, to the store-exclusive
        0xd503'201f,  // 0x1018 nop
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.sites.empty());
    CHECK(findings.exclusive_regions.empty());
    CHECK(findings.x18_unsupported.size() == 1);
    if (findings.x18_unsupported.size() == 1) {
        CHECK(findings.x18_unsupported[0].address == 0x1010);  // the add x3, x1, x18 stays refused
        CHECK(std::string_view(findings.x18_unsupported[0].reason) ==
              "it lies between a load- and a store-exclusive, where the island's memory accesses could break the "
              "sequence");
    }
}

// (b) ADR-0020 C7: an in-sequence x18 use (refused as inside_exclusive_sequence) that has a register
// proven dead right after it is relocated to a no-save island over that dead register, not refused. The
// window is the back-edge shape above, with `mov x5, #0` inserted after the use so x5 dies before the
// backward branch. scan_words (step 7: the step-D oracle now accepts a no-save island over a dead register
// as sound, so the fixture runs through it).
TEST(an_in_sequence_x18_use_with_a_dead_register_after_it_becomes_a_no_save_island) {
    const std::uint32_t code[] = {
        0x1400'0003,  // 0x1000 b 0x100c
        0xc802'7c03,  // 0x1004 stxr w2, x3, [x0]: the store-exclusive, before its ldxr
        0x3400'00a2,  // 0x1008 cbz w2, 0x101c
        0xc85f'7c01,  // 0x100c ldxr x1, [x0]: the load-exclusive
        0x8b12'0023,  // 0x1010 add x3, x1, x18: the in-sequence x18 read use
        0xd280'0005,  // 0x1014 mov x5, #0: writes x5, so x5 is dead after the use (the value register)
        0x17ff'fffb,  // 0x1018 b 0x1004: backward, to the store-exclusive
        0xd503'201f,  // 0x101c nop
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);  // the oracle runs too, and accepts it
    CHECK(findings.x18_unsupported.empty());  // no longer refused
    CHECK(findings.sites.size() == 1);
    if (findings.sites.size() == 1) {
        const auto &site = findings.sites[0];
        CHECK(site.kind == SiteKind::x18_nosave);
        CHECK(site.address == 0x1010);
        CHECK(site.reg == 5);                         // Xd = x5, the dead register
        CHECK(site.base == APK_ISLAND_NO_REGISTER);   // a read use needs no store-back base
        CHECK(site.instruction == 0x8b12'0023u);
    }
}

// (b) ADR-0020 C7: an in-sequence x18 WRITE use becomes a no-save island too, but needs a second dead
// register for the store-back that keeps slot 11 coherent. Two registers (x5, x6) die before the backward
// branch, so the island runs `add x18,x1,x3` in x5 and stores x5 back through x6. scan_words: the oracle
// accepts the island because both registers are proven dead (step 7).
TEST(an_in_sequence_x18_write_use_with_two_dead_registers_becomes_a_no_save_island) {
    const std::uint32_t code[] = {
        0x1400'0003,  // 0x1000 b 0x100c
        0xc802'7c03,  // 0x1004 stxr w2, x3, [x0]: the store-exclusive, before its ldxr
        0x3400'00c2,  // 0x1008 cbz w2, 0x1020: exit past the loop
        0xc85f'7c01,  // 0x100c ldxr x1, [x0]: the load-exclusive
        0x8b03'0032,  // 0x1010 add x18, x1, x3: the in-sequence x18 write use
        0xd280'0005,  // 0x1014 mov x5, #0: x5 dead after the use (the value register)
        0xd280'0006,  // 0x1018 mov x6, #0: x6 dead after the use (the store-back base)
        0x17ff'fffa,  // 0x101c b 0x1004: backward, to the store-exclusive
        0xd503'201f,  // 0x1020 nop
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);  // the oracle runs too, and accepts it
    CHECK(findings.x18_unsupported.empty());  // no longer refused
    CHECK(findings.sites.size() == 1);
    if (findings.sites.size() == 1) {
        const auto &site = findings.sites[0];
        CHECK(site.kind == SiteKind::x18_nosave);
        CHECK(site.address == 0x1010);
        CHECK(site.reg == 5);                         // Xd = x5, the value register
        CHECK(site.base == 6);                        // Xb = x6, the store-back base
        CHECK(site.instruction == 0x8b03'0032u);
    }
}

// (b) ADR-0020 C7: a write use with only ONE dead register stays refused -- the store-back needs a
// second one. Same window as above with a single `mov x5,#0`, so x6 is never freed.
TEST(an_x18_write_use_with_one_dead_register_stays_refused) {
    const std::uint32_t code[] = {
        0x1400'0003,  // 0x1000 b 0x100c
        0xc802'7c03,  // 0x1004 stxr w2, x3, [x0]
        0x3400'00a2,  // 0x1008 cbz w2, 0x101c
        0xc85f'7c01,  // 0x100c ldxr x1, [x0]
        0x8b03'0032,  // 0x1010 add x18, x1, x3: the write use
        0xd280'0005,  // 0x1014 mov x5, #0: only one register dies before the branch
        0x17ff'fffb,  // 0x1018 b 0x1004
        0xd503'201f,  // 0x101c nop
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);  // refused: no island for the oracle to see
    CHECK(findings.sites.empty());                         // not converted
    CHECK(findings.x18_unsupported.size() == 1);
    if (findings.x18_unsupported.size() == 1) {
        CHECK(findings.x18_unsupported[0].address == 0x1010);
    }
}

// (b) ADR-0020 C7: a load-exclusive whose straight-line path ends at a ret (or an unconditional branch)
// before reaching a store-exclusive is UNCLOSED -- the store-exclusive of the NEXT function, physically
// close, must not be taken as its close. exclusive_window_end() once walked linearly past the ret into the
// next function and treated that function's stxr as the window's close, refusing the whole span; adjacent
// functions are normal in compiled libraries, so this over-refused real code. Here the back-edge window's
// x18 use must become a no-save island, not be closed by the following function's store-exclusive.
TEST(a_load_exclusive_whose_path_ends_before_its_store_is_not_closed_by_the_next_function) {
    const std::uint32_t code[] = {
        // function A: a back-edge LL/SC with an in-sequence x18 read use, leaving through ret.
        0x1400'0003,  // 0x1000 b 0x100c
        0xc802'7c03,  // 0x1004 stxr w2, x3, [x0]: A's store-exclusive, before its load-exclusive
        0x3400'00a2,  // 0x1008 cbz w2, 0x101c: success leaves through the ret
        0xc85f'7c01,  // 0x100c ldxr x1, [x0]: A's load-exclusive
        0x8b12'0023,  // 0x1010 add x3, x1, x18: the in-sequence x18 read use
        0xd280'0005,  // 0x1014 mov x5, #0: x5 dead after the use
        0x17ff'fffb,  // 0x1018 b 0x1004: back to the store-exclusive
        0xd65f'03c0,  // 0x101c ret: A ends here, with no forward store-exclusive
        // function B (adjacent): its own store-exclusive, physically within the window cap of A's ldxr.
        0xc809'7c68,  // 0x1020 stxr w9, x8, [x3]: must NOT be read as A's close
        0xd503'201f,  // 0x1024 nop
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.empty());  // A's use is a no-save island, not refused with the next function's stxr
    CHECK(findings.sites.size() == 1);
    if (findings.sites.size() == 1) {
        CHECK(findings.sites[0].kind == SiteKind::x18_nosave);
        CHECK(findings.sites[0].address == 0x1010);
        CHECK(findings.sites[0].reg == 5);
    }
}

// Class 5 (step C C4): a nested second load-exclusive before the first closes re-arms the monitor,
// so the first load-exclusive pairs with no store-exclusive — it is lone and its x18 uses island —
// while the second load-exclusive starts its own window, relocated on its own merits.
TEST(a_nested_load_exclusive_makes_the_first_lone_and_the_second_its_own_window) {
    const std::uint32_t code[] = {
        0xc85f'7c01,  // 0x1000 ldxr x1, [x0]: the first load-exclusive (lone: the second re-arms before any stxr)
        0x9100'0432,  // 0x1004 add x18, x1, #1: x18 use of the first run, islanded
        0xc85f'7c62,  // 0x1008 ldxr x2, [x3]: the nested second load-exclusive
        0x9100'0452,  // 0x100c add x18, x2, #1: x18 use inside the second window
        0xc804'7c72,  // 0x1010 stxr w4, x18, [x3]: the second window's store-exclusive
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.empty());
    std::vector<std::uint64_t> sites;
    for (const auto &site : findings.sites) sites.push_back(site.address);
    CHECK(sites == (std::vector<std::uint64_t>{0x1004}));  // only the first run's add x18 islands
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() == 1) {
        CHECK(findings.exclusive_regions[0].address == 0x1008);  // the second ldxr..stxr window
    }
}

// A nested first load-exclusive whose run branches out is not lone: the branch could carry the first
// monitor to the store-exclusive, so it stays refused (window_nested), keeping that cause covered.
TEST(a_nested_load_exclusive_with_a_branch_out_of_the_first_run_stays_refused) {
    const std::uint32_t code[] = {
        0xc85f'7c01,  // 0x1000 ldxr x1, [x0]: the first load-exclusive
        0x9100'0432,  // 0x1004 add x18, x1, #1: x18 use
        0x1400'0003,  // 0x1008 b 0x1014: a branch out of the first run
        0xc85f'7c62,  // 0x100c ldxr x2, [x3]: the nested second load-exclusive
        0xc804'7c62,  // 0x1010 stxr w4, x2, [x3]
        0xd503'201f,  // 0x1014 nop
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.sites.empty());
    CHECK(findings.x18_unsupported.size() == 1);
    if (findings.x18_unsupported.size() == 1) {
        CHECK(findings.x18_unsupported[0].address == 0x1004);
        CHECK(std::string_view(findings.x18_unsupported[0].reason).find("another load-exclusive") !=
              std::string_view::npos);
    }
}

// The load-/store-exclusive loop of ref_x18_ll_sc becomes one region; the x18 instructions before
// and after it stay single-instruction islands, and the x18-free exclusive after is left alone.
TEST(an_ll_sc_loop_becomes_a_region_with_islands_around_it) {
    const auto findings = scan_words(ref_x18_ll_sc, ref_x18_ll_sc_end, 0x1000);
    std::vector<std::uint64_t> islands;
    for (const auto &site : findings.sites) islands.push_back(site.address);
    CHECK(islands == (std::vector<std::uint64_t>{0x1000, 0x1014, 0x1020}));  // the three add x18
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() == 1) {
        const auto &region = findings.exclusive_regions[0];
        CHECK(region.address == 0x1004);         // the ldaxr
        CHECK(region.words.size() == 3);         // ldaxr, add x18, stlxr
        CHECK(region.writes_x18);
    }
}

// A load-/store-exclusive loop with x18 inside relocates as one island (ADR-0020 X2c). The window
// runs from the load-exclusive to the store-exclusive or CLREX that closes it; nothing between them
// may touch memory, so x18 becomes a scratch register loaded before and stored after the window.
TEST(an_exclusive_loop_with_x18_as_the_value_is_one_region) {
    // Shape A: atomic add. ldxr; add x18; stlxr; retry cbnz (outside the window).
    const std::uint32_t code[] = {0xc85f'7db1, 0x8b0e'0232, 0xc810'fdb2, 0x35ff'ffb0};
    const auto findings = scan_words(code, code + 4, 0x1000);
    CHECK(findings.sites.empty());
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() != 1) return;
    const auto &region = findings.exclusive_regions[0];
    CHECK(region.address == 0x1000);
    CHECK(region.words == (std::vector<std::uint32_t>{0xc85f'7db1, 0x8b0e'0232, 0xc810'fdb2}));
    CHECK(region.loads_x18);  // the ldxr loads x17, not x18, so the add's write leaves a gap settle must cover
    CHECK(region.writes_x18);
    CHECK(region.scratch == 0);  // x0 is free (the window uses x13, x14, x16, x17, x18)
}

TEST(a_cmpxchg_loop_keeps_its_internal_branch_and_reads_before_the_window) {
    // Shape B: mov x17,x18 (its own island) then ldaxr..clrex with an internal b.ne to the clrex.
    const std::uint32_t code[] = {0xaa12'03f1, 0xc85f'fdb2, 0xeb11'025f, 0x5400'0041,
                                  0xc803'fda2, 0xd503'3f5f, 0xb5ff'ff43};
    const auto findings = scan_words(code, code + 7, 0x2000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.sites.size() == 1);  // the mov x17,x18 before the loop
    if (findings.sites.size() == 1) CHECK(findings.sites[0].address == 0x2000);
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() != 1) return;
    const auto &region = findings.exclusive_regions[0];
    CHECK(region.address == 0x2004);
    CHECK(region.words.size() == 5);  // ldaxr, cmp, b.ne, stlxr, clrex
    CHECK(region.loads_x18);          // a window that writes x18 loads it, so settle's value-register map holds (X4h)
    CHECK(region.writes_x18);
}

TEST(an_exclusive_loop_using_x18_as_the_address_loads_the_scratch_first) {
    // Shape C: x18 is the base of the exclusive access, read but never written.
    const std::uint32_t code[] = {0xc85f'7e42, 0xaa01'0042, 0xc803'7e42, 0x35ff'ffa3};
    const auto findings = scan_words(code, code + 4, 0x3000);
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() != 1) return;
    const auto &region = findings.exclusive_regions[0];
    CHECK(region.words.size() == 3);
    CHECK(region.loads_x18);
    CHECK(!region.writes_x18);
}

// ADR-0020 X4h: a signal inside a relocated window is delivered at the exact instruction, so the
// window need not run the same from its start. A memory access (step C class 2) or any other
// instruction (class 3) is copied in order and relocated with the window; its x18 uses island.
TEST(a_memory_access_or_other_instruction_in_a_window_relocates) {
    const std::uint32_t inner_store[] = {
        0xc85f'7c28,  // ldxr x8, [x1]
        0x9100'0512,  // add x18, x8, #1
        0xf900'0032,  // str x18, [x1]      a guest store inside the window (class 2)
        0xc809'7c32,  // stxr w9, x18, [x1]
    };
    const std::uint32_t simd[] = {
        0xc85f'7c28,  // ldxr x8, [x1]
        0x9100'0512,  // add x18, x8, #1
        0x4ee0'8400,  // add v0.2d, v0.2d, v1.2d   a non-data-processing instruction (class 3)
        0xc809'7c32,  // stxr w9, x18, [x1]
    };
    for (const auto *window : {inner_store, simd}) {
        const auto findings = scan_words(window, window + 4, 0x10000);
        CHECK(findings.x18_unsupported.empty());
        CHECK(findings.exclusive_regions.size() == 1);
    }
}

// A window that reads the flags but never changes them runs the same twice, and so does one that
// reads the status register only its store-exclusive writes. One whose write of x18 a branch can
// skip is relocated as well, and its island loads x18 before the window: the path through the cbz
// reads x18 without writing it first.
TEST(restartable_exclusive_windows_are_relocated) {
    const auto status = scan_words(ref_x18_window_status, ref_x18_window_end, 0x10000);
    CHECK(status.exclusive_regions.size() == 1);
    CHECK(status.x18_unsupported.empty());
    const auto flags = scan_words(ref_x18_window_flags_read, ref_x18_window_skipped_write, 0x10000);
    CHECK(flags.exclusive_regions.size() == 1);
    CHECK(flags.x18_unsupported.empty());
    const auto skipped = scan_words(ref_x18_window_skipped_write, ref_x18_window_status, 0x10000);
    CHECK(skipped.x18_unsupported.empty());
    CHECK(skipped.exclusive_regions.size() == 1);
    if (skipped.exclusive_regions.size() != 1) return;
    CHECK(skipped.exclusive_regions[0].words.size() == 4);  // ldxr, cbz, mov x18, stxr
    CHECK(skipped.exclusive_regions[0].loads_x18);
    CHECK(skipped.exclusive_regions[0].writes_x18);
}

// A window whose cbz can skip its only write of x18, and which never reads x18: on that path the
// store after the window writes back whatever the value register holds, so the island loads x18
// before this window too (found in the review of X4).
TEST(a_window_that_can_skip_its_only_write_of_x18_loads_it_first) {
    const auto findings = scan_words(ref_x18_window_unread, ref_x18_window_unread_end, 0x10000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() != 1) return;
    const auto &region = findings.exclusive_regions[0];
    CHECK(region.words.size() == 4);  // ldxr, cbz, mov x18, stxr
    CHECK(region.loads_x18);
    CHECK(region.writes_x18);
}

TEST(a_branch_into_the_exclusive_window_makes_it_unsupported) {
    // A branch from before the loop lands on the add inside the window: it cannot be relocated.
    const std::uint32_t code[] = {
        0x1400'0002,  // b .+8, onto the add
        0xc85f'7db1,  // ldxr x17, [x13]
        0x8b0e'0232,  // add x18, x17, x14
        0xc810'fdb2,  // stlxr w16, x18, [x13]
        0x35ff'ffb0,  // cbnz w16, ldxr
    };
    const auto findings = scan_words(code, code + 5, 0x4000);
    CHECK(findings.exclusive_regions.empty());
    CHECK(findings.x18_unsupported.size() == 2);  // the add and the stlxr
    for (const auto &use : findings.x18_unsupported) {
        CHECK(std::string_view(use.reason) == "it lies in an exclusive window into which a branch from outside leads");
    }
}

// The refused window sites are split (counts only, ADR-0020 (b)/(c)) by how the guest x18 reaches them: written in
// the window before use (a retry after a context switch recomputes it) or live into the window (zeroing corrupts it).
TEST(scan_counts_a_refused_window_with_x18_written_as_defined_in_the_loop) {
    const std::uint32_t code[] = {
        0x1400'0002,  // b .+8, onto the add (refuses the window: a branch leads in)
        0xc85f'7db1,  // ldxr x17, [x13]
        0x8b0e'0232,  // add x18, x17, x14   (x18 written before the store: defined in the loop)
        0xc810'fdb2,  // stlxr w16, x18, [x13]
        0x35ff'ffb0,  // cbnz w16, ldxr
    };
    const auto findings = scan_words(code, code + 5, 0x4000);
    CHECK(findings.x18_unsupported.size() == 2);  // the add and the stlxr
    CHECK(findings.x18_defined_in_loop == 2);
    CHECK(findings.x18_live_in == 0);
}

TEST(scan_counts_a_refused_window_with_x18_live_in) {
    const std::uint32_t code[] = {
        0x1400'0002,  // b .+8, onto the add (refuses the window: a branch leads in)
        0xc85f'7db1,  // ldxr x17, [x13]
        0x8b0e'0240,  // add x0, x18, x14    (x18 read, never written: live into the window)
        0xc810'fdb1,  // stlxr w16, x17, [x13]
        0x35ff'ffb0,  // cbnz w16, ldxr
    };
    const auto findings = scan_words(code, code + 5, 0x4000);
    CHECK(findings.x18_unsupported.size() == 1);  // only the add uses x18
    CHECK(findings.x18_live_in == 1);
    CHECK(findings.x18_defined_in_loop == 0);
}

// The split covers in-sequence refused uses, not only refused windows: the use sits in its load-exclusive's
// construct, so live_into_window classifies it. Here x18 is written in the loop before the store (defined-in-loop).
TEST(scan_splits_an_in_sequence_refused_use_by_its_load_exclusive) {
    const std::uint32_t code[] = {
        0x1400'0003,  // 0x1000 b 0x100c
        0xc802'7c03,  // 0x1004 stxr w2, x3, [x0]
        0x3400'00a2,  // 0x1008 cbz w2, 0x101c
        0xc85f'7c01,  // 0x100c ldxr x1, [x0]
        0x8b03'0032,  // 0x1010 add x18, x1, x3: the write use, refused in sequence
        0xd280'0005,  // 0x1014 mov x5, #0
        0x17ff'fffb,  // 0x1018 b 0x1004
        0xd503'201f,  // 0x101c nop
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.size() == 1);
    CHECK(findings.x18_defined_in_loop == 1);  // classified although it is in no refused window
    CHECK(findings.x18_live_in == 0);
}

// ADR-0020 (c) must-have 3: a direct branch from outside the function into its interior (not begin) refuses (c).
TEST(has_external_entry_flags_a_branch_into_the_function_interior) {
    const std::uint32_t code[] = {
        0x1400'0005,  // 0x00 b 0x14: external, into the interior of the function [0x10, 0x20)
        0xd503'201f, 0xd503'201f, 0xd503'201f,              // 0x04 0x08 0x0c nop
        0xd503'201f, 0xd503'201f, 0xd503'201f, 0xd503'201f  // 0x10..0x1c the function
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    CHECK(apk::rewrite::has_external_entry(bytes, 0, 0x10, 0x20));
}

// A computed address (ADRP+ADD) into the interior refuses too; the same to begin is the normal entry.
TEST(has_external_entry_flags_a_computed_address_into_the_interior) {
    std::uint32_t code[] = {
        0x9000'0009,  // 0x00 adrp x9, page 0
        0x9100'5129,  // 0x04 add x9, x9, #0x14 -> 0x14, interior of the function [0x10, 0x20)
        0xd503'201f, 0xd503'201f,                          // 0x08 0x0c nop
        0xd503'201f, 0xd503'201f, 0xd503'201f, 0xd503'201f  // 0x10..0x1c the function
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    CHECK(apk::rewrite::has_external_entry(bytes, 0, 0x10, 0x20));
    code[1] = 0x9100'4129;  // add x9, x9, #0x10 -> 0x10 = begin, the normal entry (not the interior)
    CHECK(!apk::rewrite::has_external_entry(bytes, 0, 0x10, 0x20));
}

TEST(has_external_entry_allows_a_branch_to_begin_and_internal_branches) {
    const std::uint32_t code[] = {
        0x1400'0004,  // 0x00 b 0x10: external, to begin (the normal entry) -- fine
        0xd503'201f, 0xd503'201f, 0xd503'201f,              // 0x04 0x08 0x0c nop
        0x1400'0002,                                        // 0x10 begin: b 0x18 (internal) -- fine
        0xd503'201f, 0xd503'201f, 0xd503'201f               // 0x14 0x18 0x1c nop
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    CHECK(!apk::rewrite::has_external_entry(bytes, 0, 0x10, 0x20));
}

// ADR-0020 (c): R is the first free caller-saved register -- the temporaries x9-x15, then the argument registers
// x2-x7 (a leaf only, see below), and never x0 or x1 (a caller reads them after RET as a pass-through return).
TEST(free_caller_saved_register_prefers_temporaries_then_argument_registers) {
    const std::uint32_t nothing[] = {0xd503'201f};  // nop: a leaf that uses no register
    CHECK(apk::rewrite::free_caller_saved_register(
              std::span(reinterpret_cast<const std::byte *>(nothing), sizeof nothing)) == 9u);
    std::vector<std::uint32_t> temporaries;
    for (unsigned n = 9; n <= 15; n++) temporaries.push_back(0xd280'0000 | n);  // mov x9..x15, #0 (a leaf)
    CHECK(apk::rewrite::free_caller_saved_register(
              std::span(reinterpret_cast<const std::byte *>(temporaries.data()), temporaries.size() * 4)) == 2u);
}

// Never x0 or x1: with every temporary and every other argument register used, a leaf still gets nothing rather
// than x0 or x1, the return registers a caller reads after RET.
TEST(free_caller_saved_register_never_picks_x0_or_x1) {
    std::vector<std::uint32_t> used;
    for (unsigned n : {2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 11u, 12u, 13u, 14u, 15u}) used.push_back(0xd280'0000 | n);
    CHECK(!apk::rewrite::free_caller_saved_register(
               std::span(reinterpret_cast<const std::byte *>(used.data()), used.size() * 4)).has_value());
}

// x2-x7 only in a leaf: a callee (or a tail target) reads the argument registers, so a function that calls or
// branches out may pass a renamed one through untouched. With every temporary used, a BL in the body leaves nothing.
TEST(free_caller_saved_register_uses_argument_registers_only_in_a_leaf) {
    std::vector<std::uint32_t> body;
    for (unsigned n = 9; n <= 15; n++) body.push_back(0xd280'0000 | n);  // mov x9..x15, #0
    CHECK(apk::rewrite::free_caller_saved_register(  // a leaf hands out an argument register (x2)
              std::span(reinterpret_cast<const std::byte *>(body.data()), body.size() * 4)) == 2u);
    body.push_back(0x9400'0001);  // bl .+4: now the body makes a call
    CHECK(!apk::rewrite::free_caller_saved_register(  // x2-x7 may be the callee's arguments, so none is handed out
               std::span(reinterpret_cast<const std::byte *>(body.data()), body.size() * 4)).has_value());
}

// A callee-saved-only function has no register (c) may use: x8, x16/x17 and x19-x28 are never chosen.
TEST(free_caller_saved_register_skips_callee_saved_and_reserved) {
    std::vector<std::uint32_t> every_caller_saved;
    for (unsigned n : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 11u, 12u, 13u, 14u, 15u}) {
        every_caller_saved.push_back(0xd280'0000 | n);  // mov xN, #0 (x8, x16, x17 and the callee-saved regs stay free)
    }
    CHECK(!apk::rewrite::free_caller_saved_register(
               std::span(reinterpret_cast<const std::byte *>(every_caller_saved.data()),
                         every_caller_saved.size() * 4))
               .has_value());
}

// ADR-0020 option (c), lead review: a call island leaves x30 in the stub, so a function that reads x30 as a value
// after a call (a `bl .+4` PC-getter, or inline data through x30) must stay out of (c).
TEST(call_leaks_link_register_flags_a_bl_to_the_next_instruction) {
    const std::uint32_t code[] = {0x9400'0001};  // bl .+4: a PC-getter
    CHECK(apk::rewrite::call_leaks_link_register(std::span(reinterpret_cast<const std::byte *>(code), sizeof code)));
}

TEST(call_leaks_link_register_flags_a_read_of_x30_as_a_value_after_a_call) {
    const std::uint32_t code[] = {0x9400'0010, 0xaa1e'03e0, 0xd65f'03c0};  // bl .+0x40; mov x0, x30; ret
    CHECK(apk::rewrite::call_leaks_link_register(std::span(reinterpret_cast<const std::byte *>(code), sizeof code)));
}

// A spill of x30 to the stack and a return are the normal link-register reads; a non-leaf function with them is fine.
TEST(call_leaks_link_register_allows_a_spilled_link_register_and_a_return) {
    const std::uint32_t code[] = {0xa9bf'7bfd, 0x9400'0010, 0xa8c1'7bfd, 0xd65f'03c0};  // stp;bl;ldp;ret
    CHECK(!apk::rewrite::call_leaks_link_register(std::span(reinterpret_cast<const std::byte *>(code), sizeof code)));
}

// Without a call, x30 is the real caller return (the entry island never touches it), so reading it is safe.
TEST(call_leaks_link_register_allows_a_read_of_x30_without_a_call) {
    const std::uint32_t code[] = {0xaa1e'03e0, 0xd65f'03c0};  // mov x0, x30; ret
    CHECK(!apk::rewrite::call_leaks_link_register(std::span(reinterpret_cast<const std::byte *>(code), sizeof code)));
}

// ADR-0020 option (c): the entry island runs the function's first instruction, so it must be plain -- not a control
// transfer (it would leave without loading R, or skip the body) and not pc-relative (wrong PC in the island).
TEST(c_entry_runs_only_a_plain_first_instruction) {
    CHECK(apk::rewrite::c_entry_runs_first_instruction(0xa9bf'7bfd));   // stp x29, x30, [sp, #-16]! -- plain
    CHECK(apk::rewrite::c_entry_runs_first_instruction(0x9100'0529));   // add x9, x9, #1 -- plain
    CHECK(!apk::rewrite::c_entry_runs_first_instruction(0x9400'0010));  // bl .+0x40 -- a call
    CHECK(!apk::rewrite::c_entry_runs_first_instruction(0xd65f'03c0));  // ret
    CHECK(!apk::rewrite::c_entry_runs_first_instruction(0x1400'0004));  // b .+16
    CHECK(!apk::rewrite::c_entry_runs_first_instruction(0x9000'0009));  // adrp x9, . -- pc-relative
    CHECK(!apk::rewrite::c_entry_runs_first_instruction(0x5800'0040));  // ldr x0, <literal> -- pc-relative
}

// reads_link_register_as_value is the bare predicate the callee check (rule 3) uses: any x30 read but a return or a
// stack spill. A mov from x30 is a value read; a plain ret and a spill are not.
TEST(reads_link_register_as_value_sees_a_value_read_but_not_a_return_or_spill) {
    const std::uint32_t reads[] = {0xaa1e'03e0};  // mov x0, x30
    CHECK(apk::rewrite::reads_link_register_as_value(
        std::span(reinterpret_cast<const std::byte *>(reads), sizeof reads)));
    const std::uint32_t ret_only[] = {0xd65f'03c0};  // ret
    CHECK(!apk::rewrite::reads_link_register_as_value(
        std::span(reinterpret_cast<const std::byte *>(ret_only), sizeof ret_only)));
    const std::uint32_t spill[] = {0xa9bf'7bfd};  // stp x29, x30, [sp, #-16]!
    CHECK(!apk::rewrite::reads_link_register_as_value(
        std::span(reinterpret_cast<const std::byte *>(spill), sizeof spill)));
}

// ADR-0020 option (c) step 3e: c_sync_points marks where a renamed function needs a sync island -- its entry, each
// exit (RET/ERET/indirect branch, or a direct branch leaving the function), and each call. Internal branches are
// left out.
TEST(c_sync_points_classifies_entry_exits_and_calls) {
    const std::uint32_t code[] = {
        0xa9bf'7bfd,  // 0x00 stp x29, x30, [sp, #-16]!  (entry)
        0x9400'0010,  // 0x04 bl .+0x40                   (call)
        0x1400'0002,  // 0x08 b .+8 (target 0x10, inside) (internal: no sync point)
        0xb400'0200,  // 0x0c cbz x0, .+0x40 (target 0x4c, outside)  (branch exit)
        0xd61f'00a0,  // 0x10 br x5                       (indirect exit)
        0xd65f'03c0,  // 0x14 ret                         (exit)
    };
    const auto points = apk::rewrite::c_sync_points(std::span(reinterpret_cast<const std::byte *>(code), sizeof code));
    using K = apk::rewrite::CSyncKind;
    const std::vector<apk::rewrite::CSyncPoint> expected = {
        {0x00, K::entry}, {0x04, K::call}, {0x0c, K::branch_exit},
        {0x10, K::return_or_indirect_exit}, {0x14, K::return_or_indirect_exit},
    };
    CHECK(points == expected);
}

// ADR-0020 option (c) must-have 6: the R-ranges say where a signal finds the guest x18 in the register rather than
// slot 11 -- the body after the entry site, the entry stub after its load, each exit/call stub up to its store, and
// a call stub after its reload. Everywhere else (the entry site, the entry stub before the load, after a store,
// between a call's store and reload) slot 11 holds it.
TEST(c_rename_ranges_cover_the_body_after_entry_and_the_live_stub_stretches) {
    const std::uint32_t code[] = {
        0xa9bf'7bfd,  // 0x00 stp x29, x30, [sp, #-16]!  (entry)
        0xaa00'03e1,  // 0x04 mov x1, x0                 (filler)
        0x9400'0010,  // 0x08 bl .+0x40                  (call)
        0xaa00'03e2,  // 0x0c mov x2, x0                 (filler)
        0xaa00'03e3,  // 0x10 mov x3, x0                 (filler)
        0xd65f'03c0,  // 0x14 ret                        (exit)
    };
    const auto ranges = apk::rewrite::c_rename_ranges(
        std::span(reinterpret_cast<const std::byte *>(code), sizeof code), 0x1000, 9, 0x400);
    // Stubs lay out from 0x400 in sync-point order: entry (5 words), call (10), exit (6).
    const std::vector<apk::rewrite::CRange> expected = {
        {0x1004, 0x1018, 9},  // body after the entry site
        {0x40c, 0x414, 9},    // entry stub: after its ldr (words 3-4)
        {0x414, 0x424, 9},    // call stub: up to and including its store (words 0-3)
        {0x438, 0x43c, 9},    // call stub: after its reload (word 9)
        {0x43c, 0x44c, 9},    // exit stub: up to and including its store (words 0-3)
    };
    CHECK(ranges == expected);
}

// ADR-0020 option (c) step 3e: emit_c_rename renames x18 in place, writes the sync islands, patches each site to
// branch to its island, and returns the R-ranges. Here the function sets x18, calls, and returns.
TEST(emit_c_rename_renames_patches_and_lays_out_the_islands) {
    std::uint32_t code[] = {
        0xa9bf'7bfd,  // 0x00 stp x29, x30, [sp, #-16]!  (entry)
        0x9100'0652,  // 0x04 add x18, x18, #1           (an x18 use)
        0x9400'0010,  // 0x08 bl .+0x40                  (call)
        0xd65f'03c0,  // 0x0c ret                        (exit)
    };
    const std::span function(reinterpret_cast<std::byte *>(code), sizeof code);
    std::vector<std::byte> stub(apk::rewrite::c_rename_stub_bytes(function));
    CHECK(stub.size() == (5 + 10 + 6) * 4);  // entry + call + exit islands
    const auto ranges = apk::rewrite::emit_c_rename(function, 0x1000, 9, 0x400, stub);
    CHECK(code[1] == 0x9100'0529);                     // add x18, x18, #1 renamed to add x9, x9, #1, in place
    CHECK((code[0] & 0xfc00'0000u) == 0x1400'0000u);   // entry site -> entry island
    CHECK((code[2] & 0xfc00'0000u) == 0x1400'0000u);   // call site -> call island
    CHECK((code[3] & 0xfc00'0000u) == 0x1400'0000u);   // ret site -> exit island
    if (stub.size() == 21 * 4) {
        CHECK(word(stub, 2) == 0xf940'2d29u);          // entry island: ldr x9, [x9, #88]
        CHECK(word(stub, 3) == 0xa9bf'7bfdu);          // entry island: the original stp, unchanged
        CHECK(word(stub, 15 + 3) == 0xf900'2e09u);     // exit island (stub word 15): str x9, [x16, #88] at its word 3
    }
    const std::vector<apk::rewrite::CRange> expected = {
        {0x1004, 0x1010, 9}, {0x40c, 0x414, 9}, {0x414, 0x424, 9}, {0x438, 0x43c, 9}, {0x43c, 0x44c, 9},
    };
    CHECK(ranges == expected);
}

// ADR-0020 option (c): a whole-function rename redirects every x18 use to a free register, in place.
TEST(rename_x18_redirects_every_use_to_the_free_register) {
    std::uint32_t code[] = {
        0x9100'0432,  // add x18, x1, #1
        0x8b12'0023,  // add x3, x1, x18
        0xd503'201f,  // nop (no x18, untouched)
    };
    const std::span bytes(reinterpret_cast<std::byte *>(code), sizeof code);
    CHECK(apk::rewrite::rename_x18(bytes, 5));
    CHECK(code[0] == 0x9100'0425);  // add x5, x1, #1
    CHECK(code[1] == 0x8b05'0023);  // add x3, x1, x5
    CHECK(code[2] == 0xd503'201f);
}

// A use where x18 has no register field of its own (a CASP pair) cannot be renamed; the bytes stay untouched.
TEST(rename_x18_refuses_and_keeps_the_bytes_when_x18_has_no_field) {
    std::uint32_t code[] = {
        0x4832'7c82,  // casp x18, x19, x2, x3, [x4]
        0xd503'201f,  // nop
    };
    const std::span bytes(reinterpret_cast<std::byte *>(code), sizeof code);
    CHECK(!apk::rewrite::rename_x18(bytes, 5));
    CHECK(code[0] == 0x4832'7c82);  // unchanged
    CHECK(code[1] == 0xd503'201f);
}

// ADR-0020 option (c): can_rename_x18 is the register-independent check the gate uses, so it agrees with rename_x18
// and the (c) counts. A CASP (x18 has no field of its own) cannot be renamed; a plain x18 use can.
TEST(can_rename_x18_is_false_only_when_an_x18_use_has_no_register_field) {
    const std::uint32_t casp[] = {0x4832'7c82, 0xd503'201f};  // casp x18, ...; nop
    CHECK(!apk::rewrite::can_rename_x18(std::span(reinterpret_cast<const std::byte *>(casp), sizeof casp)));
    const std::uint32_t plain[] = {0x8b12'0023, 0xd503'201f};  // add x3, x1, x18; nop
    CHECK(apk::rewrite::can_rename_x18(std::span(reinterpret_cast<const std::byte *>(plain), sizeof plain)));
    const std::uint32_t none[] = {0xd503'201f};  // nop: no x18 use at all
    CHECK(apk::rewrite::can_rename_x18(std::span(reinterpret_cast<const std::byte *>(none), sizeof none)));
}

// Each window that is not relocated says why, so that one refusal names every cause at once.
TEST(an_exclusive_window_that_is_not_relocated_names_its_cause) {
    // No longer refused here: a window with no store-exclusive at all (step C C3) and a nested
    // load-exclusive with a clean first run (step C C4) make the first load-exclusive lone, so its
    // x18 uses island; and a far pair (C3b, >64 words) now goes through the C6 monitor-escape (step D),
    // not the window_unclosed blanket — covered by the FA/FB and back-edge tests above.
    const std::uint32_t stack[] = {
        0xc85f'7c01,  // ldxr x1, [x0]
        0x9100'43ff,  // add sp, sp, #16
        0x9100'0432,  // add x18, x1, #1
        0xc802'7c12,  // stxr w2, x18, [x0]
    };
    const std::uint32_t crowded[] = {
        0xc85f'7c20,  // ldxr x0, [x1]
        0x8b04'0062, 0x8b07'00c5, 0x8b0a'0128, 0x8b0d'018b,  // add x2, x3, x4 ... add x11, x12, x13
        0x8b10'01ee, 0x8b14'0271, 0x8b17'02d5, 0x8b1a'0338,  // add x14, x15, x16 ... add x24, x25, x26
        0x8b12'039b,  // add x27, x28, x18
        0xc802'7c32,  // stxr w2, x18, [x1]
    };
    const std::uint32_t written[] = {
        0xc85f'7c20,  // ldxr x0, [x1]
        0x8b04'0062, 0x8b07'00c5, 0x8b0a'0128, 0x8b0d'018b,  // add x2, x3, x4 ... add x11, x12, x13
        0x8b10'01ee, 0x8b14'0271, 0x8b17'02d5, 0x8b1a'0338,  // add x14, x15, x16 ... add x24, x25, x26
        0x8b03'007c,  // add x28, x3, x3: x27 stays free, the TSD base for the store-back is missing
        0x9100'0412,  // add x18, x0, #1
        0xc802'7c32,  // stxr w2, x18, [x1]
    };
    const std::uint32_t below_sp[] = {
        0xc85f'7c01,  // ldxr x1, [x0]
        0xf81f'83e2,  // stur x2, [sp, #-8]    reaches below sp, into the island's red zone
        0x9100'0432,  // add x18, x1, #1
        0xc802'7c12,  // stxr w2, x18, [x0]
    };
    const std::uint32_t pair[] = {
        0xc85f'7c01,  // ldxr x1, [x0]
        0x4832'7c82,  // casp x18, x19, x2, x3, [x4]
        0xc805'7c01,  // stxr w5, x1, [x0]
    };
    const std::uint32_t call[] = {
        0xc85f'7e48,  // ldxr x8, [x18]
        0xd73f'092a,  // blraa x9, x10
        0xc809'7e48,  // stxr w9, x8, [x18]
    };
    const std::uint32_t pc_relative[] = {
        0xc85f'7c01,  // ldxr x1, [x0]
        0x9000'0012,  // adrp x18, #0          a pc-relative form the island cannot place
        0xc802'7c12,  // stxr w2, x18, [x0]
    };
    const std::uint32_t simd_literal[] = {
        0xc85f'7c01,  // ldxr x1, [x0]
        0x5c00'0000,  // ldr d0, .             a SIMD literal load (bit 26 set), also pc-relative
        0x9100'0432,  // add x18, x1, #1
        0xc802'7c12,  // stxr w2, x18, [x0]
    };
    struct Case {
        const std::uint32_t *begin, *end;
        std::size_t uses;
        std::string_view reason;
    };
    const Case cases[] = {
        {call, call + std::size(call), 2,
         "it lies in an exclusive window a signal can resume, but it holds a call, whose return address would point "
         "into the island"},
        {pc_relative, pc_relative + std::size(pc_relative), 2,
         "it lies in an exclusive window a signal can resume, but it holds a pc-relative instruction the island cannot "
         "place for its own address"},
        {simd_literal, simd_literal + std::size(simd_literal), 2,
         "it lies in an exclusive window a signal can resume, but it holds a pc-relative instruction the island cannot "
         "place for its own address"},
        {stack, stack + std::size(stack), 2, "it lies in an exclusive window that moves the stack pointer"},
        {below_sp, below_sp + std::size(below_sp), 2,
         "it lies in an exclusive window that reaches below the stack pointer, where the island keeps its registers"},
        {crowded, crowded + std::size(crowded), 2, "it lies in an exclusive window that leaves no register free"},
        {written, written + std::size(written), 2,
         "it lies in an exclusive window that writes x18 and leaves fewer than two registers free"},
        {pair, pair + std::size(pair), 1,
         "it lies in an exclusive window in which x18 has no register field of its own"},
    };
    for (const auto &c : cases) {
        const auto findings = scan_words(c.begin, c.end, 0x10000);
        CHECK(findings.exclusive_regions.empty());
        CHECK(findings.sites.empty());
        CHECK(findings.x18_unsupported.size() == c.uses);
        for (const auto &use : findings.x18_unsupported) {
            if (std::string_view(use.reason) != c.reason) {
                apk::testing::fail(std::source_location::current(), "unexpected reason: ", use.reason);
            }
        }
    }
}

// A thread-pointer access or an svc in a window the converter will not relocate (it could not copy
// such an instruction without running it against the host) still becomes its own redirect island;
// only the window's x18 uses are refused, and they name the redirected instruction (ADR-0020 X4h).
TEST(a_redirected_instruction_keeps_a_window_from_relocating_but_still_islands_itself) {
    const std::uint32_t thread_pointer[] = {
        0xc85f'7c01,  // ldxr x1, [x0]
        0x9100'0432,  // add x18, x1, #1
        0xd53b'd043,  // mrs x3, tpidr_el0
        0xc802'7c12,  // stxr w2, x18, [x0]
    };
    const std::uint32_t syscall[] = {
        0xc85f'7c01,  // ldxr x1, [x0]
        0x9100'0432,  // add x18, x1, #1
        0xd400'0001,  // svc #0            an svc, not a register branch (both match is_branch)
        0xc802'7c12,  // stxr w2, x18, [x0]
    };
    const struct {
        const std::uint32_t *begin, *end;
        SiteKind site;
    } cases[] = {{thread_pointer, thread_pointer + std::size(thread_pointer), SiteKind::thread_pointer_read},
                 {syscall, syscall + std::size(syscall), SiteKind::syscall}};
    for (const auto &c : cases) {
        const auto findings = scan_words(c.begin, c.end, 0x10000);
        CHECK(findings.exclusive_regions.empty());
        CHECK(findings.sites.size() == 1);  // the redirected instruction still gets its own island
        if (findings.sites.size() == 1) CHECK(findings.sites[0].kind == c.site);
        CHECK(findings.x18_unsupported.size() == 2);  // the add and the stxr on x18
        for (const auto &use : findings.x18_unsupported) {
            if (std::string_view(use.reason) !=
                "it lies in an exclusive window a signal can resume, but it holds an instruction the converter "
                "redirects elsewhere (a thread-pointer access or an svc) that the island would copy and run "
                "unredirected") {
                apk::testing::fail(std::source_location::current(), "unexpected reason: ", use.reason);
            }
        }
    }
}

// A branch that leaves the window, or a register branch, keeps it from relocating (ADR-0020 X4h): the
// island's one entry runs its body and then the tail that restores the scratch registers and stores x18
// back to slot 11. A branch out of the window -- including one to the word right after it -- or a
// register branch the island cannot retarget would skip that tail, so the window is refused outright and
// its x18 uses are named (it cannot stay in place either, since restart_problem also refuses it).
TEST(a_branch_that_leaves_a_window_or_a_register_branch_keeps_it_from_relocating) {
    const std::uint32_t register_branch[] = {
        0xc85f'7e48,  // ldxr x8, [x18]
        0xd65f'03c0,  // ret            a return before the store-exclusive: the window's path ends here
        0xc809'7e48,  // stxr w9, x8, [x18]
    };
    const std::string_view leaves =
        "it lies in an exclusive window a signal can resume, but a branch leaves the window, skipping the island's "
        "tail that restores the scratch registers and stores x18 back to slot 11";
    struct Case {
        const std::uint32_t *begin, *end;
        std::size_t uses;
        std::string_view reason;
    };
    const Case cases[] = {
        {ref_x18_window_exit, ref_x18_window_input, 2, leaves},            // b.ne out of the window
        {ref_x18_window_exit_next, ref_x18_window_base, 2, leaves},        // branch to the word right after it
        // The return before the store-exclusive ends the window's straight-line path (ADR-0020 step D), so
        // exclusive_window_end() no longer reads the later store-exclusive as its close. The window is unclosed,
        // and its two x18-address exclusive accesses are refused as exclusive accesses (an island's memory could
        // break their LL/SC sequence) -- still not relocated. The register-branch-reach refusal stays covered by
        // an_unclosed_window_reaching_a_non_jump_table_register_branch_names_the_register_branch.
        {register_branch, register_branch + std::size(register_branch), 2,
         "it is an exclusive access, and the island's memory accesses could break its LL/SC sequence"},
    };
    for (const auto &c : cases) {
        const auto findings = scan_words(c.begin, c.end, 0x10000);
        CHECK(findings.exclusive_regions.empty());
        CHECK(findings.x18_unsupported.size() == c.uses);
        for (const auto &use : findings.x18_unsupported) {
            if (std::string_view(use.reason) != c.reason) {
                apk::testing::fail(std::source_location::current(), "unexpected reason: ", use.reason);
            }
        }
    }
}

// A window that writes x18 without ever reading it first must still load x18 in its preamble (Finding D):
// a signal after the preamble but before the write would otherwise make settle report the value
// register's own guest value as x18, and a handler that does not return would leave slot 11 wrong.
TEST(a_window_that_writes_x18_without_reading_it_first_still_loads_x18) {
    const std::uint32_t code[] = {
        0xc85f'7c28,  // ldxr x8, [x1]
        0x9100'0512,  // add x18, x8, #1     writes x18, never reads it first
        0xc809'7c32,  // stxr w9, x18, [x1]
    };
    const auto findings = scan_words(code, code + std::size(code), 0x10000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() == 1) {
        const auto &region = findings.exclusive_regions[0];
        CHECK(region.writes_x18);
        CHECK(region.loads_x18);  // even though no path reads x18 before writing it
    }
}

// A window that only reads x18 stores nothing back and needs one free register for the value, not two; and a
// window may close up to 64 words after its load-exclusive (the Unity test APK step B).
TEST(a_window_that_only_reads_x18_needs_one_free_register_and_may_span_64_words) {
    const std::uint32_t read[] = {
        0xc85f'7e40,  // ldxr x0, [x18]
        0x8b04'0062, 0x8b07'00c5, 0x8b0a'0128, 0x8b0d'018b,  // add x2, x3, x4 ... add x11, x12, x13
        0x8b10'01ee, 0x8b14'0271, 0x8b17'02d5, 0x8b1a'0338,  // add x14, x15, x16 ... add x24, x25, x26
        0x8b03'007c,  // add x28, x3, x3: only x27 stays free
        0xc801'7e40,  // stxr w1, x0, [x18]
    };
    const auto findings = scan_words(read, read + std::size(read), 0x10000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() == 1) {
        const auto &region = findings.exclusive_regions[0];
        CHECK(region.scratch == 27);
        CHECK(region.loads_x18);
        CHECK(!region.writes_x18);
        CHECK(apk::rewrite::exclusive_region_layout(region).base == APK_ISLAND_NO_REGISTER);
    }

    std::vector<std::uint32_t> long_window{0xc85f'7c01, 0x9100'0432};  // ldxr x1, [x0]; add x18, x1, #1
    long_window.insert(long_window.end(), 62, 0xd280'0003);            // mov x3, #0
    long_window.push_back(0xc802'7c12);                                // stxr w2, x18, [x0]: 64 words after
    const auto spanning = scan_words(long_window.data(), long_window.data() + long_window.size(), 0x10000);
    CHECK(spanning.x18_unsupported.empty());
    CHECK(spanning.exclusive_regions.size() == 1);
    if (spanning.exclusive_regions.size() == 1) CHECK(spanning.exclusive_regions[0].words.size() == 65);
}

// A window that uses x18 only as a temporary runs as it is when the island cannot take it (the Unity test APK step B): XNU
// zeroes x18 only at a context switch, which comes with an exception, and every exception return clears the local
// exclusive monitor, so a pass that lost x18 fails its store-exclusive and runs again. Here the window writes x18
// and leaves one register free, too few for an island.
std::vector<std::uint32_t> temporary_x18_window() {
    return {
        0xc85f'7c20,  // ldxr x0, [x1]
        0x8b04'0062, 0x8b07'00c5, 0x8b0a'0128, 0x8b0d'018b,  // add x2, x3, x4 ... add x11, x12, x13
        0x8b10'01ee, 0x8b14'0271, 0x8b17'02d5, 0x8b1a'0338,  // add x14, x15, x16 ... add x24, x25, x26
        0x8b03'007c,  // add x28, x3, x3
        0x9100'0412,  // add x18, x0, #1: written before it is read
        0xc802'7c32,  // stxr w2, x18, [x1]
        0x35ff'fe82,  // cbnz w2, ldxr: a failed pass runs again
        0xd280'0012,  // mov x18, #0: dead after the window
        0xd65f'03c0,  // ret
    };
}

TEST(a_window_that_uses_x18_only_as_a_temporary_stays_in_place) {
    const auto code = temporary_x18_window();
    const auto findings = scan_words(code.data(), code.data() + code.size(), 0x10000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.exclusive_regions.empty());
    CHECK(findings.in_place_windows == std::vector<std::uint64_t>{0x10000});
    CHECK(findings.sites.size() == 1);  // the mov x18 after the window, an island of its own
    if (findings.sites.size() == 1) CHECK(findings.sites[0].address == 0x10034);

    // x18 written again 16 words after the retry: still within reach.
    auto later = code;
    later.insert(later.begin() + 13, 15, 0xd280'0003);  // mov x3, #0
    CHECK(scan_words(later.data(), later.data() + later.size(), 0x10000).in_place_windows.size() == 1);
}

// Each condition of staying in place, broken once: the window is refused with the island's cause.
TEST(a_window_stays_in_place_only_when_a_lost_x18_cannot_escape) {
    struct Change {
        const char *what;
        std::size_t at, count;             // the words replaced
        std::vector<std::uint32_t> words;  // by these
        std::string_view reason =
            "it lies in an exclusive window that writes x18 and leaves fewer than two registers free";
    };
    const Change changes[] = {
        {"x18 read before it is written", 9, 1, {0x8b03'025c}},  // add x28, x18, x3
        {"no retry after the store-exclusive", 12, 1, {}},       // the cbnz left out
        {"a retry to another word", 12, 1, {0x35ff'fea2}},       // cbnz w2, the word after the ldxr
        {"a CBZ in place of the CBNZ", 12, 1, {0x34ff'fe82}},    // cbz w2, ldxr
        {"a CBNZ on another register", 12, 1, {0x35ff'fe83}},  // cbnz w3, ldxr
        {"a CBNZ that does not go back", 12, 1, {0x3500'0042}},  // cbnz w2, .+8
        {"a window that only a CLREX closes", 11, 2, {0xd503'3f5f, 0x35ff'fe83}},  // clrex; cbnz w3, ldxr
        {"x18 read after the window", 13, 1, {0x9100'0640, 0xd280'0012}},  // add x0, x18, #1; mov x18, #0
        {"a branch before x18 is written again", 13, 1, {0x1400'0001, 0xd280'0012}},  // b .+4; mov x18, #0
        {"x18 decides a branch in the window", 10, 3,
         {0x9100'0412, 0xb400'0032, 0xc802'7c32, 0x35ff'fe62}},  // add x18; cbz x18, .+4; stxr; cbnz w2, ldxr
        {"x18 as the store-exclusive's address", 11, 1, {0xc802'7e40}},              // stxr w2, x0, [x18]
        {"the status in x18", 11, 2, {0xc812'7c20, 0x35ff'fe92}},  // stxr w18, x0, [x1]; cbnz w18, ldxr
        {"a CLREX after the store-exclusive", 11, 2, {0xc802'7c32, 0xd503'3f5f, 0x35ff'fe62}},  // stxr; clrex; cbnz
        {"the status in wzr", 11, 2, {0xc81f'7c32, 0x35ff'fe9f}},  // stxr wzr, x18, [x1]; cbnz wzr
        {"a memory access in the window", 9, 1, {0xf940'007c}},    // ldr x28, [x3]: it would run again
        {"a branch from outside into the window", 15, 0, {0x17ff'fff6}},  // b to the fifth word
        {"a write of x18 a branch can skip", 10, 3,
         {0xb400'0040, 0x9100'0412, 0xc802'7c32, 0x35ff'fe62}},  // cbz x0, .+8; add x18; stxr; cbnz w2, ldxr
        {"x18 decides a branch through the flags", 10, 3,
         {0x9100'0412, 0xf100'025f, 0x5400'0020, 0xc802'7c32, 0x35ff'fe42}},  // add x18; cmp x18, #0; b.eq .+4
        {"x18 decides a branch through another register", 10, 3,
         {0x9100'0412, 0x9100'0645, 0xb400'0025, 0xc802'7c32, 0x35ff'fe42}},  // add x18; add x5, x18, #1; cbz x5
        {"x18 decides a branch through a cmpbr", 10, 3,  // add x18; add x5, x18, #1; cmpbr x5, x6, .+4 (Finding A)
         {0x9100'0412, 0x9100'0645, 0xf4c6'0025, 0xc802'7c32, 0x35ff'fe42}},
        {"sp from x18", 10, 3, {0x9100'0412, 0x9100'225f, 0xc802'7c32, 0x35ff'fe62},  // add x18; add sp, x18, #8
         "it lies in an exclusive window that moves the stack pointer"},
        {"an authentication with x18", 10, 3,
         {0x9100'0412, 0xaa00'03fb, 0xdac1'125b, 0xc802'7c32, 0x35ff'fe42}},  // add x18; mov x27, x0; autia x27, x18
        {"x18 written again only after 16 words", 13, 1,
         {0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003,
          0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003, 0xd280'0003,
          0xd280'0012}},  // mov x3, #0 sixteen times, then mov x18, #0
    };
    for (const auto &change : changes) {
        auto code = temporary_x18_window();
        const auto at = code.begin() + static_cast<std::ptrdiff_t>(change.at);
        code.insert(code.erase(at, at + static_cast<std::ptrdiff_t>(change.count)), change.words.begin(),
                    change.words.end());
        const auto findings = scan_words(code.data(), code.data() + code.size(), 0x10000);
        if (!findings.in_place_windows.empty() || findings.x18_unsupported.empty()) {
            apk::testing::fail(std::source_location::current(), "stays in place: ", change.what);
        }
        for (const auto &use : findings.x18_unsupported) {
            if (std::string_view(use.reason) != change.reason) {
                apk::testing::fail(std::source_location::current(), change.what, use.reason);
            }
        }
    }
}

// ADR-0020 step C C6 (class 6): a non-lone load-exclusive's monitor can still be live past a window a
// branch jumps over, so an x18 use there must stay refused — islanding it would put the island's
// memory between the load and its store and could break the LL/SC (the C3b livelock). The reviewers'
// counterexample: L0 arms [x0], a cbnz skips the inner window and reaches the use and L0's own store,
// so on the 0x04->0x18 path L0's monitor is live at 0x18.
TEST(an_x18_use_a_branch_reaches_past_a_relocated_window_stays_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]        L0, non-lone: its store is at 0x1c
        0xb50000a8,  // 0x04 cbnz x8, 0x18        skip the inner window, jump to the use
        0xc85f7c29,  // 0x08 ldxr x9, [x1]        the inner window, relocated
        0x91000652,  // 0x0c add x18, x18, #1
        0xc80a7c29,  // 0x10 stxr w10, x9, [x1]
        0xd65f03c0,  // 0x14 ret
        0x91000a52,  // 0x18 add x18, x18, #2     the use: a live monitor on the 0x04->0x18 path
        0xc80b7c0a,  // 0x1c stxr w11, x10, [x0]  L0's store
        0x35ffff0b,  // 0x20 cbnz w11, 0x00
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(bytes, 0x1000, findings);
    CHECK(findings.exclusive_regions.size() == 1);  // the inner window still relocates
    CHECK(findings.x18_unsupported.size() == 1);
    if (!findings.x18_unsupported.empty()) {
        CHECK(findings.x18_unsupported[0].address == 0x1018);
        CHECK(std::string_view(findings.x18_unsupported[0].reason).find("between a load- and a store-exclusive")
              != std::string_view::npos);
    }
    for (const auto &site : findings.sites) CHECK(site.address != 0x1018);
}

// The same hazard through a register branch the rewriter cannot follow: the exclusive_sequence flag,
// which a covered window does not clear, keeps the use refused.
TEST(an_x18_use_a_register_branch_may_reach_past_a_relocated_window_stays_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]        L0, non-lone through the register branch
        0xd61f0120,  // 0x04 br x9                a branch whose target the rewriter cannot see
        0xc85f7c29,  // 0x08 ldxr x9, [x1]        the inner window, relocated
        0x91000652,  // 0x0c add x18, x18, #1
        0xc80a7c29,  // 0x10 stxr w10, x9, [x1]
        0x91000a52,  // 0x14 add x18, x18, #2     the use: a br x9 here would find L0's monitor live
        0x91000421,  // 0x18 add x1, x1, #1
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(bytes, 0x1000, findings);
    CHECK(findings.exclusive_regions.size() == 1);
    CHECK(findings.x18_unsupported.size() == 1);
    if (!findings.x18_unsupported.empty()) CHECK(findings.x18_unsupported[0].address == 0x1014);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1014);
}

// The branch-reached refusal also closes the old gap where the inner window has no x18, so its own
// store-exclusive clears the flag: the direct branch's target is refused from the branch (rule 1).
TEST(an_x18_use_a_branch_reaches_past_an_x18_free_window_stays_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]        L0
        0xb50000a8,  // 0x04 cbnz x8, 0x18        skip the inner window
        0xc85f7c29,  // 0x08 ldxr x9, [x1]        the inner window, WITHOUT x18 (not relocated)
        0x91000529,  // 0x0c add x9, x9, #1
        0xc80a7c29,  // 0x10 stxr w10, x9, [x1]
        0xd65f03c0,  // 0x14 ret
        0x91000a52,  // 0x18 add x18, x18, #2     the use
        0xc80b7c08,  // 0x1c stxr w11, x8, [x0]   L0's store
        0x35ffff0b,  // 0x20 cbnz w11, 0x00
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(bytes, 0x1000, findings);
    CHECK(findings.exclusive_regions.empty());  // the x18-free window is not relocated
    CHECK(findings.x18_unsupported.size() == 1);
    if (!findings.x18_unsupported.empty()) CHECK(findings.x18_unsupported[0].address == 0x1018);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1018);
}

// A direct branch whose target is exactly the nested load-exclusive (the run's end) re-tags the
// monitor there, so the outer load is lone (rule 2) and its later x18 use is an ordinary island.
TEST(a_branch_to_the_nested_load_makes_the_outer_load_lone) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]        L0
        0xb5000028,  // 0x04 cbnz x8, 0x08        target == the nested load: re-tags the monitor
        0xc85f7c29,  // 0x08 ldxr x9, [x1]        nested load, its window relocated
        0x91000652,  // 0x0c add x18, x18, #1
        0xc80a7c29,  // 0x10 stxr w10, x9, [x1]
        0x91000a52,  // 0x14 add x18, x18, #2     the use: L0 is lone, no live monitor here
        0x91000421,  // 0x18 add x1, x1, #1
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(bytes, 0x1000, findings);
    CHECK(findings.exclusive_regions.size() == 1);  // the nested window relocates
    CHECK(findings.x18_unsupported.empty());        // no false in-sequence refusal
    const std::vector<Site> expected{{0x1014, SiteKind::x18, 18, 0x91000a52}};
    CHECK(findings.sites == expected);              // the later use is an island
}

// An in-place window (step B) sits in `covered` like a relocated one, so the non-lone outer load's
// monitor is not cleared across it and a branch-reached use stays refused either way.
TEST(an_x18_use_a_branch_reaches_past_an_in_place_window_stays_refused) {
    std::vector<std::uint32_t> code{0xc85f7c08, 0};  // ldxr x8, [x0]; cbnz x8, U (filled in below)
    const auto window = temporary_x18_window();      // a window kept in place (step B)
    code.insert(code.end(), window.begin(), window.end());
    const auto u_word = code.size();
    code.push_back(0x91000a52);  // U: add x18, x18, #2
    code.push_back(0xc80b7c08);  // stxr w11, x8, [x0]: the outer load's store
    code[1] = 0xb5000008 | (static_cast<std::uint32_t>(u_word - 1) << 5);  // cbnz x8, U
    const auto findings = scan_words(code.data(), code.data() + code.size(), 0x1000);
    const std::uint64_t u_addr = 0x1000 + 4 * u_word;
    CHECK(findings.in_place_windows.size() == 1);  // the inner window is kept in place
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) {
        if (use.address == u_addr) refused = true;
    }
    CHECK(refused);                                                 // the use is not islanded
    for (const auto &site : findings.sites) CHECK(site.address != u_addr);
}

// A chain of direct branches: branch1 leaves L's run, then branch2 (at/after branch1's target) skips
// the store where a single forward scan would stop, reaching a use with L's monitor still live. The
// reachability must be transitive, or this use is wrongly islanded (a C3b livelock). On the x9!=0,
// x11!=0 path, L's monitor is live at 0x1c and its store is 0x20.
TEST(an_x18_use_a_chain_of_branches_reaches_stays_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]        L arms [x0]
        0xb5000089,  // 0x04 cbnz x9, 0x14        branch1: leaves L's run
        0x91000652,  // 0x08 add x18, x18, #1     a use on the fall-through path (also monitor-live)
        0xc80a7c08,  // 0x0c stxr w10, x8, [x0]   a store pairing L (the x9 == 0 path)
        0xd65f03c0,  // 0x10 ret
        0xb500004b,  // 0x14 cbnz x11, 0x1c       branch2: skips the store at 0x18
        0xc80c7c08,  // 0x18 stxr w12, x8, [x0]   a store pairing L (the x11 == 0 path)
        0x91000a52,  // 0x1c add x18, x18, #2      the use: a live monitor on 0x00->0x14->0x1c->0x20
        0xc80d7c08,  // 0x20 stxr w13, x8, [x0]   the store on that path
        0x35fffeed,  // 0x24 cbnz w13, 0x00
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(bytes, 0x1000, findings);
    bool use_refused = false;
    for (const auto &use : findings.x18_unsupported) {
        if (use.address == 0x101c) use_refused = true;
    }
    CHECK(use_refused);  // the transitively-reached use is refused, not islanded
    for (const auto &site : findings.sites) CHECK(site.address != 0x101c);
}

// Rule 2 only relaxes a branch to the run's end exactly; a branch PAST the nested load leaves the
// outer load non-lone, because on that path the nested load is skipped and the outer monitor is still
// live at the target. Guards against relaxing target >= end instead of target == end. L0's store is
// reachable from the use, so its monitor escapes: were L0 wrongly lone, the use would be islanded.
TEST(a_branch_past_the_nested_load_keeps_the_outer_load_non_lone) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]        L0, non-lone: its store is at 0x14
        0xb5000068,  // 0x04 cbnz x8, 0x10        target 0x10 is PAST the nested load (end = 0x08)
        0xc85f7c29,  // 0x08 ldxr x9, [x1]        nested load: the run's end
        0xd5033f5f,  // 0x0c clrex
        0x91000a52,  // 0x10 add x18, x18, #2     the use: reached, L0's monitor still live and escapes
        0xc80a7c08,  // 0x14 stxr w10, x8, [x0]   L0's store, which the monitor at 0x10 reaches
        0x35ffff4a,  // 0x18 cbnz w10, 0x00
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(bytes, 0x1000, findings);
    bool use_refused = false;
    for (const auto &use : findings.x18_unsupported) {
        if (use.address == 0x1010) use_refused = true;
    }
    CHECK(use_refused);  // L0 is non-lone, so the branch-reached use is refused, not islanded
    for (const auto &site : findings.sites) CHECK(site.address != 0x1010);
}

// The GCC __atomic_compare_exchange shape: on the b.ne path the store is skipped, so the monitor is
// still live at the fail label, but no store-exclusive pairs on that path (it ends in a return). The
// use there is safe to island — reaching it is not enough, nothing escapes (ADR-0020 C6 decision 2).
TEST(a_cmpxchg_fail_path_use_with_no_store_reachable_is_islanded) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone: its store is at 0x0c
        0xf100011f,  // 0x04 cmp x8, #0
        0x54000061,  // 0x08 b.ne 0x14           the compare failed: skip the store, go to the fail label
        0xc80a7c08,  // 0x0c stxr w10, x8, [x0]
        0x35ffff8a,  // 0x10 cbnz w10, 0x00
        0x91000640,  // 0x14 add x0, x18, #1      the fail-label use: monitor live, but only a ret follows
        0xd65f03c0,  // 0x18 ret
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.empty());  // nothing escapes from the use, so it is not refused
    const std::vector<Site> expected{{0x1014, SiteKind::x18, 18, 0x91000640}};
    CHECK(findings.sites == expected);        // the use runs in an ordinary island
}

// The same shape, but a store-exclusive follows the fail-label use before any boundary: now L0's
// monitor does pair on that path, so islanding the use would break the LL/SC and it stays refused.
TEST(a_store_exclusive_reachable_from_the_fail_path_use_keeps_it_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0
        0xf100011f,  // 0x04 cmp x8, #0
        0x54000061,  // 0x08 b.ne 0x14           skip the first store, reach the use
        0xc80a7c08,  // 0x0c stxr w10, x8, [x0]
        0x35ffff8a,  // 0x10 cbnz w10, 0x00
        0x91000652,  // 0x14 add x18, x18, #1     the use: a live monitor on the 0x08->0x14 path
        0xc80b7c08,  // 0x18 stxr w11, x8, [x0]   a store that monitor reaches: the use escapes
        0x35ffff2b,  // 0x1c cbnz w11, 0x00
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.size() == 1);
    if (!findings.x18_unsupported.empty()) {
        CHECK(findings.x18_unsupported[0].address == 0x1014);
        CHECK(std::string_view(findings.x18_unsupported[0].reason).find("between a load- and a store-exclusive")
              != std::string_view::npos);
    }
    for (const auto &site : findings.sites) CHECK(site.address != 0x1014);
}

// A register branch reachable from the use is a danger too: its target the rewriter cannot follow, so
// the monitor may reach a store there. A reached register branch keeps the use refused (C6 decision 3).
TEST(a_register_branch_reachable_from_the_use_keeps_it_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0
        0xf100011f,  // 0x04 cmp x8, #0
        0x54000061,  // 0x08 b.ne 0x14           skip the store, reach the use
        0xc80a7c08,  // 0x0c stxr w10, x8, [x0]
        0x35ffff8a,  // 0x10 cbnz w10, 0x00
        0x91000652,  // 0x14 add x18, x18, #1     the use: monitor live on the 0x08->0x14 path
        0xd61f00a0,  // 0x18 br x5                a register branch the monitor reaches: the use escapes
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.size() == 1);
    if (!findings.x18_unsupported.empty()) CHECK(findings.x18_unsupported[0].address == 0x1014);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1014);
}

// An unconditional B carries the monitor to the use (the only path to it), which then escapes to a
// store: reachability must follow unconditional branches, not only conditional ones.
TEST(an_unconditional_branch_carries_the_monitor_to_a_refused_use) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone: its store is at 0x08
        0x14000003,  // 0x04 b 0x10              unconditional: skip the straight-line store, reach the use
        0xc80a7c08,  // 0x08 stxr w10, x8, [x0]   the x8 == 0 path's store (skipped by the B)
        0xd65f03c0,  // 0x0c ret
        0x91000652,  // 0x10 add x18, x18, #1     the use: reached only through the unconditional B
        0xc80b7c08,  // 0x14 stxr w11, x8, [x0]   a store the monitor reaches: the use escapes
        0x35ffff4b,  // 0x18 cbnz w11, 0x00
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x1010) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1010);
}

// A backward branch carries the monitor to a use before it. The clrex at 0x08 clears the linear-scan
// flag so only the branch reachability can refuse the use; the cbnz at 0x14 is the only path to it.
TEST(a_backward_branch_reaches_a_use_that_stays_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0
        0xb5000088,  // 0x04 cbnz x8, 0x14       skip the clrex, keep the monitor live
        0xd5033f5f,  // 0x08 clrex                clears the straight-line flag at and after here
        0x91000652,  // 0x0c add x18, x18, #1     the use: reached only by the back-edge at 0x14
        0xc80a7c08,  // 0x10 stxr w10, x8, [x0]   a store the monitor reaches from the use: it escapes
        0xb5ffffc9,  // 0x14 cbnz x9, 0x0c        back-edge to the use, with L0's monitor still live
        0xd65f03c0,  // 0x18 ret
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x100c) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x100c);
}

// A use two words past a branch target is reached by falling through from the target, so marking only
// the target word is not enough. The clrex at 0x08 clears the linear flag, isolating the reachability.
TEST(a_use_past_a_branch_target_is_reached_and_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0
        0xb5000068,  // 0x04 cbnz x8, 0x10       to the branch target at 0x10, monitor live
        0xd5033f5f,  // 0x08 clrex                clears the straight-line flag
        0xd65f03c0,  // 0x0c ret
        0x91000421,  // 0x10 add x1, x1, #1       the branch target (not an x18 use)
        0x91000652,  // 0x14 add x18, x18, #1     the use: a word past the target, reached by fall-through
        0xc80a7c08,  // 0x18 stxr w10, x8, [x0]   a store the use reaches: it escapes
        0x35ffff2a,  // 0x1c cbnz w10, 0x00
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x1014) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1014);
}

// A monitor-live run that reaches the end of the code with no store or register branch pairs nothing,
// so the use is islanded, not refused: reaching the buffer's end is not an escape (C6 decision 2).
TEST(a_monitor_run_to_the_code_end_without_a_store_islands_the_use) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone: its store is at 0x08
        0xb5000088,  // 0x04 cbnz x8, 0x14       skip the store, reach the use with the monitor live
        0xc80a7c08,  // 0x08 stxr w10, x8, [x0]
        0x35ffffaa,  // 0x0c cbnz w10, 0x00
        0xd65f03c0,  // 0x10 ret
        0x91000652,  // 0x14 add x18, x18, #1     the use: monitor live, but only x18-free code to the end
        0x91000421,  // 0x18 add x1, x1, #1       the run falls off the end of the buffer with no store
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.empty());
    bool islanded = false;
    for (const auto &site : findings.sites) if (site.address == 0x1014) islanded = true;
    CHECK(islanded);
}

// Step D + C6 decision 2: a window that does not close but whose monitor only reaches the section's
// end (a branch to the code end, or falling off it) with no store islands its use — an LL/SC never
// spans a section boundary, so nothing pairs. Before step D the window_unclosed blanket refused it.
TEST(a_branch_to_the_code_end_islands_the_use_as_no_store_pairs) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0: its run reaches the code's end (no store or clrex)
        0xb5000049,  // 0x04 cbnz x9, 0x0c       branch to 0x0c == the code's end
        0x91000652,  // 0x08 add x18, x18, #1     the use: no store reachable, so it islands (decision 2)
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.empty());
    bool islanded = false;
    for (const auto &site : findings.sites) if (site.address == 0x1008) islanded = true;
    CHECK(islanded);
}

// Step D + C6 decision 2: a window that does not close within 64 words, whose monitor reaches a far
// clrex (which clears it) or the section end with no store, islands its use. Lone-load detection still
// has no 64-word cap, but the island outcome no longer depends on it here (a store-free run islands
// whether the load is lone or window_unclosed); the store-reachable case is covered by the back-edge
// tests and the brute-force oracle.
TEST(a_branch_past_a_clrex_more_than_64_words_away_islands_the_use) {
    std::vector<std::uint32_t> code{
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0: its run ends at the clrex 65 words on, at 0x104
        0xb5000829,  // 0x04 cbnz x9, 0x108      branch PAST the clrex (0x108 > end 0x104): leaves the run
        0x91000652,  // 0x08 add x18, x18, #1     the use: no store reachable (the clrex clears), so it islands
    };
    code.insert(code.end(), 62, 0xd503201f);  // 0x0c..0x100: 62 nops, pushing the clrex past 64 words
    code.push_back(0xd5033f5f);               // 0x104 clrex: the run's end, 65 words from L0
    code.push_back(0x91000421);               // 0x108 add x1, x1, #1 (the branch target, past the clrex)
    code.push_back(0xd503201f);               // 0x10c nop
    const auto findings = scan_words(code.data(), code.data() + code.size(), 0x1000);
    CHECK(findings.x18_unsupported.empty());
    bool islanded = false;
    for (const auto &site : findings.sites) if (site.address == 0x1008) islanded = true;
    CHECK(islanded);
}

// A store reachable only through a BACKWARD branch from the use still makes the monitor escape: the
// cbz at 0x18 jumps back to the store at 0x08, which L0's monitor (live on the 0x04->0x14 path) pairs.
TEST(a_backward_branch_to_the_store_keeps_the_use_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0
        0xb5000089,  // 0x04 cbnz x9, 0x14       reach the use, monitor live
        0xc80a7c08,  // 0x08 stxr w10, x8, [x0]  the store the backward branch returns to
        0xd65f03c0,  // 0x0c ret
        0xd503201f,  // 0x10 nop
        0x91000652,  // 0x14 add x18, x18, #1    the use
        0xb4ffff8b,  // 0x18 cbz x11, 0x08       backward to the store: the monitor at 0x14 escapes to it
        0xd65f03c0,  // 0x1c ret
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x1014) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1014);
}

// A pointer-authenticated register branch (BRAA, and its Z form BRAAZ) is an indirect jump like BR, so
// a monitor reaching it escapes and the use stays refused. Guards is_register_branch's mask against a
// narrowing to bare BR only.
TEST(a_pac_register_branch_reachable_from_the_use_keeps_it_refused) {
    for (const std::uint32_t tail : {0xd71f08a6u, 0xd61f08bfu}) {  // braa x5, x6 ; braaz x5
        const std::uint32_t code[] = {
            0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0
            0xf100011f,  // 0x04 cmp x8, #0
            0x54000061,  // 0x08 b.ne 0x14           skip the store, reach the use
            0xc80a7c08,  // 0x0c stxr w10, x8, [x0]
            0x35ffff8a,  // 0x10 cbnz w10, 0x00
            0x91000652,  // 0x14 add x18, x18, #1    the use: monitor live on 0x08->0x14
            tail,        // 0x18 braa x5, x6 / braaz x5: an authenticated register branch (the escape)
        };
        const auto findings = scan_words(code, code + std::size(code), 0x1000);
        bool refused = false;
        for (const auto &use : findings.x18_unsupported) if (use.address == 0x1014) refused = true;
        CHECK(refused);
        for (const auto &site : findings.sites) CHECK(site.address != 0x1014);
    }
}

// A call ends the LL/SC pairing: a store on the far side of the call is not paired with L0, so the use
// before the call islands. Pins BL and the register calls (BLR, BLRAA) as monitor-path boundaries.
TEST(an_x18_use_before_a_call_islands_because_no_pairing_crosses_it) {
    for (const std::uint32_t call : {0x94000001u, 0xd63f00a0u, 0xd73f08a6u}) {  // bl .+4 ; blr x5 ; blraa x5, x6
        const std::uint32_t code[] = {
            0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone: its store is at 0x08
            0xb5000088,  // 0x04 cbnz x8, 0x14       reach the use, monitor live
            0xc80a7c08,  // 0x08 stxr w10, x8, [x0]
            0x35ffffaa,  // 0x0c cbnz w10, 0x00
            0xd65f03c0,  // 0x10 ret
            0x91000652,  // 0x14 add x18, x18, #1    the use: monitor live, but a call follows
            call,        // 0x18 bl / blr x5 / blraa x5, x6: a call ends the pairing
            0xc80b7c08,  // 0x1c stxr w11, x8, [x0]  a store across the call: not paired with L0
            0x35ffff0b,  // 0x20 cbnz w11, 0x00
        };
        const auto findings = scan_words(code, code + std::size(code), 0x1000);
        CHECK(findings.x18_unsupported.empty());
        bool islanded = false;
        for (const auto &site : findings.sites) if (site.address == 0x1014) islanded = true;
        CHECK(islanded);
    }
}

// A return ends the monitor path: a store after it is not reached, so the use before it islands. Pins
// both authenticated returns RETAA and RETAB (and, with the ret-x5 test, RET x30) as path ends.
TEST(a_return_ends_the_monitor_path_so_the_use_before_it_islands) {
    for (const std::uint32_t ret : {0xd65f0bffu, 0xd65f0fffu}) {  // retaa ; retab
        const std::uint32_t code[] = {
            0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone: its store is at 0x08
            0xb5000088,  // 0x04 cbnz x8, 0x14       reach the use, monitor live
            0xc80a7c08,  // 0x08 stxr w10, x8, [x0]
            0x35ffffaa,  // 0x0c cbnz w10, 0x00
            0xd65f03c0,  // 0x10 ret
            0x91000652,  // 0x14 add x18, x18, #1    the use: monitor live, but a return follows
            ret,         // 0x18 retaa / retab       the return ends the path
            0xc80b7c08,  // 0x1c stxr w11, x8, [x0]  a store after the return: not reached
            0x35ffff0b,  // 0x20 cbnz w11, 0x00
        };
        const auto findings = scan_words(code, code + std::size(code), 0x1000);
        CHECK(findings.x18_unsupported.empty());
        bool islanded = false;
        for (const auto &site : findings.sites) if (site.address == 0x1014) islanded = true;
        CHECK(islanded);
    }
}

// `ret xN` with N != 30 is an indirect branch, not a return (hand-written code uses it as a computed
// jump). So a monitor reaching it escapes and the use stays refused, as for BR.
TEST(a_ret_to_a_register_other_than_x30_is_an_escape_not_a_return) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone: its store is at 0x08
        0xb5000088,  // 0x04 cbnz x8, 0x14       reach the use, monitor live
        0xc80a7c08,  // 0x08 stxr w10, x8, [x0]
        0x35ffffaa,  // 0x0c cbnz w10, 0x00
        0xd65f03c0,  // 0x10 ret                 the normal return (x30), a safe path end
        0x91000652,  // 0x14 add x18, x18, #1    the use: monitor live
        0xd65f00a0,  // 0x18 ret x5              an indirect branch via x5: the escape
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x1014) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1014);
}

// ADR-0020 step D: a window that does not close within 64 words no longer blanket-refuses its x18
// uses; each one is decided by the C6 monitor-escape test, under the same named call/return limit.

// FA: the use islands when a call (a C6 boundary) precedes any store on its path, so nothing pairs
// with it. The store is more than 64 words on, so the window does not close (window_unclosed today).
TEST(an_x18_use_in_an_unclosed_window_islands_when_a_call_precedes_its_store) {
    std::vector<std::uint32_t> code{
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone: a store lies far ahead
        0x8b120109,  // 0x04 add x9, x8, x18     the use: monitor live, but a call follows before any store
        0x94000001,  // 0x08 bl .+4              a call: the C6 boundary, nothing pairs across it
    };
    code.insert(code.end(), 62, 0xd503201f);  // 0x0c..0x104: 62 nops, pushing the store past 64 words
    code.push_back(0xc80a7c09);               // 0x104 stxr w10, x9, [x0]  not paired with L0 (a call between)
    const auto findings = scan_words(code.data(), code.data() + code.size(), 0x1000);
    CHECK(findings.x18_unsupported.empty());
    bool islanded = false;
    for (const auto &site : findings.sites) if (site.address == 0x1004) islanded = true;
    CHECK(islanded);
}

// FB: the same with the call BEFORE the use. The use then reaches the store with no boundary between,
// so it escapes and stays refused (the deferred C3b far pair) — now with the C6 reason, not
// window_unclosed.
TEST(an_x18_use_after_a_call_in_an_unclosed_window_stays_refused) {
    std::vector<std::uint32_t> code{
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone
        0x94000001,  // 0x04 bl .+4              a call, before the use
        0x8b120109,  // 0x08 add x9, x8, x18     the use: reaches the store with no boundary after it
    };
    code.insert(code.end(), 62, 0xd503201f);  // 0x0c..0x104
    code.push_back(0xc80a7c09);               // 0x104 stxr w10, x9, [x0]  reachable straight-line from the use
    const auto findings = scan_words(code.data(), code.data() + code.size(), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported)
        if (use.address == 0x1008) {
            refused = true;
            // the C6 escape reason, no longer the window_unclosed blanket
            CHECK(std::string_view(use.reason).find("does not close") == std::string_view::npos);
        }
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1008);
}

// FC (guard ii): a window whose monitor closure reaches a register branch keeps its blanket refusal.
// The branch's targets are invisible to the C6 walk, so a use at one (0x08 here, reachable only
// through the br, as in a jump-table retry loop) could sit in a live LL/SC. Islanding it would
// livelock on the device; the whole window stays refused.
TEST(an_unclosed_window_that_reaches_a_register_branch_stays_refused) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone (the register branch leaves its run)
        0xd61f0120,  // 0x04 br x9               a register branch in the closure: its targets are unseen
        0x0b12010a,  // 0x08 add w10, w8, w18    a use reachable only through the br
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x1008) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1008);
}

// FD: a use whose monitor leaves the scanned section through a direct branch islands — nothing across a
// section boundary pairs with the load (limit 2, a step-D choice: one output section, lld having merged
// every .text.* into it). The oracle's section_exit toggle makes this dependence explicit and counted.
TEST(an_x18_use_that_leaves_the_section_islands_on_the_section_exit_limit) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone (a branch leaves its run)
        0x8b120109,  // 0x04 add x9, x8, x18     the use: its monitor leaves the section below, pairing nothing
        0xb5002009,  // 0x08 cbnz x9, 0x408      a branch past the section's end
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    CHECK(findings.x18_unsupported.empty());
    bool islanded = false;
    for (const auto &site : findings.sites) if (site.address == 0x1004) islanded = true;
    CHECK(islanded);
}

// FE: the load-exclusive itself, when it is an x18 use (ldxr x18), stays refused as an exclusive
// access even under D1 — a non-lone load's island memory could break its LL/SC (unsupported_reason).
TEST(an_ldxr_x18_in_an_unclosed_window_stays_refused) {
    std::vector<std::uint32_t> code{
        0xc85f7c12,  // 0x00 ldxr x18, [x0]      L0 and an x18 use at once, non-lone
    };
    code.insert(code.end(), 64, 0xd503201f);  // 0x04..0x104: nops pushing the store past 64 words
    code.push_back(0xc80a7c12);               // 0x104 stxr w10, x18, [x0]
    const auto findings = scan_words(code.data(), code.data() + code.size(), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x1000) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1000);
}

// FC, two variants that drop guard ii would island: the register branch comes before the x18 use, so the
// use sits only at the branch's unseen target, inside a window that still reaches its store far ahead.
// branch_reached_sequence never reaches the use (it stops at the branch), so without guard ii's blanket
// refusal monitor_escapes would miss it and the use would island into a live LL/SC — a device livelock.
// A br and a `ret xN` used as an indirect jump are both such branches; the oracle flags each if islanded.
TEST(an_unclosed_window_whose_register_branch_target_reaches_a_store_stays_refused) {
    std::vector<std::uint32_t> code{
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone
        0xd61f0120,  // 0x04 br x9               a register branch; its unseen target may re-enter the window
        0x8b12010a,  // 0x08 add x10, x8, x18    a use only the branch reaches, the store still ahead
    };
    code.insert(code.end(), 62, 0xd503201f);  // 0x0c..0x100: nops, pushing the store past 64 words
    code.push_back(0xc80b7c0a);               // 0x104 stxr w11, x10, [x0]  pairs with L0 if the monitor is live
    const auto findings = scan_words(code.data(), code.data() + code.size(), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x1008) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1008);
}

TEST(an_unclosed_window_whose_ret_used_as_a_branch_reaches_a_store_stays_refused) {
    std::vector<std::uint32_t> code{
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone
        0xd65f0120,  // 0x04 ret x9              a register branch (ret xN, N != 30), its target unseen
        0x8b12010a,  // 0x08 add x10, x8, x18    a use only the branch reaches, the store still ahead
    };
    code.insert(code.end(), 62, 0xd503201f);  // 0x0c..0x100: nops, pushing the store past 64 words
    code.push_back(0xc80b7c0a);               // 0x104 stxr w11, x10, [x0]
    const auto findings = scan_words(code.data(), code.data() + code.size(), 0x1000);
    bool refused = false;
    for (const auto &use : findings.x18_unsupported) if (use.address == 0x1008) refused = true;
    CHECK(refused);
    for (const auto &site : findings.sites) CHECK(site.address != 0x1008);
}

// Step E sizing (ADR-0020): a window that does not close and whose closure reaches a register branch
// stays refused (guard ii), but its reason now names whether that branch is a recognized jump table in
// read-only data. A re-import's per-reason counts then separate the step-E-eligible windows (the branch
// is a jump table the rewriter could follow with landing pads) from the C7-C9 ones, with no conversion
// change here. This window reaches a br x9 the rewriter cannot read as a table -> the register-branch reason.
TEST(an_unclosed_window_reaching_a_non_jump_table_register_branch_names_the_register_branch) {
    const std::uint32_t code[] = {
        0xc85f7c08,  // 0x00 ldxr x8, [x0]       L0, non-lone
        0xd61f0120,  // 0x04 br x9               a register branch, not a jump table
        0x0b12010a,  // 0x08 add w10, w8, w18    an x18 use in the window
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    const auto refused = std::ranges::find(findings.x18_unsupported, 0x1008u, &apk::rewrite::Unsupported::address);
    CHECK(refused != findings.x18_unsupported.end());
    if (refused != findings.x18_unsupported.end()) {
        CHECK(std::string_view(refused->reason).find("register branch") != std::string_view::npos);
        CHECK(std::string_view(refused->reason).find("jump table") == std::string_view::npos);
    }
}

// Step E sizing: the same, but the window's closure reaches a br x18 that IS a recognized jump table in
// read-only data (clang's dispatch, as in SyntheticJumpTable, shifted by the ldxr and the x18 use; table
// at 0x20040). The use stays refused, but with the jump-table reason: step E could follow the table.
TEST(an_unclosed_window_reaching_a_recognized_jump_table_names_the_jump_table) {
    const std::vector<std::uint32_t> code = {
        0xc85f7c08,  // 0x10000 ldxr x8, [x0]              L0, non-lone (the br leaves its run)
        0x8b12010a,  // 0x10004 add x10, x8, x18           an x18 use in the window
        0x71000e5f,  // 0x10008 cmp w18, #3
        0x540000e8,  // 0x1000c b.hi 0x10028
        0x90000089,  // 0x10010 adrp x9, 0x20000
        0x91010129,  // 0x10014 add x9, x9, #0x40
        0xb8b27932,  // 0x10018 ldrsw x18, [x9, x18, lsl #2]
        0x8b090252,  // 0x1001c add x18, x18, x9
        0xd61f0240,  // 0x10020 br x18                      a recognized jump table: step E could follow it
        0xd503201f,  // 0x10024 nop                         cases 0 and 2
        0xd65f03c0,  // 0x10028 ret                         cases 1, 3 and the default
    };
    const std::vector<std::int32_t> table = {
        0x10024 - 0x20040, 0x10028 - 0x20040, 0x10024 - 0x20040, 0x10028 - 0x20040};
    apk::rewrite::Findings findings;
    apk::rewrite::scan(std::as_bytes(std::span(code)), 0x10000, findings,
                       [&table](std::uint64_t a, std::size_t s) -> std::span<const std::byte> {
                           if (a < 0x20040 || a + s > 0x20040 + 4 * table.size()) return {};
                           return std::as_bytes(std::span(table)).subspan(a - 0x20040, s);
                       });
    const auto refused = std::ranges::find(findings.x18_unsupported, 0x10004u, &apk::rewrite::Unsupported::address);
    CHECK(refused != findings.x18_unsupported.end());
    if (refused != findings.x18_unsupported.end()) {
        CHECK(std::string_view(refused->reason).find("jump table") != std::string_view::npos);
    }
}

// A brute-force soundness oracle for step D (ADR-0020, the lead's condition 2). Its reachability and
// escape walks are written apart from scan()'s branch_reached_sequence/monitor_escapes; the instruction
// classification is the same ISA fact scan uses (the exclusive, CLREX and branch-register masks, which
// converter.a64_objdump validates over real code), while the branch displacements are decoded here from
// the Arm tables (C4.1). It looks for a live path LDXR -> islanded x18 site -> STXR over fall-through and
// direct branches. A register branch (BR, or ret xN with N != 30) is an unfollowable target: a monitor
// that reaches one may reach a store, and its own target is taken to be any word of the section (guard
// ii). When a toggle is OFF a section exit or a call/return is treated as "may reach a store" too, so the
// two load-bearing limits of step D are made visible and counted:
//   - call_return: ON = a call or return (BL, BLR, RET) ends the monitor path (the C6 compiler limit, FA);
//   - section_exit: ON = the section end, or a branch out of it, pairs nothing (C6, within one output section).
// Under both ON the oracle flags no island; with one OFF it flags the islands that rest on that limit.
namespace oracle {
constexpr bool obit(std::uint32_t w, unsigned n) { return ((w >> n) & 1u) != 0; }
constexpr std::uint32_t ofield(std::uint32_t w, unsigned hi, unsigned lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1);
}
// Load/store exclusive (C4.1): bits 29:23 = 0b0001000 are the register and pair monitor forms, where o1
// (bit 21) set with the size-high bit (31) clear is CASP, an LSE atomic with no monitor; bits 29:23 =
// 0b0010010 are LDTXR/STTXR. bit 22 (L) is load versus store.
constexpr bool o_excl(std::uint32_t w) {
    if (ofield(w, 29, 23) == 0b0010000u) return !obit(w, 21) || obit(w, 31);
    return ofield(w, 29, 23) == 0b0010010u;
}
constexpr bool o_load_excl(std::uint32_t w) { return o_excl(w) && obit(w, 22); }
constexpr bool o_store_excl(std::uint32_t w) { return o_excl(w) && !obit(w, 22); }
// CLREX (C6.2): 1101 0101 0000 0011 0011 CRm 0101 1111, CRm the barrier operand.
constexpr bool o_clrex(std::uint32_t w) { return (w & 0xffff'f0ffu) == 0xd503'305fu; }
// Unconditional branch (register) (C4.1.7). RET x30 and the authenticated RETAA/RETAB end a path where
// compiler code pairs no LL/SC (a safe return); BR and its forms, and ret xN with N != 30 used as an
// indirect jump, are register branches whose target cannot be followed; BLR and its forms are calls.
constexpr bool o_reg_return(std::uint32_t w) {
    return w == 0xd65f'03c0u || w == 0xd65f'0bffu || w == 0xd65f'0fffu;  // RET x30, RETAA, RETAB
}
constexpr bool o_reg_call(std::uint32_t w) { return (w & 0xfeff'0000u) == 0xd63f'0000u; }  // BLR and forms
constexpr bool o_reg_branch(std::uint32_t w) {
    if ((w & 0xfeff'0000u) == 0xd61f'0000u) return true;            // BR and its forms
    return (w & 0xfeff'0000u) == 0xd65f'0000u && !o_reg_return(w);  // ret xN with N != 30
}
constexpr bool o_bl(std::uint32_t w) { return (w & 0xfc00'0000u) == 0x9400'0000u; }        // BL (a call)
constexpr bool o_b_uncond(std::uint32_t w) { return (w & 0xfc00'0000u) == 0x1400'0000u; }  // B (no fall-through)
// The byte displacement of a PC-relative direct branch, else 0 (C4.1), each field sign-extended and
// scaled by 4: B and BL (imm26); B.cond and BC.cond, told apart only by bit 4 and sharing imm19; CBZ and
// CBNZ (imm19); TBZ and TBNZ (imm14); and the compare-and-branch CMPBR/CBB/CBH (imm9 at bits 13:5).
std::int64_t o_disp(std::uint32_t w) {
    const auto scaled = [](std::uint32_t field, unsigned bits) {
        return static_cast<std::int64_t>(static_cast<std::int32_t>(field << (32u - bits)) >> (32u - bits)) * 4;
    };
    if ((w & 0x7c00'0000u) == 0x1400'0000u) return scaled(ofield(w, 25, 0), 26);  // B, BL
    if (ofield(w, 31, 24) == 0x54u) return scaled(ofield(w, 23, 5), 19);          // B.cond, BC.cond
    if (ofield(w, 30, 25) == 0b011010u) return scaled(ofield(w, 23, 5), 19);      // CBZ, CBNZ
    if (ofield(w, 30, 25) == 0b011011u) return scaled(ofield(w, 18, 5), 14);      // TBZ, TBNZ
    if (ofield(w, 30, 25) == 0b111010u) return scaled(ofield(w, 13, 5), 9);       // CMPBR, CBB, CBH
    return 0;
}
// Whether, from `from`, a store-exclusive is reachable with the monitor live over fall-through and direct
// branches. A register branch is an unfollowable target counted as a store; a CLREX or a load clears or
// re-arms the monitor (a safe boundary); a call or return is safe only while call_return is ON, and a
// section exit only while section_exit is ON.
bool reaches_store(const std::uint32_t *code, std::size_t words, std::size_t from, Toggles t) {
    std::vector<std::size_t> work{from};
    std::vector<bool> seen(words, false);
    while (!work.empty()) {
        const auto p = work.back();
        work.pop_back();
        if (p >= words || seen[p]) continue;
        seen[p] = true;
        const auto w = code[p];
        if (o_store_excl(w) || o_reg_branch(w)) return true;  // the pairing store, or an unfollowable target
        if (o_clrex(w) || o_load_excl(w)) continue;           // monitor cleared or re-armed: a safe boundary
        if (o_bl(w) || o_reg_call(w) || o_reg_return(w)) {    // a call or a return
            if (!t.call_return) return true;                  // not trusted to end the pairing (C6 off)
            continue;                                         // trusted: a safe boundary
        }
        if (!o_b_uncond(w)) {  // fall through, unless an unconditional B
            if (p + 1 >= words) { if (!t.section_exit) return true; }  // off the section's end
            else work.push_back(p + 1);
        }
        if (const auto d = o_disp(w); d != 0) {
            const auto target = static_cast<std::int64_t>(p) * 4 + d;
            if (target < 0 || target / 4 >= static_cast<std::int64_t>(words)) {
                if (!t.section_exit) return true;  // a branch out of the section
            } else {
                work.push_back(static_cast<std::size_t>(target / 4));
            }
        }
    }
    return false;
}
// Whether register `reg` is proven dead right after the use at word `use`: an independent forward walk
// (the mirror of the production predicate, not a call into it) that stops at the first access or branch.
// A no-save island loads the guest x18 into `reg` and clobbers it, which is sound only if the guest reads
// `reg` no more before overwriting it. A read before any write means the value is live (the island would
// corrupt it); any branch before a write means the forward value cannot be followed, so not proven dead.
bool register_dead(const std::uint32_t *begin, std::size_t words, std::size_t use, unsigned reg) {
    const std::uint32_t bit = 1u << reg;
    for (std::size_t p = use + 1; p < words; ++p) {
        const auto w = begin[p];
        const auto u = apk::a64::register_use(w);
        if ((u.reads & bit) != 0) return false;   // read before any write: live
        if ((u.writes & bit) != 0) return true;   // written before any read: dead
        if (o_disp(w) != 0 || o_reg_branch(w) || o_reg_call(w) || o_reg_return(w)) return false;  // a branch
    }
    return false;  // the section ended before a write
}
// The unsound x18 islands (addresses) that lie on a live path LDXR -> site -> STXR under `t`. For each
// load-exclusive it walks the words its monitor reaches (fall-through and direct branches); a register
// branch's target is unseen, so the whole section is then taken to be reachable (guard ii). An island
// both reached from a load and from which reaches_store() holds lies on a live LL/SC path.
//
// C3b's exact soundness condition (ADR-0020 C7, the lead's step-7 claim): the ONLY island allowed on a
// live path is a no-save island (SiteKind::x18_nosave) -- it spills nothing, touching only the TSD granule,
// which C3b measured KEEPS the local monitor -- AND only over register(s) proven dead right after the use.
// A red-zone-spilling x18 island on a live path stores to the stack granule inside the window and can break
// the monitor, so it is flagged; a no-save island whose value (or store-back base) register is NOT dead is
// flagged too. Anything flagged is unsound.
std::vector<std::uint64_t> flags(const std::uint32_t *begin, const std::uint32_t *end, std::uint64_t base,
                                 Toggles t, const apk::rewrite::Findings &findings) {
    const auto words = static_cast<std::size_t>(end - begin);
    struct Island { std::size_t pos; SiteKind kind; std::uint8_t reg; std::uint8_t base; };
    std::vector<Island> islands;
    for (const auto &site : findings.sites) {
        if (site.kind == SiteKind::x18 || site.kind == SiteKind::x18_nosave)
            islands.push_back({static_cast<std::size_t>((site.address - base) / 4), site.kind, site.reg, site.base});
    }
    std::vector<std::uint64_t> flagged;
    for (std::size_t load = 0; load < words; ++load) {
        if (!o_load_excl(begin[load])) continue;
        std::vector<bool> from_load(words, false);
        std::vector<std::size_t> work{load + 1};  // the monitor is live from the word after the load
        bool reached_register_branch = false;
        while (!work.empty()) {
            const auto p = work.back();
            work.pop_back();
            if (p >= words || from_load[p]) continue;
            from_load[p] = true;
            const auto w = begin[p];
            if (o_reg_branch(w)) { reached_register_branch = true; continue; }  // target unseen: see below
            if (o_store_excl(w) || o_clrex(w) || o_load_excl(w)) continue;      // monitor consumed/cleared/re-armed
            if (t.call_return && (o_bl(w) || o_reg_call(w) || o_reg_return(w))) continue;  // the C6 call/return limit
            if (!o_b_uncond(w) && !o_reg_return(w) && p + 1 < words) work.push_back(p + 1);
            if (const auto d = o_disp(w); d != 0) {
                const auto target = static_cast<std::int64_t>(p) * 4 + d;
                if (target >= 0 && target / 4 < static_cast<std::int64_t>(words)) {
                    work.push_back(static_cast<std::size_t>(target / 4));
                }
            }
        }
        if (reached_register_branch) from_load.assign(words, true);  // the branch may re-enter any word (guard ii)
        for (const auto &island : islands) {
            if (!(from_load[island.pos] && reaches_store(begin, words, island.pos, t))) continue;  // not on a live path
            // On a live LL/SC path: sound only as a no-save island (no spill) over register(s) proven dead.
            const bool sound = island.kind == SiteKind::x18_nosave &&
                               register_dead(begin, words, island.pos, island.reg) &&
                               (island.base == APK_ISLAND_NO_REGISTER ||
                                register_dead(begin, words, island.pos, island.base));
            if (!sound) flagged.push_back(base + 4 * island.pos);
        }
    }
    std::ranges::sort(flagged);
    flagged.erase(std::ranges::unique(flagged).begin(), flagged.end());
    return flagged;
}
// Scans `begin..end` at `base`, then flags its islands. scan_words() calls this; the overload above takes a
// hand-built findings list so a test can give the flagging logic an island scan() would never emit.
std::vector<std::uint64_t> flags(const std::uint32_t *begin, const std::uint32_t *end, std::uint64_t base, Toggles t) {
    const std::span code(reinterpret_cast<const std::byte *>(begin), 4 * static_cast<std::size_t>(end - begin));
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, base, findings);  // directly, so scan_words() may call flags() without recursion
    return flags(begin, end, base, t, findings);
}
}  // namespace oracle

// With both limits on the oracle finds no unsound island; with one off it flags the island that rests on
// that limit — FA's on the call/return limit, the section-exit island on limit 2 — so each dependence
// is visible and counted (the lead's step-D condition 2).
TEST(the_brute_force_oracle_flags_the_toggle_dependent_islands) {
    std::vector<std::uint32_t> fa{0xc85f7c08, 0x8b120109, 0x94000001};  // ldxr; add x9,x8,x18; bl
    fa.insert(fa.end(), 62, 0xd503201f);                                // nops past 64 words
    fa.push_back(0xc80a7c09);                                           // stxr across the call
    const std::uint32_t sx[] = {0xc85f7c08, 0x8b120109, 0xb5002009};    // ldxr; add x9,x8,x18; cbnz out of section

    using oracle::flags;
    const std::vector<std::uint64_t> fa_site{0x1004}, sx_site{0x1004}, none{};
    CHECK(flags(fa.data(), fa.data() + fa.size(), 0x1000, {true, true}) == none);
    CHECK(flags(sx, sx + std::size(sx), 0x1000, {true, true}) == none);
    CHECK(flags(fa.data(), fa.data() + fa.size(), 0x1000, {false, true}) == fa_site);  // rests on call/return
    CHECK(flags(sx, sx + std::size(sx), 0x1000, {true, false}) == sx_site);  // rests on the section-exit limit
}

// (b) ADR-0020 C7, step 7: the oracle accepts a no-save island on a live LL/SC path ONLY over a register
// proven dead, and its deadness check has teeth. A sound no-save window (the back-edge shape, x5 written
// before any read) is flagged by nothing; register_dead(), which flags() applies to every no-save island,
// calls a register dead when it is written before any read and NOT dead when it is read first or a branch
// comes before the write -- a no-save island over such a register would be flagged as unsound.
TEST(the_oracle_accepts_a_dead_register_no_save_island_and_rejects_a_live_one) {
    using oracle::flags;
    using oracle::register_dead;
    const std::uint32_t sound[] = {0x1400'0003, 0xc802'7c03, 0x3400'00a2, 0xc85f'7c01,
                                   0x8b12'0023, 0xd280'0005, 0x17ff'fffb, 0xd503'201f};  // x5 dead after the use
    CHECK(flags(sound, sound + std::size(sound), 0x1000, {true, true}).empty());  // sound: no spill, dead register
    const std::uint32_t dead[] = {0x8b12'0023, 0xd280'0005};    // add x3,x1,x18 ; mov x5,#0  (x5 written)
    const std::uint32_t live[] = {0x8b12'0023, 0x8b00'00a3};    // add x3,x1,x18 ; add x3,x5,x0 (x5 read first)
    const std::uint32_t branch[] = {0x8b12'0023, 0x1400'0001};  // add x3,x1,x18 ; b  (a branch before any write)
    CHECK(register_dead(dead, std::size(dead), 0, 5));
    CHECK(!register_dead(live, std::size(live), 0, 5));
    CHECK(!register_dead(branch, std::size(branch), 0, 5));
}

// (b) ADR-0020 C7, step 7 (the lead's option 1): flags()-level teeth. scan() never emits a no-save island
// over a live register (its predicate is correct), so to prove the oracle would catch one we hand it a
// findings list it would never produce -- one x18_nosave site over x5, in a window that reads x5 right
// after the use -- and flags() must report it. The site sits on the live LL/SC path (ldxr 0x100c, the use,
// the back-edge to the store-exclusive 0x1004).
TEST(the_oracle_flags_a_hand_built_no_save_island_over_a_live_register) {
    const std::uint32_t code[] = {
        0x1400'0003,  // 0x1000 b 0x100c
        0xc802'7c03,  // 0x1004 stxr w2, x3, [x0]
        0x3400'00a2,  // 0x1008 cbz w2, 0x101c
        0xc85f'7c01,  // 0x100c ldxr x1, [x0]
        0x8b12'0023,  // 0x1010 add x3, x1, x18: the use (the injected site)
        0x8b00'00a4,  // 0x1014 add x4, x5, x0: reads x5, so x5 is live after the use
        0x17ff'fffb,  // 0x1018 b 0x1004
        0xd503'201f,  // 0x101c nop
    };
    apk::rewrite::Findings injected;  // what scan() would never build: a no-save island over the live x5
    injected.sites.push_back({0x1010, SiteKind::x18_nosave, 5, 0x8b12'0023u});  // base defaults to NO_REGISTER
    CHECK(oracle::flags(code, code + std::size(code), 0x1000, {true, true}, injected) ==
          std::vector<std::uint64_t>{0x1010});
}

// The relocated island: x18 becomes the scratch everywhere, the guest value is loaded before and
// stored after the window, and the store-exclusive keeps working on the same address.
TEST(the_relocated_exclusive_island_uses_the_scratch_and_stores_x18_after) {
    const std::uint32_t code[] = {0xc85f'7db1, 0x8b0e'0232, 0xc810'fdb2, 0x35ff'ffb0};  // shape A
    const auto findings = scan_words(code, code + 4, 0x1000);
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() != 1) return;
    const auto &region = findings.exclusive_regions[0];

    CHECK(region.scratch == 0);  // value in x0
    CHECK(region.base == 1);     // TSD base in x1
    std::vector<std::byte> stub(apk::rewrite::exclusive_region_stub_size(region));
    const auto stub_address = address_of(ref_site) - 0x100;  // anywhere; only the final b depends on it
    const auto patch = apk::rewrite::emit_exclusive_region(region, address_of(ref_site), stub_address, stub);

    // The load-exclusive is replaced by a branch to the stub.
    CHECK((patch & 0xfc00'0000) == 0x1400'0000);
    // The load-exclusive loads x17, not x18, so the island loads the guest x18 before the window
    // (Finding D); x18 is written, so it also saves both scratch registers in the red zone and stores
    // the value back.
    const auto words = stub.size() / 4;
    CHECK(words == 1 /* save value */ + 1 /* save base */ + 3 /* load x18 */ + 3 /* window */ +
                       3 /* mrs, bic, str */ + 2 /* restore value, base */ + 1 /* b */);
    for (std::size_t index = 0; index < words; ++index) {
        const auto use = apk::a64::register_use(word(stub, index));
        CHECK(((use.reads | use.writes) & (1u << 18)) == 0);  // no word still names x18
    }
    CHECK(word(stub, 0) == 0xf818'03e0);   // stur x0, [sp, #-128]
    CHECK(word(stub, 1) == 0xf818'83e1);   // stur x1, [sp, #-120]
    CHECK(word(stub, 2) == 0xd53b'd060);   // mrs x0, TPIDRRO_EL0 (load the guest x18)
    CHECK(word(stub, 3) == 0x927d'f000);   // bic x0, x0, #7
    CHECK(word(stub, 4) == 0xf940'2c00);   // ldr x0, [x0, #88]
    // The window keeps its shape, x18 renamed to x0: ldxr (no x18, unchanged), add, stlxr.
    CHECK(word(stub, 5) == 0xc85f'7db1);   // ldxr x17, [x13]
    CHECK(word(stub, 6) == 0x8b0e'0220);   // add x0, x17, x14
    CHECK(word(stub, 7) == 0xc810'fda0);   // stlxr w16, x0, [x13]
    CHECK(word(stub, 8) == 0xd53b'd061);   // mrs x1, TPIDRRO_EL0 (the TSD base)
    CHECK(word(stub, 10) == 0xf900'2c20);  // str x0, [x1, #88]
    CHECK(branch_target_at(stub, 13, stub_address) == address_of(ref_site) + 4 * 3);  // after the window
}

// A CMPBR (FEAT_CMPBR compare-and-branch, two registers and a PC-relative label) is a branch within
// a window like B.cond: a relocated window may hold one, x18 renamed where it uses it, and the island
// keeps it aimed at the same word of the copied window (step C class 7 groundwork, Finding A).
TEST(a_cmpbr_in_a_relocated_window_is_kept_and_retargeted) {
    const std::uint32_t code[] = {
        0xc85f'7c20,  // ldxr x0, [x1]
        0x9100'0412,  // add x18, x0, #1
        0xf4c4'0043,  // cmpbr cbeq x3, x4, .+8  (skips the next word, lands on the store-exclusive)
        0xd280'0005,  // mov x5, #0
        0xc802'7c32,  // stxr w2, x18, [x1]
        0x35ff'ff62,  // cbnz w2, ldxr
    };
    const auto findings = scan_words(code, code + std::size(code), 0x10000);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.exclusive_regions.size() != 1) return;
    const auto &region = findings.exclusive_regions[0];
    std::vector<std::byte> stub(apk::rewrite::exclusive_region_stub_size(region));
    apk::rewrite::emit_exclusive_region(region, address_of(ref_site), address_of(ref_site) - 0x100, stub);
    // The window is copied contiguously, so the cmpbr keeps its +8 and stays byte-for-byte itself (it
    // names no x18). The ldxr loads x0, not x18, so the island loads x18 first (Finding D): index 7 =
    // save value, save base, [mrs, bic, ldr to load x18], ldxr, add, cmpbr.
    CHECK(word(stub, 7) == 0xf4c4'0043);
}

// --- Jump tables through x18 (ADR-0020 X2d) --------------------------------------------------------

/// The reference table as the only read-only data the scanned code may use.
std::span<const std::byte> reference_table(std::uint64_t address, std::size_t size) {
    const auto table = address_of(ref_x18_jump_table);
    if (address < table || address + size > table + 4 * 4) return {};
    return {reinterpret_cast<const std::byte *>(address), size};
}

// The reference function, assembled and linked: the scanner finds the table through ADRP and ADD,
// its size through the bounds check, and the targets through the table.
TEST(a_jump_table_through_x18_is_found_with_its_targets) {
    const auto start = address_of(ref_x18_jump_function);
    const std::span code(reinterpret_cast<const std::byte *>(ref_x18_jump_function),
                         4 * static_cast<std::size_t>(ref_x18_jump_function_end - ref_x18_jump_function));
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, start, findings, reference_table);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.jump_tables.size() == 1);
    if (findings.jump_tables.size() != 1) return;
    const auto &table = findings.jump_tables[0];
    CHECK(table.address == address_of(ref_x18_jump_site));
    CHECK(table.table == address_of(ref_x18_jump_table));
    CHECK(table.entries == 4);
    // Ascending, and the repeated target only once.
    CHECK(table.targets == (std::vector<std::uint64_t>{address_of(ref_x18_jump_case0), address_of(ref_x18_jump_case1),
                                                       address_of(ref_x18_jump_case2)}));
    // The cmp, ldrsw and add on x18 stay islands of their own.
    std::vector<std::uint64_t> islands;
    for (const auto &site : findings.sites) islands.push_back(site.address);
    CHECK(islands == (std::vector<std::uint64_t>{start, start + 16, start + 20}));
}

// The island reads T, checks T - lo against the span and branches through a landing pad that
// restores both scratch registers; every word must be what the assembler and the linker made.
TEST(the_jump_table_island_and_its_landing_pads_match_the_assembler) {
    const apk::rewrite::JumpTable table{
        .address = address_of(ref_x18_jump_site),
        .table = address_of(ref_x18_jump_table),
        .entries = 4,
        .targets = {address_of(ref_x18_jump_case0), address_of(ref_x18_jump_case1), address_of(ref_x18_jump_case2)},
    };
    const auto words = static_cast<std::size_t>(ref_x18_island_jump_end - ref_x18_island_jump);
    std::vector<std::byte> stub(apk::rewrite::jump_table_stub_size(table));
    CHECK(stub.size() == 4 * words);
    if (stub.size() != 4 * words) return;
    const auto island = address_of(ref_x18_island_jump);
    const auto patch = apk::rewrite::emit_jump_table(table, table.address, island, stub);
    for (std::size_t index = 0; index < words; ++index) {
        if (word(stub, index) != ref_x18_island_jump[index]) {
            apk::testing::fail(std::source_location::current(), "word differs: ",
                               std::format("{:#010x} at {}: {:#010x} expected", word(stub, index), index,
                                           ref_x18_island_jump[index]).c_str());
        }
    }
    CHECK(patch == (0x1400'0000 | (static_cast<std::uint32_t>((island - table.address) / 4) & 0x03ff'ffff)));
}

/// A jump table at 0x10000 as clang 8 builds it, with its table in read-only data at 0x20040.
struct SyntheticJumpTable {
    std::vector<std::uint32_t> code = {
        0x7100'0e5f,  // 0x10000 cmp w18, #3
        0x5400'00e8,  // 0x10004 b.hi 0x10020
        0x9000'0089,  // 0x10008 adrp x9, 0x20000
        0x9101'0129,  // 0x1000c add x9, x9, #0x40
        0xb8b2'7932,  // 0x10010 ldrsw x18, [x9, x18, lsl #2]
        0x8b09'0252,  // 0x10014 add x18, x18, x9
        0xd61f'0240,  // 0x10018 br x18
        0xd503'201f,  // 0x1001c nop: cases 0 and 2
        0xd65f'03c0,  // 0x10020 ret: cases 1 and 3, and the default
    };
    std::vector<std::int32_t> table = {0x1001c - 0x20040, 0x10020 - 0x20040, 0x1001c - 0x20040, 0x10020 - 0x20040};

    apk::rewrite::Findings scan() const {
        apk::rewrite::Findings findings;
        apk::rewrite::scan(std::as_bytes(std::span(code)), 0x10000, findings,
                           [this](std::uint64_t address, std::size_t size) -> std::span<const std::byte> {
                               if (address < 0x20040 || address + size > 0x20040 + 4 * table.size()) return {};
                               return std::as_bytes(std::span(table)).subspan(address - 0x20040, size);
                           });
        return findings;
    }
};

void check_refused(const apk::rewrite::Findings &findings, std::string_view why,
                   std::source_location location = std::source_location::current()) {
    if (!findings.jump_tables.empty()) apk::testing::fail(location, "a jump table was found");
    const auto refused = std::ranges::find(findings.x18_unsupported, 0x10018u, &apk::rewrite::Unsupported::address);
    if (refused == findings.x18_unsupported.end()) {
        apk::testing::fail(location, "the br x18 was not refused");
    } else if (std::string_view(refused->reason).find(why) == std::string_view::npos) {
        apk::testing::fail(location, "unexpected reason: ", refused->reason);
    }
}

/// The br x18 at `at` ends no jump table the rewriter recognizes, so it becomes a general indirect
/// island (ADR-0020 X2e), not a refusal: the app must run even without the jump table CFI.
void check_indirect_island(const apk::rewrite::Findings &findings, std::uint64_t at = 0x10018u,
                           std::source_location location = std::source_location::current()) {
    if (!findings.jump_tables.empty()) apk::testing::fail(location, "a jump table was found");
    const auto refused = std::ranges::find(findings.x18_unsupported, at, &apk::rewrite::Unsupported::address);
    if (refused != findings.x18_unsupported.end()) {
        apk::testing::fail(location, "the br x18 was refused: ", refused->reason);
    }
    const auto site = std::ranges::find_if(findings.sites, [at](const auto &s) { return s.address == at; });
    if (site == findings.sites.end()) {
        apk::testing::fail(location, "the br x18 is not an island site");
    } else if (apk::rewrite::island_layout(*site).kind != APK_ISLAND_INDIRECT) {
        apk::testing::fail(location, "the br x18 site is not an indirect island");
    }
}

TEST(a_br_x18_that_is_no_recognized_jump_table_is_an_indirect_island) {
    const SyntheticJumpTable found;
    const auto findings = found.scan();
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.jump_tables.size() == 1);
    if (findings.jump_tables.size() == 1) {
        CHECK(findings.jump_tables[0].targets == (std::vector<std::uint64_t>{0x1001c, 0x10020}));
    }
    // The other bound and index forms compilers use: B.HS after CMP #n (n entries), a UXTW index.
    auto unsigned_bound = found;
    unsigned_bound.code[0] = 0x7100'125f;  // cmp w18, #4
    unsigned_bound.code[1] = 0x5400'00e2;  // b.hs 0x10020
    unsigned_bound.code[4] = 0xb8b2'5932;  // ldrsw x18, [x9, w18, uxtw #2]
    const auto also_found = unsigned_bound.scan();
    CHECK(also_found.x18_unsupported.empty());
    CHECK(also_found.jump_tables.size() == 1);
    if (also_found.jump_tables.size() == 1) CHECK(also_found.jump_tables[0].entries == 4);

    // Each shape below is not a jump table the rewriter recognizes, so the br falls back to the
    // general indirect island instead of being refused (step C C2b).
    auto no_bounds_check = found;
    no_bounds_check.code[0] = 0xd503'201f;  // nop in place of the cmp
    check_indirect_island(no_bounds_check.scan());

    auto other_base = found;
    other_base.code[2] = 0xaa00'03e9;  // mov x9, x0 in place of the adrp
    check_indirect_island(other_base.scan());

    auto entered = found;
    entered.code[7] = 0x17ff'fffe;  // b 0x10014: into the jump sequence, past the bounds check
    check_indirect_island(entered.scan());

    auto outside = found;
    outside.table[2] = 0x30000 - 0x20040;  // beyond the code
    check_indirect_island(outside.scan());

    auto wide = found;  // a target 32 KiB past the first
    wide.code.resize(0x8000 / 4 + 16, 0xd503'201f);
    wide.table[1] = 0x1001c + 0x8000 - 0x20040;
    check_indirect_island(wide.scan());
    // One word less is the widest span the landing pads cover; the island table can still give
    // the length of such an island in 16 bits (ADR-0020 X4).
    auto widest = wide;
    widest.table[1] = 0x1001c + 0x8000 - 4 - 0x20040;
    const auto widest_found = widest.scan();
    CHECK(widest_found.jump_tables.size() == 1);
    if (widest_found.jump_tables.size() == 1) {
        CHECK(apk::rewrite::jump_table_layout(widest_found.jump_tables[0]).words <= 0xffff);
    }

    // A recognized jump table whose target enters a relocated exclusive window stays refused: the
    // indirect island branches to the runtime x18 without checking it against the window (step C C2b).
    auto into_window = found;  // an LL/SC loop on x18 after the cases, and a target inside it
    into_window.code.insert(into_window.code.end(), {0xc85f'7db1, 0x8b0e'0232, 0xc810'fdb2, 0x35ff'ffb0});
    into_window.table[1] = 0x10028 - 0x20040;  // the add x18 behind the ldxr at 0x10024
    check_refused(into_window.scan(), "exclusive window");

    auto equal = found;
    equal.code[1] = 0x5400'00e0;  // b.eq: no bound at all
    check_indirect_island(equal.scan());

    // A jump-table-shaped br whose table is not served as read-only data stays refused, not degraded:
    // the table may lie in executable memory, where the x18 pass could rewrite an entry (step C C2b).
    apk::rewrite::Findings without_data;  // no read-only data at all
    apk::rewrite::scan(std::as_bytes(std::span(found.code)), 0x10000, without_data);
    check_refused(without_data, "read-only data");
}

/// Scans `code` at 0x10000 with `table` served as read-only data at 0x20040 (as clang lays it out).
apk::rewrite::Findings scan_with_table(std::span<const std::uint32_t> code, std::span<const std::int32_t> table) {
    apk::rewrite::Findings findings;
    apk::rewrite::scan(std::as_bytes(code), 0x10000, findings,
                       [table](std::uint64_t address, std::size_t size) -> std::span<const std::byte> {
                           const auto bytes = std::as_bytes(table);
                           if (address < 0x20040 || address + size > 0x20040 + bytes.size()) return {};
                           return bytes.subspan(address - 0x20040, size);
                       });
    return findings;
}

/// Each recognition rule the ADR states, one code shape per rule, so a rule that stops working shows
/// up here rather than in a converted binary (ADR-0020 X2d). The table base is always adrp x9 + add
/// #0x40 to 0x20040, and its entries are 32-bit offsets from there.
TEST(each_jump_table_recognition_rule_is_covered) {
    // A flag-setting instruction between the CMP and the branch breaks the bounds check.
    const std::uint32_t flags_between[] = {
        0x7100'0e5f,  // 0x10000 cmp w18, #3
        0x7100'013f,  // 0x10004 cmp w9, #0: sets the flags the branch reads
        0x5400'00e8,  // 0x10008 b.hi 0x10024
        0x9000'0089,  // 0x1000c adrp x9, 0x20000
        0x9101'0129,  // 0x10010 add x9, x9, #0x40
        0xb8b2'7932,  // 0x10014 ldrsw x18, [x9, x18, lsl #2]
        0x8b09'0252,  // 0x10018 add x18, x18, x9
        0xd61f'0240,  // 0x1001c br x18
        0xd503'201f,  // 0x10020 nop
        0xd65f'03c0,  // 0x10024 ret
    };
    const std::int32_t four[] = {0x10020 - 0x20040, 0x10024 - 0x20040, 0x10020 - 0x20040, 0x10024 - 0x20040};
    auto findings = scan_with_table(flags_between, four);
    check_indirect_island(findings, 0x1001c);  // not a recognized jump table: the general indirect island

    // An index write between the branch and the load makes the checked value stale.
    const std::uint32_t index_written[] = {
        0x7100'0e5f,  // cmp w18, #3
        0x5400'0108,  // b.hi 0x10024
        0x2a00'03f2,  // mov w18, w0: writes the index after the check
        0x9000'0089,  // adrp x9, 0x20000
        0x9101'0129,  // add x9, x9, #0x40
        0xb8b2'7932,  // ldrsw x18, [x9, x18, lsl #2]
        0x8b09'0252,  // add x18, x18, x9
        0xd61f'0240,  // 0x1001c br x18
        0xd503'201f, 0xd65f'03c0,
    };
    findings = scan_with_table(index_written, four);
    check_indirect_island(findings, 0x1001c);

    // CMP #0 with B.HS reaches zero entries: no table, and no read of an empty target list.
    const std::uint32_t zero_entries[] = {
        0x7100'025f,  // cmp w18, #0
        0x5400'0102,  // b.hs 0x10024
        0x9000'0089,  // adrp x9, 0x20000
        0x9101'0129,  // add x9, x9, #0x40
        0xb8b2'7932,  // ldrsw x18, [x9, x18, lsl #2]
        0x8b09'0252,  // add x18, x18, x9
        0xd61f'0240,  // 0x10018 br x18
        0xd503'201f, 0xd65f'03c0, 0xd503'201f,
    };
    findings = scan_with_table(zero_entries, four);
    check_indirect_island(findings, 0x10018);

    // A target on the first word of a relocated LL/SC window is fine: the window is entered at its
    // load-exclusive, which is where a jump into it must land anyway.
    const std::uint32_t into_window_start[] = {
        0x7100'0e5f,  // 0x10000 cmp w18, #3
        0x5400'0168,  // 0x10004 b.hi 0x10030
        0x9000'0089,  // 0x10008 adrp x9, 0x20000
        0x9101'0129,  // 0x1000c add x9, x9, #0x40
        0xb8b2'7932,  // 0x10010 ldrsw x18, [x9, x18, lsl #2]
        0x8b09'0252,  // 0x10014 add x18, x18, x9
        0xd61f'0240,  // 0x10018 br x18
        0xd503'201f,  // 0x1001c nop: one target
        0xc85f'7db1,  // 0x10020 ldxr x17, [x13]: the window start, the other target
        0x8b0e'0232,  // 0x10024 add x18, x17, x14
        0xc810'fdb2,  // 0x10028 stlxr w16, x18, [x13]
        0x35ff'ffb0,  // 0x1002c cbnz w16, 0x10020
        0xd65f'03c0,  // 0x10030 ret
    };
    const std::int32_t window_targets[] = {0x1001c - 0x20040, 0x10020 - 0x20040, 0x1001c - 0x20040, 0x10020 - 0x20040};
    findings = scan_with_table(into_window_start, window_targets);
    CHECK(findings.x18_unsupported.empty());
    CHECK(findings.jump_tables.size() == 1);
    CHECK(findings.exclusive_regions.size() == 1);
    if (findings.jump_tables.size() == 1) {
        CHECK(findings.jump_tables[0].targets == (std::vector<std::uint64_t>{0x1001c, 0x10020}));
    }

    // The bounds branch itself must leave the sequence; one that lands back inside it is refused,
    // because then the entry count would not describe what the index can reach.
    const std::uint32_t branch_inside[] = {
        0x7100'0e5f,  // 0x10000 cmp w18, #3
        0x5400'0068,  // 0x10004 b.hi 0x10010: back into the sequence, not out to a default case
        0x9000'0089,  // 0x10008 adrp x9, 0x20000
        0x9101'0129,  // 0x1000c add x9, x9, #0x40
        0xb8b2'7932,  // 0x10010 ldrsw x18, [x9, x18, lsl #2]
        0x8b09'0252,  // 0x10014 add x18, x18, x9
        0xd61f'0240,  // 0x10018 br x18
        0xd503'201f,  // 0x1001c nop: a target
        0xd65f'03c0,  // 0x10020 ret: a target
    };
    // Targets in range, so the only thing that refuses this shape is the branch back into it.
    const std::int32_t inside_targets[] = {0x1001c - 0x20040, 0x10020 - 0x20040, 0x1001c - 0x20040, 0x10020 - 0x20040};
    findings = scan_with_table(branch_inside, inside_targets);
    check_indirect_island(findings, 0x10018);
}

// --- The island table (ADR-0020 X4) --------------------------------------------------------------

/// Checks what every island form's layout says about the words its emitter wrote: the length, word
/// 0 saving `value` and word 1 saving `base` in the red zone, and the store word writing `value` to
/// TSD slot 11. `instruction` names the island in a failure message.
void check_common_layout(const apk::rewrite::IslandLayout &layout, const std::vector<std::byte> &stub,
                         std::uint32_t instruction, std::source_location location = std::source_location::current()) {
    const auto failed = [&](const char *message) {
        apk::testing::fail(location, message, std::format(" (the island of {:#010x})", instruction).c_str());
    };
    const auto stur = [](std::uint32_t rt, std::int32_t offset) {
        return 0xf800'0000u | ((static_cast<std::uint32_t>(offset) & 0x1ff) << 12) | (31u << 5) | rt;
    };
    if (4 * std::size_t{layout.words} != stub.size()) {
        failed("the length differs");
        return;
    }
    if (word(stub, 0) != stur(layout.value, -128)) failed("word 0 does not save the value register");
    if (layout.base != APK_ISLAND_NO_REGISTER && word(stub, 1) != stur(layout.base, -120)) {
        failed("word 1 does not save the base register");
    }
    const auto store_x18 = 0xf900'2c00u | (std::uint32_t{layout.base} << 5) | layout.value;  // str value, [base, #88]
    if (layout.store != 0 && (layout.store >= layout.words || word(stub, layout.store) != store_x18)) {
        failed("the store word does not store the value register to slot 11");
    }
    if (layout.commit != APK_ISLAND_NO_COMMIT && layout.commit >= layout.words) failed("the commit word lies outside");
}

// What the island table records about an island must match the words its emitter wrote: for a single
// instruction the commit word completes it (the instruction itself, or the last word of a PC-relative
// one), and the last word goes on `resume` words past the site; for a resumable window commit is the
// preamble length (ADR-0020 X4h). A conditional branch and a jump table always go back to the site.
TEST(island_layouts_match_the_emitted_islands) {
    constexpr std::uint64_t site_address = 0x10000, stub_address = 0x20000;
    const auto writes_value = [](std::uint32_t word, unsigned value) {
        return ((apk::a64::register_use(word).writes >> value) & 1) != 0;
    };
    // Single instructions: those of the reference code, the five PC-relative forms and both kinds
    // of conditional branch.
    std::vector<std::uint32_t> instructions;
    for (const auto &[begin, end] : {std::pair{ref_x18, ref_x18_end}, std::pair{ref_x18_islands, ref_x18_moves_sp}}) {
        for (const auto &site : scan_words(begin, end, site_address).sites) {
            if (site.kind == SiteKind::x18) instructions.push_back(site.instruction);
        }
    }
    for (const auto *form : {ref_x18_site_adrp, ref_x18_site_adr, ref_x18_site_ldr, ref_x18_site_ldr_w, ref_x18_site_ldrsw}) {
        instructions.push_back(form[0]);
    }
    instructions.push_back(0xb400'0212);  // cbz x18, .+0x40
    instructions.push_back(0xb708'0212);  // tbnz x18, #33, .+0x40
    instructions.push_back(0xd61f'0240);  // br x18:  indirect island
    instructions.push_back(0xd63f'0240);  // blr x18: indirect island
    for (const auto instruction : instructions) {
        const Site site{site_address, SiteKind::x18, 18, instruction};
        const auto [patch, stub] = emit_at(site, site_address, stub_address);
        const auto layout = apk::rewrite::island_layout(site);
        check_common_layout(layout, stub, instruction);
        if (4 * std::size_t{layout.words} != stub.size()) continue;
        if (instruction == 0xd61f'0240u || instruction == 0xd63f'0240u) {  // br x18, blr x18: indirect
            CHECK(layout.kind == APK_ISLAND_INDIRECT);
            CHECK(layout.commit == APK_ISLAND_NO_COMMIT && layout.store == 0 && layout.resume == 0);
            CHECK(layout.value == 16);                                 // x16 holds the guest x18
            CHECK((layout.base == 30) == (instruction == 0xd63f'0240u));  // only blr saves x30
            CHECK(word(stub, layout.words - 1) == 0xd61f'0200u);       // last word is br x16
            continue;
        }
        if (((instruction >> 25) & 0x3e) == 0x1a) {  // CBZ, CBNZ, TBZ, TBNZ
            CHECK(layout.kind == APK_ISLAND_BRANCH);
            CHECK(layout.commit == APK_ISLAND_NO_COMMIT && layout.base == APK_ISLAND_NO_REGISTER && layout.store == 0);
            continue;
        }
        const bool writes = writes_value(instruction, 18);
        CHECK(layout.kind == APK_ISLAND_SINGLE);
        CHECK(layout.resume == 1 && branch_target_at(stub, layout.words - 1, stub_address) == site_address + 4);
        CHECK((layout.store != 0) == writes && (layout.base != APK_ISLAND_NO_REGISTER) == writes);
        CHECK(layout.commit < layout.words - 1);  // before the branch back
        if (layout.commit >= layout.words - 1) continue;
        const auto commit = word(stub, layout.commit);
        // ADR, ADRP and the loads from a literal become words that compute the value; the last of them commits.
        if ((instruction & 0x1f00'0000) == 0x1000'0000 || (instruction & 0x3b00'0000) == 0x1800'0000) {
            CHECK(writes_value(commit, layout.value) && !writes_value(word(stub, layout.commit + 1u), layout.value));
        } else {
            CHECK(commit == apk::a64::substitute(instruction, 18, layout.value));
        }
    }

    // LL/SC windows: x18 written by an add, x18 as the address with the flags read, a write of x18 a
    // branch can skip, only the store-exclusive writing its status, a CLREX closing the window
    // without a store-exclusive, and the cmpxchg shape with a CLREX behind its store-exclusive.
    std::vector<apk::rewrite::ExclusiveRegion> windows;
    for (const auto &[begin, end] : {std::pair{ref_x18_ll_sc, ref_x18_ll_sc_end},
                                     std::pair{ref_x18_window_flags_read, ref_x18_window_end},
                                     std::pair{ref_x18_window_clrex_only, ref_x18_window_clrex_only_end}}) {
        for (auto &region : scan_words(begin, end, site_address).exclusive_regions) windows.push_back(std::move(region));
    }
    const std::uint32_t cmpxchg[] = {0xaa12'03f1, 0xc85f'fdb2, 0xeb11'025f, 0x5400'0041,
                                     0xc803'fda2, 0xd503'3f5f, 0xb5ff'ff43};
    for (auto &region : scan_words(cmpxchg, cmpxchg + std::size(cmpxchg), site_address).exclusive_regions) {
        windows.push_back(std::move(region));
    }
    CHECK(windows.size() == 6);
    for (const auto &region : windows) {
        std::vector<std::byte> stub(apk::rewrite::exclusive_region_stub_size(region));
        apk::rewrite::emit_exclusive_region(region, region.address, stub_address, stub);
        const auto layout = apk::rewrite::exclusive_region_layout(region);
        check_common_layout(layout, stub, region.words[0]);
        if (4 * std::size_t{layout.words} != stub.size() || layout.commit >= layout.words) continue;
        CHECK(layout.kind == APK_ISLAND_EXCLUSIVE);
        CHECK(layout.resume == region.words.size());
        CHECK(branch_target_at(stub, layout.words - 1, stub_address) == region.address + 4 * layout.resume);
        CHECK((layout.store != 0) == region.writes_x18 && (layout.base != APK_ISLAND_NO_REGISTER) == region.writes_x18);
        // A relocated window is resumable (ADR-0020 X4h): commit is the preamble length, so the island
        // body starts there with the window's load-exclusive.
        const auto preamble = 1u + (region.writes_x18 ? 1u : 0u) + (region.loads_x18 ? 3u : 0u);
        CHECK(layout.commit == preamble);
        CHECK((word(stub, layout.commit) & 0x3f40'0000) == 0x0840'0000);  // a load-exclusive
    }

    // The jump table: both registers saved, nothing stored.
    apk::rewrite::Findings jumps;
    apk::rewrite::scan(std::span(reinterpret_cast<const std::byte *>(ref_x18_jump_function),
                                 4 * static_cast<std::size_t>(ref_x18_jump_function_end - ref_x18_jump_function)),
                       address_of(ref_x18_jump_function), jumps, reference_table);
    CHECK(jumps.jump_tables.size() == 1);
    for (const auto &table : jumps.jump_tables) {
        std::vector<std::byte> stub(apk::rewrite::jump_table_stub_size(table));
        apk::rewrite::emit_jump_table(table, table.address, address_of(ref_x18_island_jump), stub);
        const auto layout = apk::rewrite::jump_table_layout(table);
        check_common_layout(layout, stub, 0xd61f'0240);
        CHECK(layout.kind == APK_ISLAND_JUMP_TABLE && layout.base != APK_ISLAND_NO_REGISTER);
        CHECK(layout.commit == APK_ISLAND_NO_COMMIT && layout.store == 0);
    }
}

TEST(stubs_out_of_branch_range_are_rejected) {
    const Site site{0, SiteKind::syscall, 0};
    std::vector<std::byte> stub(apk::rewrite::stub_size(site));
    constexpr std::uint64_t far = 129ull << 20;  // b reaches -128 MiB exactly, so go one further
    CHECK_THROWS(apk::rewrite::RewriteError, apk::rewrite::emit(site, 0x1000 + far, 0x1000, 0x10'0000, stub),
                 "128 MiB");
    CHECK_THROWS(apk::rewrite::RewriteError,
                 apk::rewrite::emit(site, 0x2000, 0x1000, 0x1000 + (5ull << 30), stub), "4 GiB");
    CHECK_THROWS(apk::rewrite::RewriteError, apk::rewrite::emit(site, 0x2000, 0x1000, 0x10'0004, stub),
                 "8-byte");
}

// with_any_branch_displacement throws instead of truncating a window branch whose retargeted target
// the island's field cannot reach, or whose displacement is not a multiple of 4 (step C C1b). A
// hand-built region whose window holds a branch leaving it, emitted far from the guest window, makes
// the island-relative displacement overflow the field.
TEST(a_window_branch_out_of_field_range_is_rejected_not_truncated) {
    const auto emit_at = [](std::uint32_t branch, std::uint64_t stub_address) {
        apk::rewrite::ExclusiveRegion region;
        region.address = 0x100000;
        region.words = {0xc85f'7c20, branch, 0xc801'7c20};  // ldxr x0, [x1]; <branch leaving>; stxr w1, x0, [x1]
        region.scratch = 3;
        std::vector<std::byte> stub(apk::rewrite::exclusive_region_stub_size(region));
        apk::rewrite::emit_exclusive_region(region, region.address, stub_address, stub);
    };
    // The branch at .+16 lands past the three-word window, so emit keeps its absolute target. The
    // B.cond's stub sits 2 MiB away, beyond its +-1 MiB imm19; the CMPBR's only 4 KiB away, within
    // imm19 but beyond its own +-1 KiB imm9, so a wrong (wider) field would not catch it.
    CHECK_THROWS(apk::rewrite::RewriteError, emit_at(0x5400'0080, 0x100000 + (2u << 20)), "range");  // b.eq .+16
    CHECK_THROWS(apk::rewrite::RewriteError, emit_at(0xf4c6'0085, 0x100000 + 0x1000), "range");  // cmpbr .+16
    // A stub two bytes off makes the island-relative displacement odd: not a multiple of 4.
    CHECK_THROWS(apk::rewrite::RewriteError, emit_at(0x5400'0080, 0x100000 + (2u << 20) + 2), "multiple of 4");
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
