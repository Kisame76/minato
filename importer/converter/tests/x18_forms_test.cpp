// Two words that use x18 in an encoding no compiler writes, as a library with them in a function: the refusal names
// each one exactly (the word and its kind), so a report from a real import can be compared with it, and the pair
// together.
//  - 0x0c928be6: st2 {v6.2s, v7.2s}, [sp], x18: x18 is the post-index amount and the stack pointer is the base.
//  - 0x49dcdc72: caspalt x28, x29, x18, x19, [x3]: the unprivileged compare-and-swap pair of FEAT_LSUI (Armv9.6), whose
//    second pair is x18, x19; undefined on a CPU without that feature.
#include "apkconv/container.h"

#include "check.h"
#include "elf_builder.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace apk::testing;

constexpr std::uint32_t st2_sp_x18 = 0x0c92'8be6;
constexpr std::uint32_t caspalt_x18 = 0x49dc'dc72;
constexpr std::uint32_t nop = 0xd503'201f;
constexpr std::uint32_t ret = 0xd65f'03c0;
constexpr std::int64_t dt_relr = 36, dt_relrsz = 35, dt_relrent = 37;
constexpr std::uint32_t sht_dynamic = 6, sht_relr = 19;

/// A small static PIE whose code is `words` at 0x200, with the sized function symbol "fn" over all of it, or without
/// any symbol (a stripped library: no function bounds).
struct FdeSpan {
    std::uint32_t begin;  // address
    std::uint32_t range;
};

/// Writes .eh_frame at 0x170 (behind the program headers and the relocation table) with one CIE ("zR", pcrel
/// sdata4) and an FDE for each span.
std::size_t put_eh_frame(ElfBuilder &builder, const std::vector<FdeSpan> &fdes) {
    const unsigned char cie[] = {0x10, 0, 0, 0, 0, 0, 0, 0, 1, 'z', 'R', 0, 0x04, 0x78, 0x1e, 0x01, 0x1b, 0, 0, 0};
    std::memcpy(builder.bytes.data() + 0x170, cie, sizeof cie);
    std::size_t at = 0x170 + sizeof cie;
    for (const auto &fde : fdes) {
        const std::uint32_t cie_pointer = static_cast<std::uint32_t>(at + 4 - 0x170);
        const std::uint32_t pc_begin = fde.begin - static_cast<std::uint32_t>(at + 8);
        const unsigned char head[] = {0x10, 0, 0, 0};
        std::memcpy(builder.bytes.data() + at, head, 4);
        std::memcpy(builder.bytes.data() + at + 4, &cie_pointer, 4);
        std::memcpy(builder.bytes.data() + at + 8, &pc_begin, 4);
        std::memcpy(builder.bytes.data() + at + 12, &fde.range, 4);
        std::memset(builder.bytes.data() + at + 16, 0, 4);
        at += 20;
    }
    return at + 4 - 0x170;  // a zero terminator follows
}

/// A symbol "fn" over [value, value + size) in the text, or none.
struct Symbol {
    std::uint32_t value = 0;
    std::uint32_t size = 0;
};

std::vector<std::byte> library(const std::vector<std::uint32_t> &words, bool with_symbol = true,
                               const std::vector<FdeSpan> &fdes = {}, Symbol symbol = {}) {
    ElfBuilder builder;
    const std::uint64_t size = words.size() * 4;
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
    std::memcpy(builder.bytes.data() + 0x200, words.data(), static_cast<std::size_t>(size));
    const std::size_t eh_size = fdes.empty() ? 0 : put_eh_frame(builder, fdes);
    if (!fdes.empty()) {
        builder.sections({
            {".relr.dyn", sht_relr, shf_alloc, 0x160, 0x160, 8},
            {".eh_frame", sht_progbits, shf_alloc, 0x170, 0x170, eh_size},
            {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, size},
            {".dynamic", sht_dynamic, shf_alloc | shf_write, 0x42c0, 0x2c0, 0x40},
            {".bss", sht_nobits, shf_alloc | shf_write, 0x4300, 0x300, 0xf80},
        }, symbol.size == 0 ? std::initializer_list<SymbolEntry>{}
                            : std::initializer_list<SymbolEntry>{{"inner", symbol.value, symbol.size}});
    } else if (with_symbol) {
        builder.sections({
            {".relr.dyn", sht_relr, shf_alloc, 0x160, 0x160, 8},
            {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, size},
            {".dynamic", sht_dynamic, shf_alloc | shf_write, 0x42c0, 0x2c0, 0x40},
            {".bss", sht_nobits, shf_alloc | shf_write, 0x4300, 0x300, 0xf80},
        }, {{"fn", 0x200, size}});
    } else {
        builder.sections({
            {".relr.dyn", sht_relr, shf_alloc, 0x160, 0x160, 8},
            {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, size},
            {".dynamic", sht_dynamic, shf_alloc | shf_write, 0x42c0, 0x2c0, 0x40},
            {".bss", sht_nobits, shf_alloc | shf_write, 0x4300, 0x300, 0xf80},
        });
    }
    return builder.bytes;
}

std::string refusal_of(const std::vector<std::byte> &elf) {
    try {
        apk::container::convert(elf, {.install_name = "@rpath/x18-forms.dylib"});
    } catch (const apk::container::Incompatible &error) {
        return error.what();
    }
    return {};
}

bool contains(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}

