// Checks the parts of the loader that work on plain memory: relocations and the Linux start stack
// (ADR-0019); and on small containers the converter makes, the version check and the island table
// (ADR-0020 X4). Programs are loaded and run end to end by the fixture tests.
#include "hwcap.h"
#include "islands.h"
#include "loader.h"

#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "check.h"
#include "elf_builder.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apkrt::loader::Range;

std::filesystem::path scratch;

// Linux auxv types (include/uapi/linux/auxvec.h).
constexpr std::uint64_t at_null = 0;
constexpr std::uint64_t at_phdr = 3;
constexpr std::uint64_t at_phent = 4;
constexpr std::uint64_t at_phnum = 5;
constexpr std::uint64_t at_pagesz = 6;
constexpr std::uint64_t at_base = 7;
constexpr std::uint64_t at_entry = 9;
constexpr std::uint64_t at_uid = 11;
constexpr std::uint64_t at_euid = 12;
constexpr std::uint64_t at_gid = 13;
constexpr std::uint64_t at_egid = 14;
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
    // ADR-0039 decision 3: the app uid, also as the gid.
    CHECK(view.aux(at_uid) == 10100 && view.aux(at_euid) == 10100);
    CHECK(view.aux(at_gid) == 10100 && view.aux(at_egid) == 10100);
    // The device's CPU features under ADR-0047 (runtime.hwcap checks the table), never CPUID, SVE or SME.
    CHECK(view.aux(at_hwcap) == apkrt::hwcap::guest().hwcap && view.aux(at_hwcap2) == apkrt::hwcap::guest().hwcap2);
    CHECK((view.aux(at_hwcap) & 0b11) == 0b11 && (view.aux(at_hwcap) & (1u << 11 | 1u << 22)) == 0);
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

// The strings lie in order, as Linux's exec copies them (fs/exec.c): argv's from argv[0] on, envp's right after
// them, then AT_EXECFN's. AndroidRuntime::setArgv0 clears the arguments as one block, from argv[0] to the end of
// the last one, and /proc/self/cmdline reads that block.
TEST(the_start_strings_lie_in_order_as_linux_copies_them) {
    std::vector<std::byte> stack(64 * 1024);
    const apkrt::loader::StartInfo start{
        .arguments = {"/system/bin/app_process64", "-Xzygote", "/system/bin", "--application"},
        .environment = {"ANDROID_ROOT=/system", "ANDROID_DATA=/data"},
        .program_headers = {},
        .program_header_count = 0,
        .bias = 0,
        .entry = 0,
        .static_program = false,
    };
    const auto stack_pointer = apkrt::loader::build_start_stack(start, stack);
    const auto *words = reinterpret_cast<const std::uint64_t *>(stack_pointer);
    CHECK(words[0] == 4 && words[5] == 0 && words[8] == 0);
    const std::uint64_t strings[] = {words[1], words[2], words[3], words[4], words[6], words[7],
                                     read_start_stack(stack_pointer).aux(at_execfn)};
    for (std::size_t index = 1; index < std::size(strings); ++index) {
        // Each string starts where the one before it ends.
        const auto *before = reinterpret_cast<const char *>(strings[index - 1]);
        CHECK(strings[index] == strings[index - 1] + std::strlen(before) + 1);
    }
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

/// The container of a static program with a thread pointer read, two uses of x18 and a syscall
/// between them: its stub area holds a thread pointer island, an x18 island, a syscall veneer and
/// another x18 island, in this order. Unsigned.
std::vector<std::byte> x18_program_container() {
    namespace elf = apk::testing;
    constexpr std::uint32_t code[] = {
        0xd53b'd040,  // 0x200 mrs x0, TPIDR_EL0
        0x9100'0640,  // 0x204 add x0, x18, #1
        0xd400'0001,  // 0x208 svc #0
        0xaa13'03f2,  // 0x20c mov x18, x19
        0xd65f'03c0,  // 0x210 ret
    };
    elf::ElfBuilder builder;
    builder.put<std::uint64_t>(24, 0x200);  // e_entry
    builder.segments({{elf::pt_load, elf::r | elf::x, 0, 0, 0x214, 0x214, 0x4000}});
    std::memcpy(builder.bytes.data() + 0x200, code, sizeof code);
    builder.sections({{".text", elf::sht_progbits, elf::shf_alloc | elf::shf_execinstr, 0x200, 0x200, sizeof code}});
    return apk::container::convert(builder.bytes, {.install_name = "@rpath/program.dylib"}).bytes;
}

/// Signs `container` ad hoc and writes it as <name>.dylib into the scratch directory.
std::string write_signed(const std::string &name, std::vector<std::byte> container) {
    apk::codesign::sign(container, {.identifier = name});
    const auto path = scratch / (name + ".dylib");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(container.data()), static_cast<std::streamsize>(container.size()));
    return path.string();
}

TEST(a_container_of_another_version_is_refused) {
    auto container = x18_program_container();
    const std::string_view magic(APK_CONTAINER_MAGIC, sizeof APK_CONTAINER_MAGIC);  // with its zero
    const auto at = std::string_view(reinterpret_cast<const char *>(container.data()), container.size()).find(magic);
    CHECK(at != std::string_view::npos);
    if (at == std::string_view::npos) return;
    const std::uint32_t version = 1;
    std::memcpy(container.data() + at + offsetof(apk_container_info, version), &version, sizeof version);
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::loader::load(write_signed("version-1", std::move(container))),
                 "container version 1, this runtime reads version 2");
}

// ADR-0020 X4: the loader registers the island table of every container it loads, so that a
// signal handler can tell from a pc whether the thread stopped inside an x18 island, and in which.
TEST(the_islands_of_a_loaded_container_are_found_by_pc) {
    const auto image = apkrt::loader::load(write_signed("islands", x18_program_container()));
    const auto header = reinterpret_cast<std::uint64_t>(image.header);
    const auto &info = *image.info;
    CHECK(info.island_count == 2);
    if (info.island_count != 2) return;
    const auto *table = reinterpret_cast<const apk_island *>(header + info.islands);
    const auto *container = apkrt::islands::container_at(header + table[0].stub);
    CHECK(container != nullptr);
    if (container == nullptr) return;
    CHECK(std::string_view(container->name) == "islands");
    for (std::uint32_t index = 0; index < info.island_count; ++index) {
        const auto first = header + table[index].stub, last = first + 4 * (table[index].words - 1u);
        CHECK(apkrt::islands::container_at(first) == container && apkrt::islands::container_at(last) == container);
        CHECK(apkrt::islands::island_at(*container, first) == &table[index]);
        CHECK(apkrt::islands::island_at(*container, last) == &table[index]);
    }
    // The thread pointer island in front and the veneer between the x18 islands are stubs, but no
    // x18 islands. The last x18 island ends the stub area, and the image is no stub.
    CHECK(apkrt::islands::container_at(header + info.stubs) == container);
    CHECK(apkrt::islands::island_at(*container, header + info.stubs) == nullptr);
    CHECK(apkrt::islands::island_at(*container, header + table[0].stub + 4 * table[0].words) == nullptr);
    CHECK(apkrt::islands::container_at(header + info.stubs + info.stubs_size) == nullptr);
    CHECK(apkrt::islands::container_at(header + info.image_offset) == nullptr);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: loader_test <scratch-directory>\n");
        return 2;
    }
    // dyld records the canonical path of a loaded image, and the loader looks images up by path.
    std::filesystem::create_directories(argv[1]);
    scratch = std::filesystem::canonical(argv[1]);
    return apk::testing::run_all();
}
