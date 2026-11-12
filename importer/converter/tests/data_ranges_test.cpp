// Data inside executable sections (apkconv/data_ranges.h): what counts as proof that words are data, what keeps them
// code, on hand-assembled words with the roots given by the test. Each layout known to make an earlier version of the
// split miss a real svc silently is a case here, with the words that showed it.
#include "apkconv/data_ranges.h"

#include "apkconv/container.h"

#include "check.h"
#include "elf_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using apk::data::Code;
using apk::data::DataRanges;

constexpr std::uint32_t ret = 0xd65f'03c0;
constexpr std::uint32_t nop = 0xd503'201f;
constexpr std::uint32_t mov_x8_172 = 0xd280'1588;
constexpr std::uint32_t svc0 = 0xd400'0001;
constexpr std::uint32_t svc_data = 0xd402'4681;  // svc #0x1234, as a word of a pool

constexpr std::uint32_t ldr_literal(unsigned rt, int bytes, bool x = false) {  // ldr wRt/xRt, <pc + bytes>
    return (x ? 0x5800'0000u : 0x1800'0000u) | ((static_cast<std::uint32_t>(bytes / 4) & 0x7ffff) << 5) | rt;
}
constexpr std::uint32_t adr(unsigned rd, int bytes) {
    const auto imm = static_cast<std::uint32_t>(bytes) & 0x1fffff;
    return 0x1000'0000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd;
}
constexpr std::uint32_t adrp(unsigned rd, int pages) {
    const auto imm = static_cast<std::uint32_t>(pages) & 0x1fffff;
    return 0x9000'0000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd;
}
constexpr std::uint32_t add_imm(unsigned rd, unsigned rn, unsigned imm) {
    return 0x9100'0000u | (imm << 10) | (rn << 5) | rd;
}
constexpr std::uint32_t cmp_w_imm(unsigned rn, unsigned imm) { return 0x7100'001fu | (imm << 10) | (rn << 5); }
constexpr std::uint32_t b_cond(int bytes, unsigned cond) {
    return 0x5400'0000u | ((static_cast<std::uint32_t>(bytes / 4) & 0x7ffff) << 5) | cond;
}
constexpr std::uint32_t b_hi(int bytes) { return b_cond(bytes, 8); }
constexpr std::uint32_t cbz_x0(int bytes) {
    return 0xb400'0000u | ((static_cast<std::uint32_t>(bytes / 4) & 0x7ffff) << 5);
}
constexpr std::uint32_t tbz_x0(int bytes) {
    return 0xb600'0000u | ((static_cast<std::uint32_t>(bytes / 4) & 0x3fff) << 5);
}
constexpr std::uint32_t bl(int bytes) { return 0x9400'0000u | (static_cast<std::uint32_t>(bytes / 4) & 0x03ff'ffff); }
constexpr std::uint32_t br(unsigned rn) { return 0xd61f'0000u | (rn << 5); }
constexpr std::uint32_t blr(unsigned rn) { return 0xd63f'0000u | (rn << 5); }
constexpr std::uint32_t ldrsw_indexed = 0xb8a0'5822;   // ldrsw x2, [x1, w0, uxtw #2]
constexpr std::uint32_t ldrb_indexed = 0x3860'4822;    // ldrb w2, [x1, w0, uxtw]
constexpr std::uint32_t add_x1_x1_x2 = 0x8b02'0021;    // add x1, x1, x2
constexpr std::uint32_t add_x3_x3_x2_lsl2 = 0x8b02'0863;  // add x3, x3, x2, lsl #2
constexpr std::uint32_t ldr_w0_x8 = 0xb940'0100;       // ldr w0, [x8]
constexpr std::uint32_t swpal_x10_x8_x9 = 0xf8ea'8128;  // swpal x10, x8, [x9]
constexpr std::uint32_t ldr_x11_x8_16 = 0xf940'090b;   // ldr x11, [x8, #16]
constexpr std::uint32_t ldur_w9_x1_m4 = 0xb85f'c029;   // ldur w9, [x1, #-4]

struct Words {
    std::vector<std::uint32_t> words;
    Code code(std::uint64_t address = 0) const {
        return {{reinterpret_cast<const std::byte *>(words.data()), words.size() * 4}, address};
    }
};

using apk::data::Roots;

DataRanges run(const Words &w, std::vector<std::uint64_t> strong, std::vector<std::uint64_t> weak = {},
               std::vector<std::uint64_t> no_return = {}, std::vector<apk::data::Range> functions = {},
               std::vector<std::uint64_t> probable = {}) {
    const Code code = w.code();
    return apk::data::find_ranges({&code, 1}, Roots{std::move(strong), std::move(weak), std::move(no_return),
                                                     std::move(functions), std::move(probable)});
}

bool in_ranges(const DataRanges &data, std::uint64_t address) {
    for (const auto &range : data.ranges) {
        if (address >= range.begin && address < range.end) return true;
    }
    return false;
}

TEST(a_literal_pool_loaded_from_proven_code_is_data_by_its_exact_size_and_not_after_it) {
    // 0: ldr w0, <8>; 4: ret; 8: pool word; 12: a word nothing loads (code, as far as anyone knows).
    const Words w{{ldr_literal(0, 8), ret, svc_data, svc_data, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.size() == 1);
    CHECK(data.ranges[0].begin == 8 && data.ranges[0].end == 12);
    CHECK(data.literal_words == 1 && data.table_words == 0);
}

TEST(a_pool_nothing_loads_stays_code) {
    const Words w{{ret, svc_data, svc_data, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.empty());
}

TEST(a_data_word_that_looks_like_a_load_is_no_loader_and_the_function_after_it_stays_code) {
    // Probe A: ldr x0, pool; ret; pool: .quad 0x5800006012345678 (its high word is `ldr x0, <+12>`),
    // then a function with its own root: mov x8, #172; nop; svc #0; ret.
    const Words w{{ldr_literal(0, 8, true), ret, 0x1234'5678, 0x5800'0060, mov_x8_172, nop, svc0, ret}};
    const auto data = run(w, {0, 16});
    CHECK(data.ranges.size() == 1);
    CHECK(data.ranges[0].begin == 8 && data.ranges[0].end == 16);
    CHECK(!in_ranges(data, 24));  // the svc
    CHECK(data.reached_words == 6);
}

TEST(the_descent_stops_after_an_unconditional_branch) {
    // 0: b <12>; 4: a word that decodes as `ldr x0, <+12>` (data, never run); 12: ret; 16: a pool word.
    const Words w{{0x1400'0003, 0x5800'0060, svc_data, ret, svc_data}};
    const auto data = run(w, {0});
    CHECK(data.ranges.empty() && data.reached_words == 2);
}

TEST(a_literal_at_a_root_is_code) {
    const Words w{{ldr_literal(0, 8), ret, svc_data, ret}};
    const auto data = run(w, {0, 8});
    CHECK(data.ranges.empty() && data.dropped_reached == 1);  // a root is reached, so it is code
}

// A jump table as LLVM makes it: cmp w0, #2; b.hi default; adr x1, T; ldrsw x2, [x1, w0, uxtw #2]; add x1, x1, x2;
// br x1;
// T: three offsets from T to the cases, which come behind it: mov x8, #172; svc #0; ret at 40, 48 and 56.
Words table_function(bool with_bound) {
    // 0 cmp, 4 b.hi 64, 8 adr x1 (T = 28), 12 ldrsw, 16 add, 20 br, 24 ret, 28/32/36 the offsets, 40 case 0 (44 its
    // svc),
    // 48 case 1, 56 case 2, 64 the default.
    return Words{{with_bound ? cmp_w_imm(0, 2) : nop, with_bound ? b_hi(60) : nop, adr(1, 20), ldrsw_indexed,
                  add_x1_x1_x2, br(1), ret, 12, 20, 28, mov_x8_172, svc0, ret, nop, ret, nop, nop, ret, ret}};
}

TEST(a_jump_table_with_a_proven_bound_is_data_and_its_cases_are_reached) {
    const auto w = table_function(true);
    const auto data = run(w, {0});
    CHECK(data.ranges.size() == 1);
    CHECK(data.ranges[0].begin == 28 && data.ranges[0].end == 40);
    CHECK(data.table_words == 3 && data.unbounded_tables == 0);
    CHECK(!in_ranges(data, 44));  // the case's svc is code
    CHECK(data.reached_words >= 12);
}

TEST(a_jump_table_without_a_bound_is_no_data_and_its_cases_stay_assumed_code) {
    const auto w = table_function(false);
    const auto data = run(w, {0});
    CHECK(data.ranges.empty());
    CHECK(data.unbounded_tables == 1);
}

TEST(a_table_whose_entry_leaves_the_section_is_no_jump_table_and_poisons_its_function) {
    auto w = table_function(true);
    w.words[8] = 0x1000'0000;  // an entry far outside: the dispatch is not proven
    const auto data = run(w, {0}, {}, {}, {{0, 76}});
    CHECK(data.ranges.empty() && data.dropped_dispatch >= 1);
    CHECK(data.reached_words < 12);  // none of the cases is reached through it
}

TEST(a_pool_read_by_a_bounded_index_is_data_and_one_read_by_an_unbounded_index_is_not) {
    // cmp w0,#3; b.hi 28; adr x1,<32>; ldr w0,[x1,w0,uxtw #2]; ret; 20: mov w0,#0; 24: ret; 28: ... pool at 32.
    const Words bounded{{cmp_w_imm(0, 3), b_hi(16), adr(1, 20), 0xb860'5820, ret, ret, nop, nop, svc_data, svc_data,
                         svc_data, svc_data, ret}};
    const auto a = run(bounded, {0});
    CHECK(a.ranges.size() == 1 && a.ranges[0].begin == 28 && a.ranges[0].end == 44);
    const Words unbounded{{nop, nop, adr(1, 20), 0xb860'5820, ret, ret, nop, nop, svc_data, svc_data, svc_data,
                           svc_data, ret}};
    const auto b = run(unbounded, {0});
    CHECK(b.ranges.empty() && b.unbounded_tables == 1);
}

TEST(a_byte_table_added_to_a_code_label_shifted_by_two) {
    // cmp w0,#1; b.hi 48; adr x1, T (28); ldrb w2,[x1,w0,uxtw]; adr x3, base (32); add x3,x3,x2,lsl #2; br x3; T: the
    // bytes
    // 0 and 3; 32: case 0 (mov, svc, ret); 44: case 1; 48: the default.
    const Words w{{cmp_w_imm(0, 1), b_hi(44), adr(1, 20), ldrb_indexed, adr(3, 16), add_x3_x3_x2_lsl2, br(3),
                   0x0000'0300, mov_x8_172, svc0, ret, ret, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.size() == 1);
    CHECK(data.ranges.size() == 1 && data.ranges[0].begin == 28 && data.ranges[0].end == 32);
    CHECK(!in_ranges(data, 36));
}

// A jump table is proven only for the exact form `table + (entry << shift)` that reaches the
// branch register untouched; any other form leaves the dispatch unresolved.
constexpr std::uint32_t mov_x3_x5 = 0xaa05'03e3;
constexpr std::uint32_t mov_x2_x5 = 0xaa05'03e2;
constexpr std::uint32_t add_x3_x2_x3_lsl2 = 0x8b03'0843;  // add x3, x2, x3, lsl #2: the entry is the unshifted operand

TEST(a_sum_with_the_entry_as_the_unshifted_operand_is_not_the_table_the_code_assumes) {
    // It computes entry + (base << 2), not base + (entry << 2): no target is proven, the br stays unresolved.
    const Words w{{cmp_w_imm(0, 1), b_hi(44), adr(1, 20), ldrb_indexed, adr(3, 16), add_x3_x2_x3_lsl2, br(3),
                   0x0000'0300, mov_x8_172, svc0, ret, ret, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.empty());
}

TEST(a_branch_register_written_after_the_sum_is_not_the_sum) {
    const Words w{{cmp_w_imm(0, 1), b_hi(48), adr(1, 24), ldrb_indexed, adr(3, 20), add_x3_x3_x2_lsl2, mov_x3_x5, br(3),
                   0x0000'0300, mov_x8_172, svc0, ret, ret, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.empty());
}

TEST(a_loaded_register_overwritten_before_the_sum_is_not_the_entry) {
    const Words w{{cmp_w_imm(0, 1), b_hi(48), adr(1, 24), ldrb_indexed, mov_x2_x5, adr(3, 16), add_x3_x3_x2_lsl2, br(3),
                   0x0000'0300, mov_x8_172, svc0, ret, ret, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.empty());
}

// A strong run ends where the function it belongs to ends. A function whose last word is a
// conditional branch has no fall-through; the pool that follows it must not be decoded as trusted code.
TEST(a_pool_behind_a_function_that_ends_in_a_conditional_branch_is_not_trusted_code) {
    // f = [0, 4): b.hi 8. Word 4 is a pool word whose bits are `ldr w1, <12>`; 8: ret; 12: a word a real load
    // would prove.
    const Words w{{b_hi(8), ldr_literal(1, 8), ret, svc_data}};
    const auto data = run(w, {0}, {}, {}, {{0, 4}});
    CHECK(data.ranges.empty());
}

// The packed decoder reads as bionic's does (a group without an addend has the addend 0), and a
// table that is cut short is incomplete, as the unwind table is: the split gives up.
TEST(a_group_without_the_has_addend_flag_has_the_addend_zero) {
    // "APS2", 2 relocations, offset 0; group 1: 1 relocation, by info and delta and addend, delta 8, info 1027,
    // addend 0x100; group 2: 1 relocation, by info and delta, no addend: bionic's addend is 0 there.
    const unsigned char table[] = {'A', 'P', 'S', '2', 2, 0, 1, 0x0f, 8, 0x83, 0x08, 0x80, 0x02,
                                   1,   3, 8, 0x83, 0x08};
    const auto addends =
        apk::data::packed_relocation_addends({reinterpret_cast<const std::byte *>(table), sizeof table});
    CHECK(addends.size() == 2);
    CHECK(addends.size() == 2 && addends[0] == 0x100 && addends[1] == 0);
}

TEST(a_group_larger_than_the_count_that_is_left_gives_no_run) {
    // "APS2", 1 relocation promised, offset 0; one group of 3 by info, delta and addend: more than the table says.
    const unsigned char table[] = {'A', 'P', 'S', '2', 1, 0, 3, 0x0f, 8, 0x83, 0x08, 0x80, 0x02};
    const auto addends =
        apk::data::packed_relocation_addends({reinterpret_cast<const std::byte *>(table), sizeof table});
    CHECK(addends.empty());
}

TEST(a_table_that_is_cut_short_is_incomplete_and_gives_no_ranges) {
    using namespace apk::testing;
    // The same library twice: with a whole packed table the pool is data, with the table cut short nothing is proven.
    const auto library = [](std::size_t table_size, std::uint8_t count) {
        ElfBuilder builder;
        builder.bytes.resize(0x400);
        builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
        const unsigned char table[] = {'A', 'P', 'S', '2', count, 0, 1, 0x0f, 8, 0x83, 0x08, 0x80, 0x02};
        std::memcpy(builder.bytes.data() + 0x100, table, sizeof table);
        const std::uint32_t words[] = {ldr_literal(0, 8), ret, svc_data, ret};
        std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
        builder.sections({{".rela.dyn", 0x6000'0002, shf_alloc, 0x100, 0x100, table_size},
                          {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}},
                         {{"f", 0x200, 16}});
        return apk::container::scan_code(builder.bytes);
    };
    CHECK(library(13, 1).data.ranges.size() == 1);  // a whole table: the pool is proven data
    CHECK(library(13, 5).data.ranges.empty());      // it promises 5 relocations and holds 1
    CHECK(library(8, 1).data.ranges.empty());       // the group is cut off in its header
}

// Minor points.
TEST(a_br_after_a_call_is_an_unresolved_dispatch_too) {
    // f = [0, 24): a trusted load of the pool at 20, a call, then `br x5` that nothing resolves: it may land anywhere
    // in f, so the pool word it could land on is not proven data.
    const Words w{{ldr_literal(0, 20), bl(12), br(5), ret, ret, svc_data}};
    const auto data = run(w, {0}, {}, {}, {{0, 24}});
    CHECK(data.ranges.empty());
}

TEST(a_stray_data_value_does_not_cut_the_span_an_unresolved_br_may_reach) {
    // No unwind range: the fallback span runs between the roots around the br. A word of data whose value is no
    // instruction address (here 9: unaligned) is no root and must not end the span before the pool.
    const Words w{{ldr_literal(0, 24), br(5), ret, nop, nop, nop, svc_data, ret}};
    const auto data = run(w, {0}, {9});
    CHECK(data.ranges.empty());
}

TEST(an_atomic_overwriting_the_base_ends_what_the_base_stood_for) {
    // A probe: adr x8, P; ldr w0, [x8]; swpal x10, x8, [x9]; ldr x11, [x8, #16]: the last load is not
    // from P + 16.
    const Words w{{adr(8, 32), ldr_w0_x8, swpal_x10_x8_x9, ldr_x11_x8_16, ret, nop, nop, nop, svc_data, nop, nop, nop,
                   svc0, ret}};
    const auto data = run(w, {0, 48});
    CHECK(data.ranges.size() == 1 && data.ranges[0].begin == 32 && data.ranges[0].end == 36);
    CHECK(!in_ranges(data, 48));
}

TEST(an_add_that_copies_an_address_does_not_stop_the_original_from_being_taken) {
    // adr x9, stub; add x10, x9, #12; blr x9: the stub is called, so it is reached.
    const Words w{{adr(9, 16), add_imm(10, 9, 12), blr(9), ret, mov_x8_172, svc0, ret}};
    const auto data = run(w, {0});
    CHECK(data.reached_words == 7);
}

TEST(a_load_through_a_negative_unscaled_offset_reads_the_address_it_names) {
    // adr x1, code; ldur w9, [x1, #-4]; br x1: the word read is the br, which is reached; code is not data.
    const Words w{{adr(1, 12), ldur_w9_x1_m4, br(1), mov_x8_172, svc0, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.empty());
    CHECK(!in_ranges(data, 16));
}

TEST(a_load_overwrites_the_register_it_names_as_base_and_the_next_load_reads_elsewhere) {
    // adr x8, P (32); ldr w8, [x8]; ldr w0, [x8, #16]; ret; ... P: a pool word; 48: a word nothing proven loads.
    const Words w{{adr(8, 32), 0xb940'0108, 0xb940'1100, ret, nop, nop, nop, nop, svc_data, nop, nop, nop, svc_data}};
    const auto data = run(w, {0});
    CHECK(data.ranges.size() == 1 && data.ranges[0].begin == 32 && data.ranges[0].end == 36);
    CHECK(!in_ranges(data, 48));
}

// Rules without a test: each access form proves exactly the bytes it reads from a formed address.
TEST(every_access_form_proves_exactly_the_bytes_it_reads_from_a_formed_address) {
    constexpr std::uint32_t ldp_post_8 = 0x28c1'0500;    // ldp w0, w1, [x8], #8: reads [x8, x8 + 8)
    constexpr std::uint32_t ldp_offset_4 = 0x2940'8500;  // ldp w0, w1, [x8, #4]: reads [x8 + 4, x8 + 12)
    constexpr std::uint32_t ldp_pre_8 = 0x29c1'0500;     // ldp w0, w1, [x8, #8]!: reads [x8 + 8, x8 + 16)
    const std::pair<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> forms[] = {
        {ldp_post_8, {32, 40}}, {ldp_offset_4, {36, 44}}, {ldp_pre_8, {40, 48}}};
    for (const auto &[access, extent] : forms) {
        const Words w{{adr(8, 32), access, ret, nop, nop, nop, nop, nop, svc_data, svc_data, svc_data, svc_data,
                       svc_data, svc_data}};
        const auto data = run(w, {0});
        CHECK(data.ranges.size() == 1);
        CHECK(data.ranges.size() == 1 && data.ranges[0].begin == extent.first && data.ranges[0].end == extent.second);
    }
}

TEST(a_simd_or_exclusive_access_from_a_formed_address_proves_no_extent) {
    constexpr std::uint32_t ldxr_w0_x8 = 0x885f'7d00;  // ldxr w0, [x8]
    constexpr std::uint32_t ld1_v0_4s_x8 = 0x4c40'7900;  // ld1 {v0.4s}, [x8]
    for (const auto access : {ldxr_w0_x8, ld1_v0_4s_x8}) {
        const Words w{{adr(8, 32), access, ret, nop, nop, nop, nop, nop, svc_data, svc_data}};
        const auto data = run(w, {0});
        CHECK(data.ranges.empty());
        CHECK(std::ranges::find(data.unproven_bases, std::uint64_t{32}) != data.unproven_bases.end());
    }
}

TEST(an_address_that_a_register_overwrite_leaves_behind_was_taken_and_is_reached) {
    constexpr std::uint32_t movz_x9_0 = 0xd280'0009;  // mov x9, #0
    // adr x9, stub; mov x9, #0; ret; ...; stub: mov x8, #172; svc #0; ret. The address was formed and then dropped.
    const Words w{{adr(9, 16), movz_x9_0, ret, nop, mov_x8_172, svc0, ret}};
    const auto data = run(w, {0});
    CHECK(data.reached_words == 6);
}

TEST(an_adrp_whose_use_is_not_seen_keeps_the_words_of_its_page_code) {
    // adrp x5, 0 and nothing reads x5 before the run ends: a pointer into the page may be taken from it.
    const Words w{{adrp(5, 0), nop, ldr_literal(0, 12), ret, nop, svc_data, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.empty() && data.dropped_unresolved == 1);
}

TEST(code_reached_through_a_cbz_a_b_cond_or_a_tbz_is_proven_and_loads_from_it) {
    for (const auto branch : {cbz_x0(16), b_cond(16, 0), tbz_x0(16)}) {
        // 0: branch to 16; 4: ret; 16: ldr w1, <24>; 20: ret; 24: pool.
        const Words w{{branch, ret, nop, nop, ldr_literal(1, 8), ret, svc_data, ret}};
        const auto data = run(w, {0});
        CHECK(data.ranges.size() == 1 && data.ranges[0].begin == 24 && data.ranges[0].end == 28);
    }
}

TEST(a_literal_that_runs_past_its_section_is_not_data_and_two_sections_are_kept_apart) {
    const Words first{{ldr_literal(0, 8, true), ret, svc_data}};  // an 8-byte literal at 8, the section ends at 12
    const Words second{{ret, svc_data, ret}};
    const Code both[] = {first.code(0), second.code(0x100)};
    const auto data = apk::data::find_ranges(both, Roots{{0, 0x100}, {}, {}, {}, {}});
    CHECK(data.ranges.empty());
}

TEST(a_literal_that_spans_two_adjacent_sections_is_not_data) {
    // The first section ends at 12, the second starts there; an 8-byte literal at 8 would run into it.
    const Words first{{ldr_literal(0, 8, true), ret, svc_data}};
    const Words second{{svc_data, ret}};
    const Code both[] = {first.code(0), second.code(12)};
    const auto data = apk::data::find_ranges(both, Roots{{0}, {}, {}, {}, {}});
    CHECK(data.ranges.empty());
}

TEST(unaligned_and_foreign_roots_are_ignored) {
    const Words w{{ret, ret}};
    const auto data = run(w, {3, 1000, 0});
    CHECK(data.reached_words == 1);
}

TEST(an_aps2_table_gives_the_addends_of_its_relative_relocations) {
    // "APS2", 2 relocations, first offset 0x1000; one group of 2, flags: by info + by offset delta + has addend;
    // delta 8, info 1027 (RELATIVE), the addend deltas 0x4000 and 0x10.
    const unsigned char table[] = {'A', 'P', 'S', '2', 2, 0x80, 0x20, 2, 0x0b, 8, 0x83, 0x08, 0x80, 0x80, 0x01, 0x10};
    const auto addends = apk::data::packed_relocation_addends(
        {reinterpret_cast<const std::byte *>(table), sizeof(table)});
    CHECK(addends.size() == 2);
    CHECK(addends.size() == 2 && addends[0] == 0x4000 && addends[1] == 0x4010);
}

TEST(the_time_stays_linear_on_alternating_adrp_and_literal_words) {
    Words w;
    for (int i = 0; i < 100'000; ++i) {
        w.words.push_back(adrp(0, 1));
        w.words.push_back(ldr_literal(1, 8));
    }
    w.words.push_back(ret);
    const auto started = std::chrono::steady_clock::now();
    const auto data = run(w, {0});
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(seconds < 5.0);
    CHECK(data.reached_words > 0);
}


TEST(a_pointer_in_data_is_only_a_weak_root_and_what_it_reaches_never_loads) {
    // A `.quad pool` in .data points at a word that decodes as a literal load of a real stub. The pointer is weak:
    // the word blocks (it is not data) but it does not load, so its target is not taken for data.
    // 0: ldr w1, <16>; 4: ret; 16: the pointed-at word (ldr x0, <+8>); 24: a word the "load" would name.
    const Words w{{ldr_literal(1, 16), ret, nop, nop, 0x5800'0040, nop, svc_data, ret}};
    const auto data = run(w, {0}, {16});
    CHECK(data.ranges.empty());
    CHECK(data.dropped_reached == 1);  // the loaded word is the weakly reached one
    CHECK(!in_ranges(data, 24));
    CHECK(data.reach_of(16) == apk::data::Reach::weak && data.reach_of(0) == apk::data::Reach::strong);
}

TEST(an_address_that_code_takes_is_weak) {
    // adr x9, <stub>; str x9, [x2] (taken, stored); the stub is reached weakly and its literal load is no evidence.
    const Words w{{adr(9, 12), 0xf900'0049, ret, ldr_literal(0, 12), ret, nop, svc_data}};
    const auto data = run(w, {0});
    CHECK(data.reach_of(12) == apk::data::Reach::weak);
    CHECK(data.ranges.empty());
}

TEST(a_call_to_an_import_that_never_returns_ends_the_run_and_the_pool_after_it_stays_unreached) {
    // 0: ldr w0, <8>; 4: bl <40> (abort's PLT entry); 8: the loaded pool word, behind a call that never returns.
    const Words w{{ldr_literal(0, 8), bl(36), svc_data, ret}};
    const auto with = run(w, {0}, {}, {40});
    CHECK(with.ranges.size() == 1 && with.ranges[0].begin == 8 && with.ranges[0].end == 12);
    CHECK(with.reach_of(8) == apk::data::Reach::none);
    // What follows such a call is no code, loaded or not: a word nothing loads is not reached either.
    const Words after{{ldr_literal(0, 16), bl(36), nop, ret, svc_data, nop, nop, nop, nop, nop, ret}};
    CHECK(run(after, {0}, {}, {40}).reach_of(8) == apk::data::Reach::none);
    CHECK(run(after, {0}).reach_of(8) == apk::data::Reach::weak);  // a call that returns: reached, not proven
    // Without the name the run falls through the call into the loaded word: reached, so it stays code.
    const auto without = run(w, {0});
    CHECK(without.ranges.empty() && without.dropped_reached == 1);
}

TEST(a_run_that_meets_an_undecodable_word_is_proven_only_up_to_its_last_call) {
    // 0: bl <28>; 4: ldr w1, <24> (after the call); 8: a udf: the words after the call are no proven code. Nothing
    // falls into the loaded word, so only the udf takes the proof away.
    const Words w{{bl(28), ldr_literal(1, 20), 0x0000'0000, nop, ret, ret, svc_data, ret}};
    const auto data = run(w, {0});
    CHECK(data.ranges.empty());
    CHECK(data.reach_of(4) == apk::data::Reach::weak);
    CHECK(data.reach_of(0) == apk::data::Reach::strong);
}

TEST(an_unresolved_register_branch_poisons_the_data_inside_its_function) {
    // 0: ldr w0, <16>; 4: br x9 (unresolved); 8, 12: padding; 16: the pool. The function is [0, 20).
    const Words w{{ldr_literal(0, 16), br(9), nop, nop, svc_data, ret}};
    const auto poisoned = run(w, {0}, {}, {}, {{0, 20}});
    CHECK(poisoned.ranges.empty() && poisoned.dropped_dispatch == 1);
    // Without the unresolved branch the same pool is data.
    const Words fine{{ldr_literal(0, 16), ret, nop, nop, svc_data, ret}};
    const auto data = run(fine, {0}, {}, {}, {{0, 20}});
    CHECK(data.ranges.size() == 1);
}

TEST(the_bound_is_only_the_exact_form) {
    // cmp w0,#3 then b.hs 4 entries; then a write of the index register before the load: no proof.
    const auto table = [](std::uint32_t second, std::uint32_t between) {
        return Words{{cmp_w_imm(0, 3), second, between, adr(1, 16), 0xb860'5820, ret, ret, svc_data, svc_data, svc_data,
                      svc_data, ret}};
    };
    const auto hi = run(table(b_hi(20), nop), {0});
    CHECK(hi.ranges.size() == 1 && hi.ranges[0].end - hi.ranges[0].begin == 16);  // n + 1 = 4 entries
    const auto hs = run(table(b_cond(20, 2), nop), {0});
    CHECK(hs.ranges.size() == 1 && hs.ranges[0].end - hs.ranges[0].begin == 12);  // n = 3 entries
    const auto lo = run(table(b_cond(20, 3), nop), {0});
    CHECK(lo.ranges.empty());  // b.lo is no bound
    const auto written = run(table(b_hi(20), 0x1280'0000 /* mov w0, #0 */), {0});
    CHECK(written.ranges.empty() && written.unbounded_tables == 1);  // the index register was written
    const auto flags = run(table(0x7100'083f /* cmp w1, #2 */, b_hi(20)), {0});
    CHECK(flags.ranges.empty());  // another compare set the flags the branch reads
}

TEST(the_refused_words_are_counted_by_how_they_were_reached) {
    // 0: strong; 4: ret; 8 is weakly reached (a weak root); 12: unreached; an unproven base: adr x1,<12>; ldr by index.
    const Words w{{adr(1, 20), 0xb860'5820, ret, nop, ret, nop, nop, nop, svc_data}};
    const auto data = run(w, {0}, {12});
    CHECK(data.reach_of(0) == apk::data::Reach::strong);
    CHECK(data.reach_of(12) == apk::data::Reach::weak);
    CHECK(data.reach_of(28) == apk::data::Reach::none);
    CHECK(data.behind_unproven_base(28) && !data.behind_unproven_base(12));
    CHECK(data.unbounded_tables == 1);
}

/// A library built by hand: one executable section at 0x200 of `words`, with the given function symbols (strong roots).
std::vector<std::byte> library(const std::vector<std::uint32_t> &words,
                               std::initializer_list<apk::testing::SymbolEntry> symbols,
                               std::uint64_t entry = 0x4200) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.put<std::uint64_t>(24, entry);
    builder.bytes.resize(0x400);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    std::memcpy(builder.bytes.data() + 0x200, words.data(), words.size() * 4);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, words.size() * 4}}, symbols);
    return builder.bytes;
}

TEST(a_site_before_a_pool_and_one_after_it_are_both_found_and_the_pool_is_not) {
    // f at 0x200: ldr w9, <0x210>; svc #0; ret; nop. 0x210: the pool word. g at 0x214: svc #0; ret.
    const auto bytes =
        library({ldr_literal(9, 16), svc0, ret, nop, svc_data, svc0, ret}, {{"f", 0x200, 12}, {"g", 0x214, 8}});
    const auto scan = apk::container::scan_code(bytes);
    const auto sites = apk::container::site_addresses(scan);
    CHECK((sites == std::vector<std::uint64_t>{0x204, 0x214}));
    CHECK(scan.data.ranges.size() == 1 && scan.data.ranges[0].begin == 0x210 && scan.data.ranges[0].end == 0x214);
    const auto unsplit = apk::container::site_addresses(apk::container::scan_code(bytes, false));
    CHECK((unsplit == std::vector<std::uint64_t>{0x204, 0x214}) || unsplit.size() >= sites.size());
}

TEST(a_branch_across_a_data_range_into_a_relocated_window_still_gets_its_entry_pad) {
    // 0x200: ldr w9, <0x20c>; b <0x214> (onto the add inside the window); nop; 0x20c: the pool word; the window of
    // the rewrite tests at 0x210: ldxr x17,[x13]; add x18,x17,x14; stlxr w16,x18,[x13]; cbnz w16, ldxr; ret.
    const auto bytes = library({ldr_literal(9, 12), 0x1400'0004, nop, svc_data, 0xc85f'7db1, 0x8b0e'0232, 0xc810'fdb2,
                                0x35ff'ffb0, ret},
                               {{"f", 0x200, 12}});
    const auto scan = apk::container::scan_code(bytes);
    CHECK(scan.data.ranges.size() == 1 && scan.data.ranges[0].begin == 0x20c && scan.data.ranges[0].end == 0x210);
    CHECK(scan.findings.exclusive_regions.size() == 1);
    if (scan.findings.exclusive_regions.size() == 1) {
        CHECK(scan.findings.exclusive_regions[0].address == 0x210);
        CHECK(scan.findings.exclusive_regions[0].entries == std::vector<std::uint32_t>{1});
    }
}


TEST(the_data_boundary_refuses_a_site_a_window_or_a_jump_table_branch_that_touches_data) {
    apk::data::DataRanges data;
    data.ranges.push_back({0x20c, 0x210});
    const auto refused = [&](const apk::rewrite::Findings &findings) {
        try {
            apk::container::check_data_boundary(findings, data);
        } catch (const apk::container::Incompatible &) {
            return true;
        }
        return false;
    };
    apk::rewrite::Findings none;
    CHECK(!refused(none));
    apk::rewrite::Findings site;
    site.sites.push_back({.address = 0x20c, .kind = apk::rewrite::SiteKind::syscall, .reg = 0});
    CHECK(refused(site));
    apk::rewrite::Findings next_to;
    next_to.sites.push_back({.address = 0x210, .kind = apk::rewrite::SiteKind::syscall, .reg = 0});
    next_to.sites.push_back({.address = 0x208, .kind = apk::rewrite::SiteKind::syscall, .reg = 0});
    CHECK(!refused(next_to));  // the words beside the data are code
    apk::rewrite::Findings window;
    window.exclusive_regions.push_back({.address = 0x208, .words = {nop, nop, nop}});
    CHECK(refused(window));  // spans 0x208..0x214
    apk::rewrite::Findings before;
    before.exclusive_regions.push_back({.address = 0x204, .words = {nop, nop}});
    CHECK(!refused(before));  // ends at 0x20c
    apk::rewrite::Findings table;
    table.jump_tables.push_back({.address = 0x20c, .table = 0x300, .entries = 4, .targets = {}});
    CHECK(refused(table));
}


TEST(only_a_compare_of_the_load_s_own_index_register_is_a_bound) {
    // cmp w1,#2 then b.hi, but the table is read by w0.
    const Words w{{cmp_w_imm(1, 2), b_hi(20), nop, adr(1, 16), 0xb860'5820, ret, ret, svc_data, svc_data, svc_data,
                   ret}};
    CHECK(run(w, {0}).ranges.empty());
    const Words control{{cmp_w_imm(0, 2), b_hi(20), nop, adr(1, 16), 0xb860'5820, ret, ret, svc_data, svc_data,
                         svc_data, ret}};
    CHECK(run(control, {0}).ranges.size() == 1);
}

TEST(a_flag_setting_word_between_the_compare_and_the_branch_voids_the_bound) {
    // cmp w0,#3; subs w2,w2,#1; b.hi: the branch reads the flags of the subs.
    const Words w{{cmp_w_imm(0, 3), 0x7100'0442, b_hi(16), adr(1, 16), 0xb860'5820, ret, ret, svc_data, svc_data,
                   svc_data, svc_data, ret}};
    CHECK(run(w, {0}).ranges.empty());
    const Words control{{cmp_w_imm(0, 3), nop, b_hi(16), adr(1, 16), 0xb860'5820, ret, ret, svc_data, svc_data,
                         svc_data, svc_data, ret}};
    CHECK(run(control, {0}).ranges.size() == 1);
}

TEST(a_jump_table_added_with_the_wrong_shift_is_not_proven) {
    auto w = table_function(true);
    w.words[4] = 0x8b02'0821;  // add x1, x1, x2, lsl #2: not the form for 4-byte entries
    const auto data = run(w, {0}, {}, {}, {{0, 76}});
    CHECK(data.ranges.empty() && data.dropped_dispatch >= 1);
    // A byte table whose entries are added shifted by 1 (not 2): the entries would be aligned, the form is still wrong.
    const Words bytes{{cmp_w_imm(0, 1), b_hi(44), adr(1, 20), ldrb_indexed, adr(3, 16), 0x8b02'0463 /* lsl #1 */, br(3),
                       0x0000'0200, mov_x8_172, svc0, ret, ret, ret}};
    const auto wrong = run(bytes, {0}, {}, {}, {{0, 52}});
    CHECK(wrong.ranges.empty() && wrong.dropped_dispatch >= 1);
}

TEST(the_function_of_an_unresolved_branch_is_its_fde_not_the_stretch_between_two_roots) {
    // A root inside the function at 12; the pool at 16 lies in the function [0, 20), beyond that root.
    const Words w{{ldr_literal(0, 16), br(9), nop, ret, svc_data, ret}};
    const auto data = run(w, {0, 12}, {}, {}, {{0, 20}});
    CHECK(data.ranges.empty() && data.dropped_dispatch == 1);
}

/// A library with a pointer in a data section (no relocation table, no symbols; the entry point is the first word) to a
/// word that decodes as a literal load of a caspal: the pointer is a weak root, so the caspal stays code and is
/// refused.
TEST(a_pointer_in_a_data_section_is_weak_so_what_it_reaches_does_not_make_data) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const std::uint32_t words[] = {ret, ldr_literal(0, 16, true), ret, nop, nop, 0x4862'fe44, nop, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    const std::uint64_t pointer = 0x204;
    std::memcpy(builder.bytes.data() + 0x300, &pointer, 8);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words},
                      {".data.rel.ro", sht_progbits, shf_alloc, 0x300, 0x300, 8}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.reach_of(0x204) == apk::data::Reach::weak);  // the pointer is found, and weak
    CHECK(scan.data.ranges.empty());
    CHECK(scan.findings.x18_unsupported.size() == 1);  // the caspal at 0x214, loudly
}

TEST(an_addend_of_a_relative_relocation_is_weak_too) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const std::uint32_t words[] = {ret, ldr_literal(0, 16, true), ret, nop, nop, 0x4862'fe44, nop, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    const std::uint64_t entry[] = {0x500, 1027, 0x204};  // offset, R_AARCH64_RELATIVE, addend
    std::memcpy(builder.bytes.data() + 0x300, entry, sizeof entry);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words},
                      {".rela.dyn", 4, shf_alloc, 0x300, 0x300, sizeof entry}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.reach_of(0x204) == apk::data::Reach::weak);
    CHECK(scan.data.ranges.empty());
    CHECK(scan.findings.x18_unsupported.size() == 1);
}


// The table of a library built by the NDK has the section type SHT_ANDROID_RELA (0x60000002): the addends of its
// relocations are roots like those of a plain RELA table, which a test of the decoder alone does not show.
TEST(a_packed_table_in_a_section_of_its_own_type_makes_its_addend_weak) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const std::uint32_t words[] = {ret, ldr_literal(0, 16, true), ret, nop, nop, 0x4862'fe44, nop, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    // "APS2", 1 relocation, first offset 0; one group of 1, flags 8 (addend only): offset delta 0x500, info 1027
    // (RELATIVE), addend delta 0x204.
    const unsigned char table[] = {'A', 'P', 'S', '2', 1, 0, 1, 8, 0x80, 0x0a, 0x83, 0x08, 0x84, 0x04};
    std::memcpy(builder.bytes.data() + 0x300, table, sizeof table);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words},
                      {".rela.dyn", 0x6000'0002, shf_alloc, 0x300, 0x300, sizeof table}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.reach_of(0x204) == apk::data::Reach::weak);
    CHECK(scan.data.ranges.empty());
}

// ...and one that fills an init array is trusted, as in a plain table.
TEST(a_packed_relocation_that_fills_the_init_array_is_a_strong_root) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const std::uint32_t words[] = {ret, ldr_literal(0, 16, true), ret, nop, nop, 0x4862'fe44, nop, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    const unsigned char table[] = {'A', 'P', 'S', '2', 1, 0, 1, 8, 0x80, 0x0a, 0x83, 0x08, 0x84, 0x04};
    std::memcpy(builder.bytes.data() + 0x300, table, sizeof table);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words},
                      {".init_array", sht_progbits, shf_alloc | shf_write, 0x500, 0x340, 8},
                      {".rela.dyn", 0x6000'0002, shf_alloc, 0x300, 0x300, sizeof table}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.reach_of(0x204) == apk::data::Reach::strong);
}

TEST(the_offsets_run_on_across_a_shared_group_and_stop_at_its_count) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const std::uint32_t words[] = {ret, ldr_literal(0, 16, true), ret, nop, nop, 0x4862'fe44, nop, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    // "APS2", 3 relocations, first offset 0x4f8. Group 1: 2 relocations, flags 15 (info, delta and addend shared),
    // delta 8, info 1027, addend delta 0x204: offsets 0x500 and 0x508, addend 0x204. Group 2: 1 relocation, flags 8:
    // offset delta 0x18 (to 0x520), info 1027, addend delta 4 (0x208).
    const unsigned char table[] = {'A', 'P', 'S', '2', 3, 0xf8, 0x09, 2, 15, 8, 0x83, 0x08, 0x84, 0x04,
                                   1,   8, 0x18, 0x83, 0x08, 4};
    std::memcpy(builder.bytes.data() + 0x300, table, sizeof table);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words},
                      {".init_array", sht_progbits, shf_alloc | shf_write, 0x520, 0x340, 8},
                      {".rela.dyn", 0x6000'0002, shf_alloc, 0x300, 0x300, sizeof table}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.reach_of(0x208) == apk::data::Reach::strong);  // the second group's offset is 0x520
    CHECK(scan.data.reach_of(0x204) == apk::data::Reach::weak);    // the shared group ends at 0x508, before the array
}

TEST(the_packed_decoder_keeps_the_offsets_of_its_relocations) {
    const unsigned char table[] = {'A', 'P', 'S', '2', 2, 0x80, 0x20, 2, 0x0b, 8, 0x83, 0x08, 0x80, 0x80, 0x01, 0x10};
    const auto runs = apk::data::packed_relocations({reinterpret_cast<const std::byte *>(table), sizeof(table)});
    CHECK(runs.size() == 2 && runs[0].offset == 0x1008 && runs[1].offset == 0x1010 && runs[1].count == 1);
    // A group that reads nothing per relocation is one run: 39,104 relocations 8 apart from offset 8.
    const unsigned char shared[] = {'A', 'P', 'S', '2', 0xc0, 0xb1, 0x02, 0, 0xc0, 0xb1, 0x02, 0x03, 8, 0x83, 0x08};
    const auto big = apk::data::packed_relocations({reinterpret_cast<const std::byte *>(shared), sizeof shared});
    CHECK(big.size() == 1 && big[0].offset == 8 && big[0].delta == 8 && big[0].count == 39'104);
}

constexpr std::uint32_t brk1 = 0xd420'0020;
constexpr std::uint32_t casp_x0_x4 = 0x4820'7c82;  // casp x0, x1, x2, x3, [x4]
constexpr std::uint32_t mrs_tpidrro = 0xd53b'd060;  // mrs x0, TPIDRRO_EL0
constexpr std::uint32_t ldr_w0_index_w5 = 0xb865'5820;  // ldr w0, [x1, w5, uxtw #2]
constexpr std::uint32_t cmp_w5_3 = 0x7100'0cbf;

template <typename F>
double seconds(F &&f) {
    const auto started = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

// ---- More layouts that made an earlier version of the split go wrong: each is a case.

TEST(a_callee_is_not_proven_by_its_caller_when_an_address_is_live_at_the_call) {
    // f is sized (a strong root) and runs `adr x0, <string>; bl g`. g is not a root of its own and its pool load proves
    // nothing: a callee is reached, never trusted. The address live in x0 is also reached, weakly.
    const std::vector<std::uint32_t> w{adr(0, 24), bl(8), ret, ldr_literal(1, 8), ret, svc_data, nop};
    const Code codes[] = {{{reinterpret_cast<const std::byte *>(w.data()), w.size() * 4}, 0}};
    const auto data = apk::data::find_ranges(codes, Roots{{0}, {}, {}, {}, {}});
    CHECK(data.ranges.empty());
}

TEST(ranges_do_not_merge_across_a_section_boundary) {
    // Two sections that touch: the last word of the first and the first word of the second are both read by proven
    // loads. Two ranges, each inside its own section (the header says so), not one that spans both.
    const std::vector<std::uint32_t> a{ldr_literal(0, 12), ret, ret, svc_data};
    const std::vector<std::uint32_t> b{svc_data, ldr_literal(0, -4), ret};
    const Code codes[] = {{{reinterpret_cast<const std::byte *>(a.data()), a.size() * 4}, 0},
                          {{reinterpret_cast<const std::byte *>(b.data()), b.size() * 4}, 16}};
    const auto data = apk::data::find_ranges(codes, Roots{{0, 20}, {}, {}, {}, {}});
    CHECK(data.ranges.size() == 2);
    CHECK(data.ranges.size() == 2 && data.ranges[0].end == 16 && data.ranges[1].begin == 16);
}

// The boundary check compares each rewritten site with the ranges by binary search, so the time
// grows with the size of the library, not with the product of its sites and its ranges.
TEST(the_boundary_check_is_linear_in_the_size_of_a_library) {
    using namespace apk::testing;
    // n units of {ldr w0, <pool>; ret; pool word; svc #0}: n proven ranges and n sites, each unit a sized function.
    const auto library_of = [](std::size_t n) {
        ElfBuilder builder;
        const std::size_t size = n * 16;
        builder.bytes.resize(0x200 + size);
        builder.segments({{pt_load, r | x, 0, 0, 0x200 + size, 0x200 + size, 0x4000}});
        std::vector<SymbolEntry> symbols;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t unit[] = {ldr_literal(0, 8), ret, svc_data, svc0};
            std::memcpy(builder.bytes.data() + 0x200 + 16 * i, unit, sizeof unit);
            symbols.push_back({"f", 0x200 + 16 * i, 16});
        }
        builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, size}}, symbols);
        return builder.bytes;
    };
    const auto small = library_of(20000), large = library_of(80000);
    const auto t_small = seconds([&] { apk::container::convert(small, {.install_name = "@rpath/t.dylib"}); });
    const auto t_large = seconds([&] { apk::container::convert(large, {.install_name = "@rpath/t.dylib"}); });
    CHECK(t_large < 8 * t_small + 0.2);  // four times the library: about four times the time, not sixteen
}

TEST(overlapping_executable_sections_give_no_ranges) {
    // Two sections that overlap: a literal inside both. The range would belong to one of them and be masked in the
    // other past its end.
    const std::vector<std::uint32_t> a{ldr_literal(0, 8), ret, svc_data, svc_data, ret};
    const std::vector<std::uint32_t> b{svc_data, svc_data, ret, ret};
    const Code codes[] = {{{reinterpret_cast<const std::byte *>(a.data()), a.size() * 4}, 0},
                          {{reinterpret_cast<const std::byte *>(b.data()), b.size() * 4}, 8}};
    const auto data = apk::data::find_ranges(codes, Roots{{0}, {}, {}, {}, {}});
    CHECK(data.ranges.empty());
}

TEST(masking_stays_inside_each_section) {
    using namespace apk::testing;
    // .text at 0x200 (6 words) and .text2 at 0x20c overlapping its last three words; the pool at 0x208 is read from
    // .text; every byte of the converter's copy is inside its section whatever the ranges say.
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const std::uint32_t words[] = {ldr_literal(0, 8), ret, svc_data, svc_data, svc_data, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, 24},
                      {".text2", sht_progbits, shf_alloc | shf_execinstr, 0x20c, 0x20c, 12}});
    const auto scan = apk::container::scan_code(builder.bytes);  // under the sanitizer build: no report
    CHECK(scan.data.ranges.empty());
}

TEST(a_group_that_reads_nothing_per_relocation_costs_no_time_and_no_memory) {
    // "APS2", 5,000,000 relocations, first offset 0, one group of 5,000,000 by info + by delta, no addend: the
    // group header (size, flags, delta, info) is all the file holds.
    const unsigned char table[] = {'A', 'P', 'S', '2', 0xc0, 0xb1, 0x02, 0, 0xc0, 0xb1, 0x02, 0x03, 8, 0x83, 0x08};
    std::vector<std::uint64_t> addends;
    const auto took = seconds([&] {
        addends = apk::data::packed_relocation_addends({reinterpret_cast<const std::byte *>(table), sizeof table});
    });
    CHECK(took < 1.0);
    CHECK(addends.size() < 64);
}

TEST(a_group_with_a_huge_count_ends_with_the_stream) {
    // The same, but the group reads one byte per relocation (no delta): the stream ends long before the count.
    const unsigned char table[] = {'A', 'P', 'S', '2', 0xc0, 0xb1, 0x02, 0, 0xc0, 0xb1, 0x02, 0x01, 0x83, 0x08,
                                   1,   1,   1};
    std::vector<std::uint64_t> addends;
    const auto took = seconds([&] {
        addends = apk::data::packed_relocation_addends({reinterpret_cast<const std::byte *>(table), sizeof table});
    });
    CHECK(took < 1.0);
    CHECK(addends.size() <= sizeof table);
}

TEST(labels_without_a_type_or_a_size_and_the_entry_point_reach_but_never_prove) {
    using namespace apk::testing;
    const auto words = std::vector<std::uint32_t>{ldr_literal(9, 16), ret, nop, nop, 0x1234'5678, ldr_literal(0, 12),
                                                  ret, nop, svc0, ret};
    struct Case {
        const char *name;
        std::vector<SymbolEntry> symbols;
        std::uint64_t entry;
    };
    const Case cases[] = {
        {"notype label", {{"f", 0x200, 8}, {"lab", 0x210, 0, 0}}, 0x4200},
        {"function symbol without size", {{"f", 0x200, 8}, {"lab", 0x210, 0, 2}}, 0x4200},
        {"entry point", {{"f", 0x200, 8}}, 0x210},
    };
    for (const auto &c : cases) {
        ElfBuilder builder;
        builder.put<std::uint64_t>(24, c.entry);
        builder.bytes.resize(0x400);
        builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
        std::memcpy(builder.bytes.data() + 0x200, words.data(), words.size() * 4);
        std::vector<SymbolEntry> symbols = c.symbols;
        if (symbols.size() == 2) {
            builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, words.size() * 4}},
                             {symbols[0], symbols[1]});
        } else {
            builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, words.size() * 4}},
                             {symbols[0]});
        }
        const auto scan = apk::container::scan_code(builder.bytes);
        const auto sites = apk::container::site_addresses(scan);
        CHECK(std::ranges::find(sites, std::uint64_t{0x220}) != sites.end());  // the victim's svc is a site
        CHECK(scan.data.ranges.empty());  // the labelled pool is reached, so it stays code, and its words load nothing
    }
}

TEST(a_casp_overwrites_two_registers_and_the_next_load_reads_nothing_known) {
    // adr x1, P; ldr w9, [x1]; casp x0,x1,x2,x3,[x4]; ldr x11, [x1, #16]: x1 is the casp's second result.
    const auto make = [](std::uint32_t middle) {
        return Words{{adr(1, 28), 0xb940'0029, middle, 0xf940'082b, ret, nop, nop, svc_data, svc_data, svc_data,
                      svc_data, svc_data, svc_data, ret}};
    };
    const auto data = run(make(casp_x0_x4), {0});
    CHECK(in_ranges(data, 28));        // the first load: P itself
    CHECK(!in_ranges(data, 28 + 16));  // the load through the overwritten x1
    const auto control = run(make(nop), {0});
    CHECK(in_ranges(control, 28 + 16));  // without the casp the same load reads P + 16
}

TEST(many_roots_that_all_end_in_one_udf_cost_linear_time) {
    const auto timed = [](int n) {
        Words w;
        for (int i = 0; i < n; ++i) w.words.push_back(nop);
        w.words.push_back(0);
        w.words.push_back(ret);
        std::vector<std::uint64_t> roots;
        for (int i = 0; i < n; ++i) roots.push_back(static_cast<std::uint64_t>(i) * 4);
        return seconds([&] { run(w, roots); });
    };
    const auto small = timed(5000), large = timed(20000);
    CHECK(large < 0.2 || large < 8 * (small + 0.01));
}

TEST(an_unresolved_register_branch_per_function_costs_linear_time) {
    const auto timed = [](int n) {
        Words w;
        std::vector<std::uint64_t> roots;
        std::vector<apk::data::Range> functions;
        for (int i = 0; i < n; ++i) {
            roots.push_back(w.words.size() * 4);
            functions.push_back({w.words.size() * 4, w.words.size() * 4 + 12});
            w.words.insert(w.words.end(), {nop, br(1), ret});
        }
        return seconds([&] { run(w, roots, {}, {}, functions); });
    };
    const auto small = timed(10000), large = timed(40000);
    CHECK(large < 0.2 || large < 8 * (small + 0.01));
}

TEST(an_object_symbol_in_dynsym_is_no_pointer_into_code) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const std::uint32_t words[] = {ldr_literal(9, 8), ret, 0x1234'5678, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    unsigned char symbol[24] = {};
    symbol[4] = 0x11;  // STB_GLOBAL | STT_OBJECT
    const std::uint64_t pool = 0x208;
    std::memcpy(symbol + 8, &pool, 8);
    std::memcpy(builder.bytes.data() + 0x300, symbol, sizeof symbol);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words},
                      {".dynsym", 11, shf_alloc, 0x300, 0x300, sizeof symbol}},
                     {{"f", 0x200, 8}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.ranges.size() == 1 && scan.data.ranges[0].begin == 0x208);
}

TEST(an_add_into_another_register_resolves_the_page) {
    // 0: ldr w1, <24>; 4: adrp x8, #0; 8: add x0, x8, #0x30; 12: ret; 24: the pool, on the same page.
    const Words w{{ldr_literal(1, 24), adrp(8, 0), add_imm(0, 8, 0x30), ret, nop, nop, 0x1234'5678, ret}};
    const auto data = run(w, {0});
    CHECK(data.dropped_unresolved == 0);
    CHECK(in_ranges(data, 24));
}

TEST(the_hook_lists_a_tpidrro_read) {
    const auto bytes = library({mrs_tpidrro, ret}, {{"f", 0x200, 8}});
    const auto scan = apk::container::scan_code(bytes);
    const auto sites = apk::container::site_addresses(scan);
    CHECK(std::ranges::find(sites, std::uint64_t{0x200}) != sites.end());
}

TEST(a_load_shaped_data_word_still_makes_the_word_it_names_a_literal) {
    // f: ldr w9, <0x208>; ret; 0x208: ldr w0, <+4> (loaded, data); 0x20c: svc #0x1234, which that load names. Without
    // the split the rewriter keeps the svc word as a literal's target; the split must not make it a refusal.
    const auto bytes = library({ldr_literal(9, 8), ret, ldr_literal(0, 4), svc_data, ret}, {{"f", 0x200, 8}});
    const auto split = apk::container::scan_code(bytes);
    const auto unsplit = apk::container::scan_code(bytes, false);
    CHECK(split.findings.sites.size() == unsplit.findings.sites.size());
    CHECK(split.findings.data_syscalls == unsplit.findings.data_syscalls);
}

TEST(a_load_shaped_data_word_does_not_make_a_word_that_is_reached_code_a_literal) {
    // f: ldr x0, <0x208>; ret; 0x208: the 8-byte literal 0x5800006012345678, whose high word decodes as
    // `ldr x0, <+12>` and names 0x218; g (sized, at 0x210): mov x8, #172; nop; svc #0; ret. The svc is real code, so
    // it is a site that gets rewritten, not a literal that is kept.
    const auto bytes = library({ldr_literal(0, 8, true), ret, 0x1234'5678, 0x5800'0060, mov_x8_172, nop, svc0, ret},
                               {{"f", 0x200, 8}, {"g", 0x210, 16}});
    const auto scan = apk::container::scan_code(bytes);
    const auto sites = apk::container::site_addresses(scan);
    CHECK(std::ranges::find(sites, std::uint64_t{0x218}) != sites.end());
    CHECK(scan.findings.data_syscalls.empty());
}

TEST(a_load_shaped_word_that_only_a_pointer_reaches_does_not_make_real_code_a_literal) {
    // The same layout, but the pool's high word is the start of a symbol without a size, so it is reached and the
    // pool, which lies in f, stays code: that word names the svc of g (0x218), real code in another function, which
    // must be rewritten, not kept.
    const auto bytes = library({ldr_literal(0, 8, true), ret, 0x1234'5678, 0x5800'0060, mov_x8_172, nop, svc0, ret},
                               {{"f", 0x200, 16}, {"t", 0x20c, 0}, {"g", 0x210, 16}});
    const auto scan = apk::container::scan_code(bytes);
    CHECK(scan.findings.data_syscalls.empty());
    const auto sites = apk::container::site_addresses(scan);
    CHECK(std::ranges::find(sites, std::uint64_t{0x218}) != sites.end());
}

TEST(an_unproven_load_inside_its_own_function_still_makes_the_word_it_names_a_literal) {
    // f (sized, 0x200..0x218): mov x8, #172; svc #0 (a barrier, so what follows only reaches); ldr w0, <0x210>;
    // b <0x210>; 0x210: svc #0x1234, a word the load names; ret. The load is not proven and names a word of its own
    // function, so the word stays a literal exactly as without the split.
    const auto bytes = library({mov_x8_172, svc0, ldr_literal(0, 8), 0x1400'0001, svc_data, ret}, {{"f", 0x200, 24}});
    const auto scan = apk::container::scan_code(bytes);
    CHECK(scan.findings.data_syscalls == std::vector<std::uint64_t>{0x210});
}

TEST(fcmp_ccmp_adcs_and_sbcs_set_the_flags_the_branch_reads) {
    for (const std::uint32_t word : {0x1e21'2000u /* fcmp s0, s1 */, 0x7a41'8000u /* ccmp w0, w1, #0, hi */,
                                     0x3a02'0042u /* adcs w2, w2, w2 */, 0x7a02'0042u /* sbcs w2, w2, w2 */}) {
        const Words w{{cmp_w5_3, word, b_hi(16), adr(1, 16), ldr_w0_index_w5, ret, ret, svc_data, svc_data, svc_data,
                       svc_data, ret}};
        CHECK(run(w, {0}).ranges.empty());
    }
    const Words control{{cmp_w5_3, nop, b_hi(16), adr(1, 16), ldr_w0_index_w5, ret, ret, svc_data, svc_data, svc_data,
                         svc_data, ret}};
    CHECK(run(control, {0}).ranges.size() == 1);
}


TEST(the_hook_lists_a_register_branch_that_ends_a_monitor_hull) {
    // ldxr x1, [x2]; br x9: an exclusive load, then a register branch that is no jump table.
    const auto bytes = library({0xc85f'7c41, br(9), ret}, {{"f", 0x200, 12}});
    const auto scan = apk::container::scan_code(bytes);
    CHECK(!scan.findings.hull_ending_register_branches.empty());
    const auto sites = apk::container::site_addresses(scan);
    CHECK(std::ranges::find(sites, std::uint64_t{0x204}) != sites.end());
}


TEST(a_call_to_the_plt_entry_of_abort_ends_the_run_and_one_to_another_import_does_not) {
    using namespace apk::testing;
    const auto build = [](const char *name) {
        ElfBuilder builder;
        builder.bytes.resize(0x500);
        builder.put<std::uint64_t>(24, 0x200);
        builder.segments({{pt_load, r | x, 0, 0, 0x500, 0x500, 0x1000}});
        const std::uint32_t text[] = {bl(0x100 - 0), nop, ret};  // bl <0x300>: the PLT entry
        std::memcpy(builder.bytes.data() + 0x200, text, sizeof text);
        const std::uint32_t plt[] = {0x9000'0010 /* adrp x16, #0 */, 0xf942'0211 /* ldr x17, [x16, #0x400] */,
                                     0x9100'0210, 0xd61f'0220};
        std::memcpy(builder.bytes.data() + 0x300, plt, sizeof plt);
        const std::uint64_t rela[] = {0x400, (std::uint64_t{1} << 32) | 1026, 0};  // JUMP_SLOT of dynsym entry 1
        std::memcpy(builder.bytes.data() + 0x320, rela, sizeof rela);
        unsigned char dynsym[48] = {};
        dynsym[24] = 1;  // entry 1: st_name 1
        std::memcpy(builder.bytes.data() + 0x340, dynsym, sizeof dynsym);
        const std::string names = std::string(1, '\0') + name + std::string(1, '\0');
        std::memcpy(builder.bytes.data() + 0x380, names.data(), names.size());
        builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, 12},
                          {".plt", sht_progbits, shf_alloc | shf_execinstr, 0x300, 0x300, 16},
                          {".rela.plt", 4, shf_alloc, 0x320, 0x320, 24},
                          {".dynsym", 11, shf_alloc, 0x340, 0x340, 48},
                          {".dynstr", 3, shf_alloc, 0x380, 0x380, 16}});
        return apk::container::scan_code(builder.bytes);
    };
    CHECK(build("abort").data.reach_of(0x204) == apk::data::Reach::none);
    CHECK(build("puts").data.reach_of(0x204) == apk::data::Reach::weak);  // the call returns: reached, not proven
}


// ---- Version 5: a word is data only if a load in a trusted run proves it and no run of any kind reaches it.

/// g_cut CASE 1: f (trusted root at 0) loads the pool at 12 and calls (bl) on; behind the call the code falls into the
/// pool, whose second word decodes as a literal load of the svc at 24, the middle of the sized function g (root at 20).
TEST(a_run_that_falls_through_a_call_into_a_pool_never_makes_the_code_it_names_data) {
    const Words w{{ldr_literal(9, 12), bl(28), nop, ldr_literal(0, 12), ret, nop, svc0, ret, ret}};
    const auto data = run(w, {0, 20});
    CHECK(!in_ranges(data, 24));              // the victim's svc stays code
    CHECK(!in_ranges(data, 12));              // the pool is reached (behind the call): code, never data
    CHECK(data.reach_of(12) == apk::data::Reach::weak);
}

TEST(the_same_under_an_untyped_label_on_the_pool) {
    const Words w{{ldr_literal(9, 12), ret, nop, 0x1234'5678, ldr_literal(0, 12), nop, svc0, ret}};
    const auto data = run(w, {0, 24}, {}, {}, {}, {12});  // a probable root on the pool
    CHECK(!in_ranges(data, 24));
    CHECK(!in_ranges(data, 12));
}

TEST(proven_code_that_reads_an_instruction_on_purpose_leaves_it_code) {
    // f loads the svc word at 24, the middle of g (a trusted root): a contradiction leaves the word code.
    const Words w{{ldr_literal(9, 24), ret, nop, nop, nop, nop, svc0, ret}};
    const auto data = run(w, {0, 20});
    CHECK(!in_ranges(data, 24));
    CHECK(data.dropped_reached == 1);
}

TEST(the_leaf_that_only_an_fde_names_is_reached_and_stays_code) {
    // f loads the first word of a leaf `mrs x0, tpidr_el0; ret` that only a root without proof names.
    const Words w{{ldr_literal(9, 16), ret, nop, nop, 0xd53b'd040, ret}};
    const auto data = run(w, {0}, {}, {}, {}, {16});
    CHECK(!in_ranges(data, 16));
    CHECK(data.dropped_reached == 1);
}

TEST(a_root_without_proof_reaches_but_never_loads) {
    // 0: ldr w1, <8>; 4: ret; 8: pool. Run from a probable root only: its load proves nothing.
    const Words w{{ldr_literal(1, 8), ret, 0x1234'5678, ret}};
    CHECK(run(w, {}, {}, {}, {}, {0}).ranges.empty());
    CHECK(run(w, {0}).ranges.size() == 1);
}

TEST(a_load_proves_only_up_to_the_first_barrier) {
    // The load comes behind: a call, a blr, a system call, a trap, a register branch, a reserved word.
    const auto behind = [](std::uint32_t barrier) {
        return run(Words{{barrier, ldr_literal(1, 12), ret, 0x1234'5678, ret}}, {0});
    };
    for (const std::uint32_t barrier : {bl(16), blr(9), svc0, brk1, 0u}) {
        CHECK(behind(barrier).ranges.empty());  // not proven: the pool stays code
    }
    // Control: the same load with a harmless word in front proves the pool.
    CHECK(behind(nop).ranges.size() == 1);
}

TEST(the_branch_targets_of_the_trusted_prefix_are_trusted_and_those_behind_a_barrier_are_not) {
    // cbz x0, <16>; ... 16: ldr w1, <24>; ret; 24: pool. The target is in the trusted prefix: proven.
    const Words ok{{cbz_x0(16), nop, nop, nop, ldr_literal(1, 8), ret, 0x1234'5678, ret}};
    CHECK(run(ok, {0}).ranges.size() == 1);
    // The same behind a call: the target is only reached, its load proves nothing.
    const Words behind{{bl(28), cbz_x0(12), nop, nop, ldr_literal(1, 8), ret, 0x1234'5678, ret}};
    CHECK(run(behind, {0}).ranges.empty());
}

TEST(a_callee_is_not_proven_by_its_caller) {
    // 0: bl <16>; 4: ret; 16: ldr w1, <24>; ret; 24: pool: the callee has no trusted root of its own.
    const Words w{{bl(16), ret, nop, nop, ldr_literal(1, 8), ret, 0x1234'5678, ret}};
    CHECK(run(w, {0}).ranges.empty());
    CHECK(run(w, {0, 16}).ranges.size() == 1);
}

TEST(many_loads_of_one_big_table_cost_linear_time) {
    const auto timed = [](int loads) {
        // cmp w0,#N; b.hi; adr x1, T; ldr w2,[x1,w0,uxtw #2]; repeated; T has N+1 entries.
        Words w;
        const unsigned entries = 4000;
        const auto table = static_cast<int>((loads * 5 + 1) * 4);
        for (int i = 0; i < loads; ++i) {
            const int here = static_cast<int>(w.words.size() * 4);
            w.words.push_back(cmp_w_imm(0, entries - 1));
            w.words.push_back(b_hi(table - (here + 4) + 4));
            w.words.push_back(adr(1, table - (here + 8)));
            w.words.push_back(0xb860'5822);  // ldr w2, [x1, w0, uxtw #2]
            w.words.push_back(nop);
        }
        w.words.push_back(ret);
        for (unsigned i = 0; i < entries; ++i) w.words.push_back(svc_data);
        w.words.push_back(ret);
        return seconds([&] { run(w, {0}); });
    };
    const auto small = timed(500), large = timed(2000);
    CHECK(large < 0.2 || large < 8 * (small + 0.01));
}


TEST(a_literal_of_which_only_the_last_word_is_reached_stays_code) {
    // 0: ldr x9, <8> (8 bytes: words 8 and 12); the word at 12 is a root of its own.
    const Words w{{ldr_literal(9, 8, true), ret, 0x1234'5678, ret, ret}};
    const auto data = run(w, {0, 12});
    CHECK(data.ranges.empty() && data.dropped_reached == 1);
}


TEST(a_leaf_that_only_an_fde_names_is_reached_so_a_load_of_its_first_word_leaves_it_code) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x4200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const unsigned char cie[] = {0x10, 0, 0, 0, 0, 0, 0, 0, 1, 'z', 'R', 0, 0x04, 0x78, 0x1e, 0x01, 0x1b, 0, 0, 0};
    // The FDE covers [0x210, 0x218): pc_begin 0x210 - 0x11c, pc_range 8.
    const unsigned char fde[] = {0x10, 0, 0, 0, 0x18, 0, 0, 0, 0xf4, 0, 0, 0, 0x08, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(builder.bytes.data() + 0x100, cie, sizeof cie);
    std::memcpy(builder.bytes.data() + 0x114, fde, sizeof fde);
    // f (a sized symbol, trusted): ldr w9, <0x210>; ret. The leaf: mrs x0, tpidr_el0; ret.
    const std::uint32_t words[] = {ldr_literal(9, 16), ret, nop, nop, 0xd53b'd040, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    builder.sections({{".eh_frame", sht_progbits, shf_alloc, 0x100, 0x100, 0x28},
                      {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}},
                     {{"f", 0x200, 8}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.ranges.empty());
    const auto sites = apk::container::site_addresses(scan);
    CHECK(std::ranges::find(sites, std::uint64_t{0x210}) != sites.end());  // the thread-pointer read stays a site
}

// An unwind record that cannot be read ends the table, and the FDEs behind it are lost. They are roots that
// reach, so losing them can only make data larger: a table that is not read to its end gives no data range at all.
TEST(an_unreadable_unwind_record_leaves_the_whole_file_as_code) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x4200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const unsigned char cie[] = {0x10, 0, 0, 0, 0, 0, 0, 0, 1, 'z', 'R', 0, 0x04, 0x78, 0x1e, 0x01, 0x1b, 0, 0, 0};
    // A record whose length runs past the section, then the FDE of the leaf [0x210, 0x218) that the load below names.
    const unsigned char broken[] = {0xf0, 0xff, 0xff, 0x7f};
    const unsigned char fde[] = {0x10, 0, 0, 0, 0x28, 0, 0, 0, 0xe4, 0, 0, 0, 0x08, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(builder.bytes.data() + 0x100, cie, sizeof cie);
    std::memcpy(builder.bytes.data() + 0x114, broken, sizeof broken);
    std::memcpy(builder.bytes.data() + 0x120, fde, sizeof fde);
    const std::uint32_t words[] = {ldr_literal(9, 16), ret, nop, nop, 0xd53b'd040, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    builder.sections({{".eh_frame", sht_progbits, shf_alloc, 0x100, 0x100, 0x40},
                      {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}},
                     {{"f", 0x200, 8}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.ranges.empty());
    const auto sites = apk::container::site_addresses(scan);
    CHECK(std::ranges::find(sites, std::uint64_t{0x210}) != sites.end());  // the thread-pointer read stays a site
}

// A record that is read but not understood (a personality encoding the reader cannot decode)
// loses the FDEs that name it, just as one that runs past the section does: the table is not complete.
TEST(a_cie_with_a_personality_the_reader_cannot_decode_leaves_the_whole_file_as_code) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x4200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    // "zP" with the personality encoding 0x08 (no format the reader knows), then the FDE of the leaf [0x210, 0x218).
    const unsigned char cie[] = {0x10, 0, 0, 0, 0, 0, 0, 0, 1, 'z', 'P', 0, 0x04, 0x78, 0x1e, 0x02, 0x08, 0, 0, 0};
    const unsigned char fde[] = {0x10, 0, 0, 0, 0x18, 0, 0, 0, 0xf4, 0, 0, 0, 0x08, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(builder.bytes.data() + 0x100, cie, sizeof cie);
    std::memcpy(builder.bytes.data() + 0x114, fde, sizeof fde);
    const std::uint32_t words[] = {ldr_literal(9, 16), ret, nop, nop, 0xd53b'd040, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    builder.sections({{".eh_frame", sht_progbits, shf_alloc, 0x100, 0x100, 0x28},
                      {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}},
                     {{"f", 0x200, 8}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.ranges.empty());
    const auto sites = apk::container::site_addresses(scan);
    CHECK(std::ranges::find(sites, std::uint64_t{0x210}) != sites.end());  // the thread-pointer read stays a site
}

// The unwind table: each way a record can be unreadable leaves the whole file as code. One library, the
// leaf f = [0x200, 0x208) loads a pool word at 0x210 (a thread-pointer read in disguise); only the table differs.
bool eh_frame_variant_keeps_the_pool_as_code(const unsigned char *eh, std::size_t eh_size) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x4200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    std::memcpy(builder.bytes.data() + 0x100, eh, eh_size);
    const std::uint32_t words[] = {ldr_literal(9, 16), ret, nop, nop, 0xd53b'd040, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    builder.sections({{".eh_frame", sht_progbits, shf_alloc, 0x100, 0x100, 0x28},
                      {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}},
                     {{"f", 0x200, 8}});
    const auto scan = apk::container::scan_code(builder.bytes);
    const auto sites = apk::container::site_addresses(scan);
    return scan.data.ranges.empty() && std::ranges::find(sites, std::uint64_t{0x210}) != sites.end();
}

TEST(a_cie_of_a_version_the_reader_does_not_know_leaves_the_whole_file_as_code) {
    const unsigned char eh[0x28] = {0x10, 0, 0, 0, 0, 0, 0, 0, 2 /* version 2 */};
    CHECK(eh_frame_variant_keeps_the_pool_as_code(eh, sizeof eh));
}

TEST(an_fde_that_names_no_cie_leaves_the_whole_file_as_code) {
    // An FDE at the start of the section whose id points 4 bytes back from its own id field: no CIE is there.
    const unsigned char eh[0x28] = {0x10, 0, 0, 0, 4, 0, 0, 0, 0xf4, 0, 0, 0, 0x08, 0, 0, 0};
    CHECK(eh_frame_variant_keeps_the_pool_as_code(eh, sizeof eh));
}

TEST(an_fde_that_gives_no_range_leaves_the_whole_file_as_code) {
    // A readable CIE "zR" with the encoding "omitted" (0xff): the reader stays fine, the FDE has no start to give.
    unsigned char eh[0x28] = {0x10, 0, 0, 0, 0, 0, 0, 0, 1, 'z', 'R', 0, 0x04, 0x78, 0x1e, 1, 0xff, 0, 0, 0,
                              0x10, 0, 0, 0, 0x18, 0, 0, 0, 0xf4, 0, 0, 0, 0x08, 0, 0, 0, 0, 0, 0, 0};
    CHECK(eh_frame_variant_keeps_the_pool_as_code(eh, sizeof eh));
}

// The .eh_frame clang writes for a C++ function with a destructor (NDK r30, aarch64, -fPIC -fexceptions): its CIE is
// "zPLR" with the personality encoded as indirect, pc-relative, 8 bytes (0x9c). Only the FDE's start is patched, to put
// it at 0x300. The reader must read it whole: the personality is skipped by its size, never applied.
TEST(the_unwind_table_clang_writes_for_c_plus_plus_is_read_and_the_split_works) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x4200);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const unsigned char eh[0x54] = {
        0x1c, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x7a, 0x50, 0x4c, 0x52, 0x00, 0x01, 0x7c, 0x1e, 0x0b, 0x9c, 0x15, 0xc2, 0, 0,
        0, 0, 0, 0, 0x1c, 0x1b, 0x0c, 0x1f, 0x00,
        0x2c, 0, 0, 0, 0x24, 0, 0, 0, 0xd8, 0x01, 0, 0, 0x58, 0, 0, 0, 0x08, 0xaf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0x44, 0x0e, 0x30, 0x48, 0x0c, 0x1d, 0x10, 0x9e, 0x02, 0x9d, 0x04, 0x0a, 0x60, 0x0c, 0x1f, 0x30, 0x48,
        0x0e, 0x00, 0xde, 0xdd, 0x44, 0x0b, 0, 0, 0, 0};
    std::memcpy(builder.bytes.data() + 0x100, eh, sizeof eh);
    const std::uint32_t words[] = {ldr_literal(9, 16), ret, nop, nop, 0xd53b'd040, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    builder.sections({{".eh_frame", sht_progbits, shf_alloc, 0x100, 0x100, sizeof eh},
                      {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}},
                     {{"f", 0x200, 8}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.ranges.size() == 1);  // the pool word at 0x210 is data: the table was read, the split is on
    const auto sites = apk::container::site_addresses(scan);
    CHECK(std::ranges::find(sites, std::uint64_t{0x210}) == sites.end());
}

TEST(an_fde_with_a_pointer_encoding_the_reader_does_not_apply_leaves_the_whole_file_as_code) {
    // "zR" with 0x3b (datarel, sdata4): the reader would take the raw number as an address.
    for (const std::uint8_t encoding : {std::uint8_t{0x3b}, std::uint8_t{0x9b}}) {  // datarel; indirect pcrel
        // The FDE starts at 0x11c + 0x1e4 = 0x300 if the reader applied the pc-relative base, far from the pool.
        unsigned char eh[0x28] = {0x10, 0, 0, 0, 0, 0, 0, 0, 1, 'z', 'R', 0, 0x04, 0x78, 0x1e, 1, encoding, 0, 0, 0,
                                  0x10, 0, 0, 0, 0x18, 0, 0, 0, 0xe4, 0x01, 0, 0, 0x08, 0, 0, 0, 0, 0, 0, 0};
        CHECK(eh_frame_variant_keeps_the_pool_as_code(eh, sizeof eh));
    }
}

// A sized function symbol bounds a strong run: the pool behind a function that ends in a conditional branch is not
// code, even with no unwind table to say where the function ends.
TEST(a_function_symbol_ends_the_strong_run_at_its_size) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}});
    const std::uint32_t words[] = {b_hi(8), ldr_literal(1, 8), ret, svc_data};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}},
                     {{"f", 0x200, 4}});
    const auto scan = apk::container::scan_code(builder.bytes);
    CHECK(scan.data.ranges.empty());
}

// The function DT_INIT or DT_FINI names is proven code like a sized symbol: its literal load proves the pool.
TEST(the_function_that_dt_init_or_dt_fini_names_is_a_trusted_root) {
    using namespace apk::testing;
    for (const std::int64_t tag : {12, 13}) {
        ElfBuilder builder;
        builder.bytes.resize(0x400);
        builder.segments({{pt_load, r | x, 0, 0, 0x400, 0x400, 0x1000}, {pt_dynamic, r, 0x300, 0x300, 0x20, 0x20, 8}});
        builder.dynamic(0x300, {{tag, 0x200}, {0, 0}});
        const std::uint32_t words[] = {ldr_literal(0, 8), ret, svc_data, ret};
        std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
        builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words},
                          {".dynamic", 6 /* SHT_DYNAMIC */, shf_alloc, 0x300, 0x300, 0x20}});
        const auto scan = apk::container::scan_code(builder.bytes);
        CHECK(scan.data.ranges.size() == 1);
        CHECK(scan.data.ranges.size() == 1 && scan.data.ranges[0].begin == 0x208 && scan.data.ranges[0].end == 0x20c);
    }
}

// Hostile section headers must not make the roots grow without bound. Sections that overlap in
// the file make the same bytes roots again and again; a library whose root-bearing sections hold more bytes than the
// file has is no library of ours, and nothing is split.
TEST(sections_that_overlap_in_the_file_give_up_the_split_instead_of_growing_the_roots) {
    using namespace apk::testing;
    const auto library = [](int copies) {
        ElfBuilder builder;
        builder.bytes.resize(0x1000);
        builder.segments({{pt_load, r | x, 0, 0, 0x1000, 0x1000, 0x1000}});
        const std::uint32_t words[] = {ldr_literal(0, 8), ret, svc_data, ret};
        std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
        std::vector<SectionHeader> headers{
            {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}};
        for (int copy = 0; copy < copies; ++copy) {
            headers.push_back({".d", sht_progbits, shf_alloc, 0x800, 0x800, 0x400});
        }
        builder.sections(headers, {{"f", 0x200, 16}});
        return apk::container::scan_code(builder.bytes);
    };
    CHECK(library(1).data.ranges.size() == 1);  // a whole library: the pool is proven data
    CHECK(library(300).data.ranges.empty());    // 300 copies of 1 KiB in a 4 KiB file: nothing is proven
}

TEST(many_sections_that_name_an_init_array_give_up_the_split) {
    using namespace apk::testing;
    const auto library = [](int arrays) {
        ElfBuilder builder;
        builder.bytes.resize(0x1000);
        builder.segments({{pt_load, r | x, 0, 0, 0x1000, 0x1000, 0x1000}});
        const std::uint32_t words[] = {ldr_literal(0, 8), ret, svc_data, ret};
        std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
        std::vector<SectionHeader> headers{
            {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof words}};
        for (int array = 0; array < arrays; ++array) {
            const std::uint64_t at = 0x800 + 8 * static_cast<std::uint64_t>(array);
            headers.push_back({".init_array", sht_progbits, shf_alloc | shf_write, at, at, 8});
        }
        builder.sections(headers, {{"f", 0x200, 16}});
        return apk::container::scan_code(builder.bytes);
    };
    CHECK(library(3).data.ranges.size() == 1);  // the few a real library has
    CHECK(library(200).data.ranges.empty());    // no real library has hundreds
}

// Low points: the authenticated register branch is a dispatch too, and the function ranges do not
// depend on the order in which symbols and unwind records gave them.
TEST(an_authenticated_br_is_an_unresolved_dispatch_too) {
    constexpr std::uint32_t braa_x5_x6 = 0xd71f'08a6;  // braa x5, x6
    const Words w{{ldr_literal(0, 12), braa_x5_x6, ret, svc_data}};
    const auto data = run(w, {0}, {}, {}, {{0, 16}});
    CHECK(data.ranges.empty());
}

TEST(function_ranges_with_one_start_do_not_depend_on_the_order_they_are_given_in) {
    // A symbol [0, 24) and an unwind record [0, 8) start at the same word; `br x5` at 8 may land in the function.
    const Words w{{ldr_literal(0, 20), bl(8), br(5), ret, ret, svc_data}};
    const auto a = run(w, {0}, {}, {}, {{0, 24}, {0, 8}});
    const auto b = run(w, {0}, {}, {}, {{0, 8}, {0, 24}});
    CHECK(a.ranges.size() == b.ranges.size());
    CHECK(a.reached_words == b.reached_words);
    CHECK(a.dropped_dispatch == b.dropped_dispatch);
}

TEST(a_table_that_reached_code_and_trusted_code_both_dispatch_through_is_resolved_by_the_trusted_run) {
    // f (a root) calls X, so what follows its call is only reached; the dispatch there and the one of h (a root of its
    // own) use the same table T. Only the trusted dispatch resolves it, so the case at 72 (`ldr w1, <pool>; ret`) is
    // trusted code and the pool at 80 is data, whichever of the two runs first.
    const Words w{{bl(60),
                   cmp_w_imm(0, 1), b_hi(20), adr(1, 52), ldrsw_indexed, add_x1_x1_x2, br(1), ret,
                   cmp_w_imm(0, 1), b_hi(20), adr(1, 24), ldrsw_indexed, add_x1_x1_x2, br(1), ret,
                   ret, 8, 8, ldr_literal(1, 8), ret, svc_data}};
    for (const auto &roots : {std::vector<std::uint64_t>{0, 32}, std::vector<std::uint64_t>{32, 0}}) {
        CHECK(in_ranges(run(w, roots), 80));  // whichever of the two runs first
    }
}

TEST(a_table_that_is_no_table_poisons_every_function_that_dispatches_through_it) {
    // f1 and f2 each load a pool word and dispatch through the same table, whose entries leave the section.
    const Words w{{ldr_literal(0, 36), cmp_w_imm(0, 1), b_hi(20), adr(1, 68), ldrsw_indexed, add_x1_x1_x2, br(1), ret,
                   nop, svc_data,
                   ldr_literal(0, 36), cmp_w_imm(0, 1), b_hi(20), adr(1, 28), ldrsw_indexed, add_x1_x1_x2, br(1), ret,
                   nop, svc_data,
                   0x1000'0000, 0x1000'0000}};
    const auto data = run(w, {0, 40}, {}, {}, {{0, 40}, {40, 80}});
    CHECK(!in_ranges(data, 36));
    CHECK(!in_ranges(data, 76));
}

// Hostile sizes: executable sections and .eh_frame sections are under the same budget as the others.
// The library: f = `ldr w0, <pool>; ret; pool; ret` at 0x200 and `copies` more sections that give the same bytes again.
apk::data::DataRanges split_of_overlapping_sections(const char *name, std::uint64_t flags, std::uint64_t offset,
                                                    int copies) {
    using namespace apk::testing;
    ElfBuilder builder;
    builder.bytes.resize(0x1000);
    builder.segments({{pt_load, r | x, 0, 0, 0x1000, 0x1000, 0x1000}});
    const std::uint32_t words[] = {ldr_literal(0, 8), ret, svc_data, ret};
    std::memcpy(builder.bytes.data() + 0x200, words, sizeof words);
    std::vector<SectionHeader> headers{{".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, 0x400}};
    for (int copy = 1; copy <= copies; ++copy) {
        const std::uint64_t address = offset + 0x10000 * static_cast<std::uint64_t>(copy);
        headers.push_back({name, sht_progbits, flags, address, offset, 0x400});
    }
    builder.sections(headers, {{"f", 0x200, 16}});
    const auto file = apk::elf::read(builder.bytes);
    const auto sections = apk::elf::read_sections(builder.bytes);
    return apk::data::find(file, builder.bytes, sections, apk::elf::read_symbols(builder.bytes));
}

TEST(executable_sections_that_share_file_bytes_give_up_the_split) {
    using namespace apk::testing;
    CHECK(split_of_overlapping_sections(".t", shf_alloc | shf_execinstr, 0x200, 0).ranges.size() == 1);
    CHECK(split_of_overlapping_sections(".t", shf_alloc | shf_execinstr, 0x200, 300).ranges.empty());
}

TEST(many_eh_frame_sections_over_one_table_give_up_the_split) {
    using namespace apk::testing;
    CHECK(split_of_overlapping_sections(".eh_frame", 0, 0x800, 1).ranges.size() == 1);
    CHECK(split_of_overlapping_sections(".eh_frame", 0, 0x800, 300).ranges.empty());
}

// Hostile tables: a chain of dispatches, each through a table of 4096 byte entries added to a different origin,
// reads that many entries per dispatch; a file may read only so many table entries in all, or nothing is split.
apk::data::DataRanges split_of_dispatch_chain(unsigned sites, bool table_outside_the_code = false) {
    // f = `ldr w1, <pool>; ret; pool; nop`; then, per site (8 words): cmp w0, #4095; b.hi end; adr x1, T; adr x3, next
    // site; ldrb w2, [x1, w0, uxtw]; add x3, x3, x2, lsl #2; br x3; ret. T: 4096 zero bytes, behind the last site.
    std::vector<std::uint32_t> w{ldr_literal(1, 8), ret, svc_data, nop};
    const std::uint32_t table = 16 + 32 * sites + 4 + (table_outside_the_code ? 0x10000 : 0);
    for (unsigned i = 0; i < sites; ++i) {
        const std::uint32_t at = 16 + 32 * i;
        w.insert(w.end(), {cmp_w_imm(0, 4095), b_hi(20), adr(1, static_cast<int>(table - (at + 8))),
                           adr(3, 32 - 12), ldrb_indexed, add_x3_x3_x2_lsl2, br(3), ret});
    }
    w.push_back(ret);
    if (!table_outside_the_code) w.insert(w.end(), 1024, 0);
    std::vector<std::uint64_t> roots{0};
    for (unsigned i = 0; i < sites; ++i) roots.push_back(16 + 32 * i);  // every site is a root of its own
    return run(Words{w}, roots);
}

// A table that lies outside the code (clang keeps AArch64 jump tables in .rodata) is not read, so it
// costs nothing against the cap: a large library with many of them keeps its split.
TEST(tables_that_are_never_read_do_not_count_against_the_cap) {
    CHECK(in_ranges(split_of_dispatch_chain(600, true), 8));
}

TEST(a_chain_of_dispatches_that_read_more_table_entries_than_a_file_may_give_up_the_split) {
    CHECK(in_ranges(split_of_dispatch_chain(100), 8));  // 0.4 million entries: the pool is data
    CHECK(split_of_dispatch_chain(600).ranges.empty());  // 2.4 million: nothing is split
}

// Many dispatches that share one large table must not read it again for each of them.
TEST(many_dispatches_that_share_one_table_read_it_once) {
    const auto time_of = [](unsigned sites, unsigned entries) {
        // Each site: cmp w0, #entries-1; b.hi end; adr x1, T; ldrsw x2, [x1, w0, uxtw #2]; add x1, x1, x2; br x1; ret.
        // T holds `entries` offsets to one case, a ret behind the table.
        std::vector<std::uint32_t> w;
        const std::uint32_t table = 28 * sites, ret_case = table + 4 * entries;
        for (unsigned s = 0; s < sites; ++s) {
            w.insert(w.end(), {cmp_w_imm(0, entries - 1), b_hi(20), adr(1, static_cast<int>(table - (28 * s + 8))),
                               ldrsw_indexed, add_x1_x1_x2, br(1), ret});
        }
        w.insert(w.end(), entries, ret_case - table);
        w.push_back(ret);
        std::vector<std::uint64_t> roots;
        for (unsigned s = 0; s < sites; ++s) roots.push_back(28 * s);
        const Words words{w};
        apk::data::DataRanges data;
        const double elapsed = seconds([&] { data = run(words, roots); });
        CHECK(data.table_words == entries);
        return elapsed;
    };
    const double small = time_of(50, 1024), large = time_of(200, 4096);
    CHECK(large < 8 * small + 0.3);  // once-per-table gives about 4 times, once per dispatch 16 times
    (void)time_of(600, 4096);  // one table of 4096 entries shared by 600 dispatches counts once against the cap
}

// With many executable sections and many data ranges, the masking and the counts must not
// compare every section with every range.
TEST(the_masking_and_the_counts_are_linear_in_sections_plus_ranges) {
    using namespace apk::testing;
    const auto time_of = [](unsigned pools) {
        // Each 16 bytes are a section of their own and a sized function of their own: `ldr w0, <pool>; ret; pool; nop`.
        // So there are as many ranges as sections.
        ElfBuilder builder;
        builder.bytes.resize(0x1000 + 16 * pools + 0x100);
        builder.segments({{pt_load, r | x, 0, 0, builder.bytes.size(), builder.bytes.size(), 0x1000}});
        std::vector<SectionHeader> headers;
        std::vector<std::string> names;
        std::vector<SymbolEntry> symbols;
        names.reserve(pools);
        for (unsigned i = 0; i < pools; ++i) {
            const std::uint32_t words[] = {ldr_literal(0, 8), ret, svc_data, nop};
            std::memcpy(builder.bytes.data() + 0x1000 + 16 * i, words, sizeof words);
            headers.push_back({".t", sht_progbits, shf_alloc | shf_execinstr, 0x1000 + 16u * i, 0x1000 + 16u * i, 16});
            names.push_back("f" + std::to_string(i));
            symbols.push_back({names.back(), 0x1000 + 16u * i, 8});
        }
        builder.sections(headers, symbols);
        std::size_t ranges = 0;
        const double elapsed = seconds([&] {
            const auto scan = apk::container::scan_code(builder.bytes);
            ranges = scan.data.ranges.size();
            (void)apk::container::counts(builder.bytes);
        });
        CHECK(ranges == pools);
        return elapsed;
    };
    const double small = time_of(3000), large = time_of(24000);
    CHECK(large < 24 * small + 0.3);  // linear gives about 8 times, a comparison of all pairs 64 times
}

// Defence in depth for the scan's reads: a grid of hostile section and segment headers, with the outcome of each case
// stated. The ELF reader refuses a section of the file that lies outside it (types other than 0 and NOBITS); the scan
// reads a section that is loaded, executable, not NOBITS, not type 0 and not empty, and only when it lies where its
// segment says, inside the segment's file part, on whole words; every other case is accepted and reads nothing of it.
// ASan is the second oracle: no case reads outside the file.
TEST(a_grid_of_hostile_section_and_segment_headers_has_the_stated_outcome_and_reads_nothing_outside_the_file) {
    using namespace apk::testing;
    const std::uint64_t file_sizes[] = {0x200, 0x400, 0x1000};
    const std::uint64_t file_parts[] = {0, 0x100, 0x400};
    const std::uint64_t memory_sizes[] = {0x400, 0x10000};
    const std::uint32_t types[] = {0, 1, 8};
    const std::uint64_t places[] = {0, 0x100, 0x300, 0x3ff, 0x400, 0x8000, 0xffff'ffff'ffff'ff00ull};
    const std::uint64_t sizes[] = {1, 4, 0x100, 0x1000, 0x10000, 0xffff'ffff'ffff'ffffull};
    const auto fits = [](std::uint64_t offset, std::uint64_t size, std::uint64_t whole) {
        return offset <= whole && size <= whole - offset;
    };
    int runs = 0, refused_cases = 0, accepted_cases = 0;
    for (const auto file_size : file_sizes) {
        for (const auto file_part : file_parts) {
            for (const auto memory_size : memory_sizes) {
                for (const auto type : types) {
                    for (const auto address : places) {
                        for (const auto offset : places) {
                            for (const auto size : sizes) {
                                if (file_part > file_size || file_part > memory_size) continue;
                                ElfBuilder builder;
                                builder.bytes.resize(file_size);
                                builder.segments({{pt_load, r | x, 0, 0, file_part, memory_size, 0x1000}});
                                builder.sections({{".odd", type, shf_alloc | shf_execinstr, address, offset, size}},
                                                 {{"f", 0, 4}});
                                const bool reader_refuses = type == 1 && !fits(offset, size, file_size);
                                const bool scanned = type == 1;  // loaded and executable by construction; size >= 1
                                const bool scan_accepts = offset == address && fits(address, size, file_part) &&
                                                          address % 4 == 0 && size % 4 == 0;
                                const bool expect_refused = reader_refuses || (scanned && !scan_accepts);
                                bool refused = false;
                                try {
                                    apk::container::scan_code(builder.bytes);
                                } catch (const std::exception &) {
                                    refused = true;
                                }
                                CHECK(refused == expect_refused);
                                (refused ? refused_cases : accepted_cases) += 1;
                                ++runs;
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(runs > 1000 && refused_cases > 100 && accepted_cases > 100);
}

}  // namespace

int main() { return apk::testing::run_all(); }
