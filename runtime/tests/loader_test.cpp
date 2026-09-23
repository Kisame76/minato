// Checks the parts of the loader that work on plain memory: relocations and the Linux start stack
// (ADR-0019). Loading real containers is checked end to end by the fixture tests.
#include "loader.h"

#include "check.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using apkrt::loader::Range;

// Linux auxv types (include/uapi/linux/auxvec.h).
constexpr std::uint64_t at_null = 0;
constexpr std::uint64_t at_phdr = 3;
constexpr std::uint64_t at_phent = 4;
constexpr std::uint64_t at_phnum = 5;
constexpr std::uint64_t at_pagesz = 6;
constexpr std::uint64_t at_base = 7;
constexpr std::uint64_t at_entry = 9;
constexpr std::uint64_t at_platform = 15;
constexpr std::uint64_t at_hwcap = 16;
constexpr std::uint64_t at_secure = 23;
constexpr std::uint64_t at_random = 25;
constexpr std::uint64_t at_hwcap2 = 26;
constexpr std::uint64_t at_execfn = 31;

constexpr std::uint32_t pt_load = 1;
constexpr std::uint32_t pt_note = 4;
constexpr std::uint32_t pt_phdr = 6;

struct ProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t vaddr;
    std::uint64_t paddr;
    std::uint64_t file_size;
    std::uint64_t memory_size;
    std::uint64_t alignment;
};
static_assert(sizeof(ProgramHeader) == 56);

TEST(relr_relocates_addresses_and_bitmaps) {
    // 128 words of "image" at ELF address 0x1000; the bias is the host address of ELF address 0.
    std::vector<std::uint64_t> memory(128);
    for (std::size_t index = 0; index < memory.size(); ++index) memory[index] = 0x100 * index;
    const auto bias = reinterpret_cast<std::uint64_t>(memory.data()) - 0x1000;

    // An address entry (0x1008), then a bitmap for the 63 words after it: bits 1 and 3 set, so
    // 0x1010 and 0x1020; then a second bitmap for the next 63 words: bit 1, so 0x1010 + 63 * 8.
    const std::uint64_t table[] = {0x1008, (0b101 << 1) | 1, (0b1 << 1) | 1};
    apkrt::loader::apply_relr(table, bias, Range{0x1000, 0x1400});

    CHECK(memory[0] == 0);                      // untouched
    CHECK(memory[1] == 0x100 + bias);           // 0x1008
    CHECK(memory[2] == 0x200 + bias);           // 0x1010
    CHECK(memory[3] == 0x300);                  // not in the bitmap
    CHECK(memory[4] == 0x400 + bias);           // 0x1020
    CHECK(memory[2 + 63] == 0x100 * 65 + bias);  // 0x1010 + 63 * 8
    CHECK(memory[3 + 63] == 0x100 * 66);
}

TEST(relr_refuses_addresses_outside_the_writable_range) {
    std::vector<std::uint64_t> memory(16);
    const auto bias = reinterpret_cast<std::uint64_t>(memory.data()) - 0x1000;
    const std::uint64_t outside[] = {0x0ff8};
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::loader::apply_relr(outside, bias, Range{0x1000, 0x1080}),
                 "outside the writable");
    const std::uint64_t at_end[] = {0x1080};
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::loader::apply_relr(at_end, bias, Range{0x1000, 0x1080}),
                 "outside the writable");
}

TEST(rela_applies_relative_relocations_only) {
    std::vector<std::uint64_t> memory(4, 0xdead);
    const auto bias = reinterpret_cast<std::uint64_t>(memory.data()) - 0x2000;
    const apkrt::loader::Rela relative[] = {{0x2008, 1027 /* R_AARCH64_RELATIVE */, 0x40}, {0x2010, 0, 0}};
    apkrt::loader::apply_rela(relative, bias, Range{0x2000, 0x2020});
    CHECK(memory[0] == 0xdead);
    CHECK(memory[1] == bias + 0x40);
    CHECK(memory[2] == 0xdead);  // R_AARCH64_NONE

    const apkrt::loader::Rela symbolic[] = {{0x2008, (1ull << 32) | 1025 /* R_AARCH64_GLOB_DAT */, 0}};
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::loader::apply_rela(symbolic, bias, Range{0x2000, 0x2020}),
                 "1025");
}

