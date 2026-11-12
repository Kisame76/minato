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
std::vector<std::byte> library(const std::vector<std::uint32_t> &words, bool with_symbol = true) {
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
    if (with_symbol) {
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