// With the function's bounds from a sized symbol, the converter renames x18 through the whole function and takes the
// store: nothing is refused.
TEST(takes_the_store_with_sp_as_base_and_x18_as_post_index_in_a_function_with_a_sized_symbol) {
    CHECK(refusal_of(library({nop, st2_sp_x18, ret})).empty());
}

// Without a symbol (a stripped library) it has no bounds for the function and refuses the word, as in a real report.
TEST(refuses_the_store_with_sp_as_base_in_a_stripped_library_and_names_the_word) {
    const auto text = refusal_of(library({nop, st2_sp_x18, ret}, false));
    CHECK(contains(text, "x18 at 0x204 cannot run on iOS: it moves the stack pointer, below which the island keeps "
                         "its registers [word 0x0c928be6, stack pointer use]"));
    CHECK(contains(text, "no .symtab"));
}

// FEAT_LSUI's unprivileged compare-and-swap pair is undefined without the feature (SIGILL, nothing written): kept
// as it is.
TEST(keeps_the_unprivileged_compare_and_swap_pair_that_ends_in_x18_and_x19_with_or_without_a_symbol) {
    CHECK(refusal_of(library({nop, caspalt_x18, ret}, true)).empty());
    CHECK(refusal_of(library({nop, caspalt_x18, ret}, false)).empty());
}

// Of the two words in one function the store still decides: it is refused in a stripped library, the pair is not named.
// A stripped library with unwind tables: the FDE is the function's bounds when no sized symbol holds the word, and the
// whole-function rename takes the store as it does with a symbol.
TEST(takes_the_store_with_sp_as_base_in_a_stripped_library_whose_unwind_table_bounds_the_function) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, ret};
    CHECK(refusal_of(library(words, false, {{0x200, 12}})).empty());
}

// ...but a symbol that lies inside the FDE's range and does not hold the word makes the bounds doubtful: the symbol
// wins and nothing bounds the function.
TEST(refuses_the_store_when_a_sized_symbol_lies_inside_the_range_of_its_fde) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, nop, ret};
    const auto text = refusal_of(library(words, false, {{0x200, 16}}, {0x208, 4}));
    CHECK(contains(text, "[word 0x0c928be6, stack pointer use]"));
}

// Two unwind entries that overlap are no function bounds either.
TEST(refuses_the_store_when_two_fdes_overlap) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, nop, ret};
    const auto text = refusal_of(library(words, false, {{0x200, 16}, {0x204, 12}}));
    CHECK(contains(text, "[word 0x0c928be6, stack pointer use]"));
}

// An entry that overlaps another one is no function even when only one of them holds the word.
TEST(refuses_the_store_when_the_fde_that_holds_it_overlaps_another_one) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, nop, ret};
    const auto text = refusal_of(library(words, false, {{0x200, 12}, {0x208, 8}}));
    CHECK(contains(text, "[word 0x0c928be6, stack pointer use]"));
}

// An entry that ends before the function's return is no whole function: the rename would leave the rest on x18.
TEST(refuses_the_store_when_its_fde_ends_before_the_return) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, ret};
    const auto text = refusal_of(library(words, false, {{0x200, 8}}));
    CHECK(contains(text, "[word 0x0c928be6, stack pointer use]"));
}

// An entry that runs past the end of the section is cut at the section's end.
TEST(cuts_the_bounds_at_the_end_of_the_section_when_the_fde_runs_past_it) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, ret};
    CHECK(refusal_of(library(words, false, {{0x200, 0x40}})).empty());
}

// An FDE that does not hold the word gives no bounds.
TEST(refuses_the_store_when_no_fde_holds_it) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, nop, ret};
    const auto text = refusal_of(library(words, false, {{0x208, 8}}));
    CHECK(contains(text, "[word 0x0c928be6, stack pointer use]"));
}

// The rename leaves x30 pointing into a call island, so a function that calls one reading x30 as a value is
// refused. In a stripped library the callee's bounds come from its unwind entry too, as the caller's do.
constexpr std::uint32_t bl_plus_8 = 0x9400'0002;      // bl .+8: from 0x208 to 0x210
constexpr std::uint32_t mov_x9_x30 = 0xaa1e'03e9;     // mov x9, x30: reads the link register as a value

TEST(refuses_the_store_when_a_callee_found_by_its_fde_reads_x30_as_a_value) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, bl_plus_8, ret, mov_x9_x30, ret};
    const auto text = refusal_of(library(words, false, {{0x200, 16}, {0x210, 8}}));
    CHECK(contains(text, "[word 0x0c928be6, stack pointer use]"));
}

TEST(takes_the_store_when_a_callee_found_by_its_fde_does_not_read_x30) {
    const std::vector<std::uint32_t> words = {nop, st2_sp_x18, bl_plus_8, ret, nop, ret};
    CHECK(refusal_of(library(words, false, {{0x200, 16}, {0x210, 8}})).empty());
}

TEST(names_only_the_store_when_both_words_are_in_a_stripped_library) {
    const auto text = refusal_of(library({nop, st2_sp_x18, nop, caspalt_x18, ret}, false));
    CHECK(contains(text, "x18 at 0x204 cannot run on iOS: it moves the stack pointer"));
    CHECK(contains(text, "[word 0x0c928be6, stack pointer use]"));
    CHECK(!contains(text, "0x49dcdc72"));
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