TEST(rela_resolves_symbols_through_the_resolver) {
    std::vector<std::uint64_t> memory(6, 0xdead);
    const auto bias = reinterpret_cast<std::uint64_t>(memory.data()) - 0x3000;
    // Symbol 1 resolves to 0x5000, symbol 2 is an undefined weak one (0); nothing else exists.
    const auto resolve = [](std::uint32_t symbol) -> std::uint64_t {
        if (symbol == 1) return 0x5000;
        if (symbol == 2) return 0;
        throw apkrt::loader::LoadError("no such symbol");
    };
    const apkrt::loader::Rela entries[] = {
        {0x3000, (1ull << 32) | 257 /* R_AARCH64_ABS64 */, 0x10},
        {0x3008, (1ull << 32) | 1025 /* R_AARCH64_GLOB_DAT */, 0x8},
        {0x3010, (1ull << 32) | 1026 /* R_AARCH64_JUMP_SLOT */, 0},
        {0x3018, (2ull << 32) | 1025 /* R_AARCH64_GLOB_DAT */, 0},
        {0x3020, 1027 /* R_AARCH64_RELATIVE */, 0x40},
    };
    apkrt::loader::apply_rela(entries, bias, Range{0x3000, 0x3030}, resolve);
    CHECK(memory[0] == 0x5010);  // S + A
    CHECK(memory[1] == 0x5008);
    CHECK(memory[2] == 0x5000);
    CHECK(memory[3] == 0);       // undefined weak
    CHECK(memory[4] == bias + 0x40);
    CHECK(memory[5] == 0xdead);

    // Android's linker refuses copy relocations, and so do we; unknown types name themselves.
    const apkrt::loader::Rela copy[] = {{0x3000, (1ull << 32) | 1024 /* R_AARCH64_COPY */, 0}};
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::loader::apply_rela(copy, bias, Range{0x3000, 0x3030}, resolve),
                 "R_AARCH64_COPY");
    const apkrt::loader::Rela unknown[] = {{0x3000, (1ull << 32) | 999, 0}};
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::loader::apply_rela(unknown, bias, Range{0x3000, 0x3030}, resolve),
                 "999");
    // A symbolic relocation outside the writable part is refused before the symbol is looked up.
    const apkrt::loader::Rela outside[] = {{0x2ff8, (3ull << 32) | 257, 0}};
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::loader::apply_rela(outside, bias, Range{0x3000, 0x3030}, resolve),
                 "outside");
}

/// What a Linux program sees at its entry: argc at sp, then argv, envp and auxv.
struct StartView {
    std::uint64_t argc;
    std::vector<std::string> argv;
    std::vector<std::string> envp;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> auxv;

    std::uint64_t aux(std::uint64_t type) const {
        for (const auto &[key, value] : auxv) {
            if (key == type) return value;
        }
        return ~std::uint64_t{0};
    }
};

StartView read_start_stack(std::uint64_t stack_pointer) {
    const auto *words = reinterpret_cast<const std::uint64_t *>(stack_pointer);
    StartView view;
    view.argc = *words++;
    for (std::uint64_t index = 0; index < view.argc; ++index) view.argv.emplace_back(reinterpret_cast<const char *>(*words++));
    CHECK(*words++ == 0);
    while (*words != 0) view.envp.emplace_back(reinterpret_cast<const char *>(*words++));
    ++words;
    for (;;) {
        const auto type = *words++;
        const auto value = *words++;
        view.auxv.emplace_back(type, value);
        if (type == at_null) break;
    }
    return view;
}

