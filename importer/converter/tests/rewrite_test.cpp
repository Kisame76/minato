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
extern const std::uint32_t ref_x18_window_flags[];
extern const std::uint32_t ref_x18_window_memory[];
extern const std::uint32_t ref_x18_window_syscall[];
extern const std::uint32_t ref_x18_window_simd[];
extern const std::uint32_t ref_x18_window_exit_next[];
extern const std::uint32_t ref_x18_window_base[];
extern const std::uint32_t ref_x18_window_call[];
extern const std::uint32_t ref_x18_window_flags_read[];
extern const std::uint32_t ref_x18_window_skipped_write[];
extern const std::uint32_t ref_x18_window_status[];
extern const std::uint32_t ref_x18_window_end[];
extern const std::uint32_t ref_x18_window_clrex_only[];
extern const std::uint32_t ref_x18_window_clrex_only_end[];
extern const std::uint32_t ref_x18_island_cbz[];
extern const std::uint32_t ref_x18_island_tbz[];
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

/// Scans the words from `begin` to `end` as if they sat at `address`.
apk::rewrite::Findings scan_words(const std::uint32_t *begin, const std::uint32_t *end, std::uint64_t address) {
    const std::span code(reinterpret_cast<const std::byte *>(begin), 4 * static_cast<std::size_t>(end - begin));
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, address, findings);
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