TEST(the_start_stack_looks_like_linux_built_it) {
    ProgramHeader headers[] = {
        {pt_phdr, 4, 0x40, 0x40, 0x40, 3 * 56, 3 * 56, 8},
        {pt_load, 5, 0, 0, 0, 0x300, 0x300, 0x4000},
        {pt_note, 4, 0x270, 0x270, 0x270, 0x20, 0x20, 4},
    };
    std::vector<std::byte> stack(64 * 1024);
    const auto bias = std::uint64_t{0x7'0000'0000};
    const apkrt::loader::StartInfo start{
        .arguments = {"/data/app/hello", "--flag"},
        .environment = {"ANDROID_ROOT=/system", "LANG=en_US.UTF-8"},
        .program_headers = std::span(reinterpret_cast<const std::byte *>(headers), sizeof headers),
        .program_header_count = 3,
        .bias = bias,
        .entry = bias + 0x200,
        .static_program = true,
    };
    const auto stack_pointer = apkrt::loader::build_start_stack(start, stack);

    CHECK(stack_pointer % 16 == 0);
    const auto stack_begin = reinterpret_cast<std::uint64_t>(stack.data());
    CHECK(stack_pointer >= stack_begin && stack_pointer < stack_begin + stack.size());

    const auto view = read_start_stack(stack_pointer);
    CHECK(view.argc == 2);
    CHECK(view.argv == (std::vector<std::string>{"/data/app/hello", "--flag"}));
    CHECK(view.envp == (std::vector<std::string>{"ANDROID_ROOT=/system", "LANG=en_US.UTF-8"}));
    CHECK(view.aux(at_pagesz) == 16384);  // ADR-0007
    CHECK(view.aux(at_entry) == bias + 0x200);
    CHECK(view.aux(at_base) == 0);
    CHECK(view.aux(at_phent) == 56);
    CHECK(view.aux(at_phnum) == 3);
    CHECK(view.aux(at_secure) == 0);
    CHECK(view.aux(at_hwcap) == 0b11);  // FP and ASIMD, nothing XNU would not run (no CPUID, SVE, SME)
    CHECK(view.aux(at_hwcap2) == 0);
    CHECK(std::string(reinterpret_cast<const char *>(view.aux(at_platform))) == "aarch64");
    CHECK(std::string(reinterpret_cast<const char *>(view.aux(at_execfn))) == "/data/app/hello");

    // Sixteen random bytes that are not all zero.
    const auto *random = reinterpret_cast<const std::uint8_t *>(view.aux(at_random));
    bool any = false;
    for (int index = 0; index < 16; ++index) any = any || random[index] != 0;
    CHECK(any);

    // A static program gets its program headers at absolute addresses, because the static libc
    // assumes a load bias of 0 (it reads PT_NOTE and PT_TLS at p_vaddr). PT_PHDR points at the copy.
    const auto phdr = view.aux(at_phdr);
    CHECK(phdr % 8 == 0);
    ProgramHeader seen[3];
    std::memcpy(seen, reinterpret_cast<const void *>(phdr), sizeof seen);
    CHECK(seen[0].type == pt_phdr && seen[0].vaddr == phdr && seen[0].paddr == phdr);
    CHECK(seen[1].type == pt_load && seen[1].vaddr == bias && seen[1].offset == 0);
    CHECK(seen[2].type == pt_note && seen[2].vaddr == bias + 0x270 && seen[2].memory_size == 0x20);
}

TEST(a_start_stack_that_does_not_fit_is_refused) {
    std::vector<std::byte> stack(256);
    const apkrt::loader::StartInfo start{
        .arguments = {std::string(1000, 'a')},
        .environment = {},
        .program_headers = {},
        .program_header_count = 0,
        .bias = 0,
        .entry = 0,
        .static_program = false,
    };
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::loader::build_start_stack(start, stack), "start stack");
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