TEST(scan_puts_x18_instructions_into_islands_and_names_the_forms_it_cannot) {
    const std::uint32_t code[] = {
        0x8b01'0252,  // add x18, x18, x1: island
        0xb400'0052,  // cbz x18, #+8: a two-way island (X2)
        0x9000'0012,  // adrp x18, #0: recomputed in the island (X2)
        0xd61f'0240,  // br x18: not yet
        0x9100'025f,  // mov sp, x18: moves sp, which the island needs unmoved
        0xf81f'0ff2,  // str x18, [sp, #-16]!: writes below sp and moves it
        0xc85f'7c12,  // ldxr x18, [x0]: exclusive access, not yet
        0x9100'0421,  // add x1, x1, #1: no x18
    };
    const std::span bytes(reinterpret_cast<const std::byte *>(code), sizeof code);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(bytes, 0x1000, findings);
    const std::vector<Site> expected{{0x1000, SiteKind::x18, 18, 0x8b01'0252},
                                     {0x1004, SiteKind::x18, 18, 0xb400'0052},
                                     {0x1008, SiteKind::x18, 18, 0x9000'0012}};
    CHECK(findings.sites == expected);
    CHECK(findings.x18_unsupported.size() == 4);
    const std::uint64_t first_unsupported[] = {0x100c, 0x1010, 0x1014, 0x1018};
    for (std::size_t index = 0; index < findings.x18_unsupported.size() && index < 4; ++index) {
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

TEST(a_conditional_branch_on_x18_is_an_island_but_a_register_branch_is_refused) {
    const std::uint32_t code[] = {
        0xb400'0212,  // cbz x18, .+8: island
        0xb500'0212,  // cbnz x18: island
        0x3618'0212,  // tbz w18, #3: island
        0xb708'0212,  // tbnz x18, #33: island
        0xd61f'0240,  // br x18: refused, needs a landing pad
        0xd63f'0240,  // blr x18: refused
    };
    const auto findings = scan_words(code, code + std::size(code), 0x1000);
    std::vector<std::uint64_t> islands;
    for (const auto &s : findings.sites) islands.push_back(s.address);
    CHECK(islands == (std::vector<std::uint64_t>{0x1000, 0x1004, 0x1008, 0x100c}));
    CHECK(findings.x18_unsupported.size() == 2);
    for (const auto &entry : findings.x18_unsupported) {
        CHECK(std::string_view(entry.reason).find("register branch") != std::string_view::npos);
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
    CHECK(!region.reads_x18);  // x18 is written by the add before the stlxr reads it
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
    CHECK(!region.reads_x18);         // the ldaxr writes x18 first
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
    CHECK(region.reads_x18);
    CHECK(!region.writes_x18);
}

// ADR-0020 X4: a signal inside a relocated window sends the thread back to its load-exclusive, so
// only windows that run the same when they run again from there are relocated. The x18 uses of any
// other window are refused, with the reason.
TEST(an_exclusive_window_that_cannot_run_again_is_refused) {
    constexpr std::string_view other =
        "it holds an instruction other than data processing on general-purpose registers, a branch within "
        "the window (not a call), a barrier or the exclusive pair";
    struct Case {
        const std::uint32_t *begin, *end;
        std::string_view why;
    };
    const Case cases[] = {
        {ref_x18_window_exit, ref_x18_window_input, "a branch leaves it"},
        {ref_x18_window_input, ref_x18_window_flags, "it writes a register it reads first"},
        {ref_x18_window_flags, ref_x18_window_memory, "it reads the flags before it sets them"},
        {ref_x18_window_memory, ref_x18_window_syscall, other},
        {ref_x18_window_syscall, ref_x18_window_simd, other},
        {ref_x18_window_simd, ref_x18_window_exit_next, other},
        {ref_x18_window_exit_next, ref_x18_window_base, "a branch leaves it"},
        {ref_x18_window_base, ref_x18_window_call, "it writes a register it reads first"},
        {ref_x18_window_call, ref_x18_window_flags_read, other},
    };
    for (const auto &c : cases) {
        const auto findings = scan_words(c.begin, c.end, 0x10000);
        CHECK(findings.exclusive_regions.empty());
        CHECK(findings.x18_unsupported.size() == 2);  // the load- and the store-exclusive on x18
        for (const auto &use : findings.x18_unsupported) {
            const std::string_view reason(use.reason);
            if (!reason.starts_with("it lies in an exclusive window that cannot run again from its start: ") ||
                !reason.ends_with(c.why)) {
                apk::testing::fail(std::source_location::current(), "unexpected reason: ", use.reason);
            }
        }
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
    CHECK(skipped.exclusive_regions[0].reads_x18);
    CHECK(skipped.exclusive_regions[0].writes_x18);
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
    CHECK(!findings.x18_unsupported.empty());
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
    // x18 is written, so the island saves both scratch registers in the red zone and stores back.
    const auto words = stub.size() / 4;
    CHECK(words == 1 /* save value */ + 1 /* save base */ + 3 /* window */ + 3 /* mrs, bic, str */ +
                       2 /* restore value, base */ + 1 /* b */);
    for (std::size_t index = 0; index < words; ++index) {
        const auto use = apk::a64::register_use(word(stub, index));
        CHECK(((use.reads | use.writes) & (1u << 18)) == 0);  // no word still names x18
    }
    CHECK(word(stub, 0) == 0xf818'03e0);   // stur x0, [sp, #-128]
    CHECK(word(stub, 1) == 0xf818'83e1);   // stur x1, [sp, #-120]
    // The window keeps its shape, x18 renamed to x0: ldxr (no x18, unchanged), add, stlxr.
    CHECK(word(stub, 2) == 0xc85f'7db1);   // ldxr x17, [x13]
    CHECK(word(stub, 3) == 0x8b0e'0220);   // add x0, x17, x14
    CHECK(word(stub, 4) == 0xc810'fda0);   // stlxr w16, x0, [x13]
    CHECK(word(stub, 5) == 0xd53b'd061);   // mrs x1, TPIDRRO_EL0 (the TSD base)
    CHECK(word(stub, 7) == 0xf900'2c20);   // str x0, [x1, #88]
    CHECK(branch_target_at(stub, 10, stub_address) == address_of(ref_site) + 4 * 3);  // after the window
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

TEST(a_br_x18_stays_refused_where_no_jump_table_it_can_read_ends_in_it) {
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

    auto no_bounds_check = found;
    no_bounds_check.code[0] = 0xd503'201f;  // nop in place of the cmp
    check_refused(no_bounds_check.scan(), "bounds check");

    auto other_base = found;
    other_base.code[2] = 0xaa00'03e9;  // mov x9, x0 in place of the adrp
    check_refused(other_base.scan(), "table base");

    auto entered = found;
    entered.code[7] = 0x17ff'fffe;  // b 0x10014: into the jump sequence, past the bounds check
    check_refused(entered.scan(), "another branch");

    auto outside = found;
    outside.table[2] = 0x30000 - 0x20040;  // beyond the code
    check_refused(outside.scan(), "outside");

    auto wide = found;  // a target 32 KiB past the first
    wide.code.resize(0x8000 / 4 + 16, 0xd503'201f);
    wide.table[1] = 0x1001c + 0x8000 - 0x20040;
    check_refused(wide.scan(), "32 KiB");
    // One word less is the widest span the landing pads cover; the island table can still give
    // the length of such an island in 16 bits (ADR-0020 X4).
    auto widest = wide;
    widest.table[1] = 0x1001c + 0x8000 - 4 - 0x20040;
    const auto widest_found = widest.scan();
    CHECK(widest_found.jump_tables.size() == 1);
    if (widest_found.jump_tables.size() == 1) {
        CHECK(apk::rewrite::jump_table_layout(widest_found.jump_tables[0]).words <= 0xffff);
    }

    auto into_window = found;  // an LL/SC loop on x18 after the cases, and a target inside it
    into_window.code.insert(into_window.code.end(), {0xc85f'7db1, 0x8b0e'0232, 0xc810'fdb2, 0x35ff'ffb0});
    into_window.table[1] = 0x10028 - 0x20040;  // the add x18 behind the ldxr at 0x10024
    check_refused(into_window.scan(), "exclusive window");

    auto equal = found;
    equal.code[1] = 0x5400'00e0;  // b.eq: no bound at all
    check_refused(equal.scan(), "bounds check");

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
    CHECK(findings.jump_tables.empty());
    CHECK(std::ranges::any_of(findings.x18_unsupported, [](const auto &u) { return u.address == 0x1001c; }));

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
    CHECK(findings.jump_tables.empty());
    CHECK(std::ranges::any_of(findings.x18_unsupported, [](const auto &u) { return u.address == 0x1001c; }));

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
    CHECK(findings.jump_tables.empty());
    CHECK(std::ranges::any_of(findings.x18_unsupported, [](const auto &u) { return u.address == 0x10018; }));

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
    CHECK(findings.jump_tables.empty());
    CHECK(std::ranges::any_of(findings.x18_unsupported, [](const auto &u) { return u.address == 0x10018; }));
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

// What the island table records about an island must match the words its emitter wrote: the commit
// word completes the guest instruction (the instruction itself, the last word of a PC-relative one,
// the store-exclusive of a window or the CLREX that closes one without it), and the last word goes
// on `resume` words past the site. A conditional branch and a jump table always go back to the site.
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
    for (const auto instruction : instructions) {
        const Site site{site_address, SiteKind::x18, 18, instruction};
        const auto [patch, stub] = emit_at(site, site_address, stub_address);
        const auto layout = apk::rewrite::island_layout(site);
        check_common_layout(layout, stub, instruction);
        if (4 * std::size_t{layout.words} != stub.size()) continue;
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
    const auto is_store_exclusive = [](std::uint32_t word) {
        return (word & 0x3f80'0000) == 0x0800'0000 && ((word >> 22) & 1) == 0;
    };
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
        const auto commit = word(stub, layout.commit);
        if (std::ranges::any_of(region.words, is_store_exclusive)) {
            CHECK(is_store_exclusive(commit));
        } else {
            CHECK((commit & 0xffff'f0ff) == 0xd503'305f);  // CLREX
        }
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

}  // namespace

int main() {
    return apk::testing::run_all();
}
