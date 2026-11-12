// Checks the converter end to end on small ELF files: the container has the layout the loader
// expects (APKRuntime/container_format.h), the code is rewritten exactly at the patterns, dyld
// loads the result, and files that cannot run are refused with a reason.
#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "apkconv/elf.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/container_format.h"
#include "check.h"
#include "elf_builder.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace apk::testing;
using apk::container::Incompatible;

// The islands the converter writes and the runtime that fills the slots must agree on them.
static_assert(apk::rewrite::guest_thread_pointer_offset == APKRT_GUEST_TLS_OFFSET);
static_assert(apk::rewrite::guest_x18_offset == APKRT_GUEST_X18_OFFSET);

std::filesystem::path scratch;

// The code of the test program, at 0x200.
constexpr std::uint32_t code[] = {
    0xd53b'd040,  // 0x200 mrs x0, TPIDR_EL0
    0xf940'1400,  // 0x204 ldr x0, [x0, #40]   (the stack guard load, stays)
    0xd400'0001,  // 0x208 svc #0
    0xd51b'd041,  // 0x20c msr TPIDR_EL0, x1
    0xd53b'd05f,  // 0x210 mrs xzr, TPIDR_EL0  (becomes a nop, no island)
    0xd65f'03c0,  // 0x214 ret
    0xd503'201f,  // 0x218 nop
    0xd503'201f,  // 0x21c nop
};

constexpr std::int64_t dt_rela = 7;
constexpr std::int64_t dt_relasz = 8;
constexpr std::int64_t dt_relaent = 9;
constexpr std::int64_t dt_relrsz = 35;
constexpr std::int64_t dt_relr = 36;
constexpr std::int64_t dt_relrent = 37;
constexpr std::uint32_t sht_dynamic = 6;
constexpr std::uint32_t sht_relr = 19;

/// A small static PIE laid out like the NDK's: code and headers in one r-x segment at 0, data in
/// an rw- segment on its own 16 KiB page, a RELR table with one address in the zero-filled part.
ElfBuilder static_program(std::uint64_t relocated_address = 0x4800,
                          std::initializer_list<SymbolEntry> symbols = {}) {
    ElfBuilder builder;
    builder.put<std::uint64_t>(24, 0x200);  // e_entry
    builder
        .segments({
            {pt_phdr, r, 0x40, 0x40, 5 * 56, 5 * 56, 8},
            {pt_load, r | x, 0, 0, 0x220, 0x220, 0x4000},
            {pt_load, r | w, 0x2c0, 0x42c0, 0x40, 0x1000, 0x4000},
            {pt_dynamic, r | w, 0x2c0, 0x42c0, 0x40, 0x40, 8},
            {pt_gnu_stack, r | w, 0, 0, 0, 0, 0},
        })
        .put<std::uint64_t>(0x160, relocated_address)
        .dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {dt_relrent, 8}, {0, 0}});
    std::memcpy(builder.bytes.data() + 0x200, code, sizeof code);
    builder.sections({
        {".relr.dyn", sht_relr, shf_alloc, 0x160, 0x160, 8},
        {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof code},
        {".dynamic", sht_dynamic, shf_alloc | shf_write, 0x42c0, 0x2c0, 0x40},
        {".bss", sht_nobits, shf_alloc | shf_write, 0x4300, 0x300, 0xf80},
    }, symbols);
    return builder;
}

/// static_program with 4 KiB pages (ADR-0007 decision 2): both LOAD segments aligned to 4 KiB, the rw- one at
/// `writable`, which must lie 0x2c0 into a 4 KiB page, as its file offset does.
ElfBuilder four_kib_program(std::uint64_t writable, std::uint64_t code_section_align = 1) {
    ElfBuilder builder;
    builder.put<std::uint64_t>(24, 0x200);  // e_entry
    builder
        .segments({
            {pt_phdr, r, 0x40, 0x40, 5 * 56, 5 * 56, 8},
            {pt_load, r | x, 0, 0, 0x220, 0x220, 0x1000},
            {pt_load, r | w, 0x2c0, writable, 0x40, 0x1000, 0x1000},
            {pt_dynamic, r | w, 0x2c0, writable, 0x40, 0x40, 8},
            {pt_gnu_stack, r | w, 0, 0, 0, 0, 0},
        })
        .put<std::uint64_t>(0x160, writable + 0x540)
        .dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {dt_relrent, 8}, {0, 0}});
    std::memcpy(builder.bytes.data() + 0x200, code, sizeof code);
    builder.sections({
        {".relr.dyn", sht_relr, shf_alloc, 0x160, 0x160, 8},
        {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof code, code_section_align},
        {".dynamic", sht_dynamic, shf_alloc | shf_write, writable, 0x2c0, 0x40},
        {".bss", sht_nobits, shf_alloc | shf_write, writable + 0x40, 0x300, 0xfc0},
    });
    return builder;
}

apk::container::Container convert(const ElfBuilder &builder) {
    return apk::container::convert(builder.bytes, {.install_name = "@rpath/test-program.dylib"});
}

std::uint32_t word_at(const std::vector<std::byte> &bytes, std::uint64_t offset) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

/// Target of the B instruction at `offset`, or nullopt when it is no B.
std::optional<std::uint64_t> branch_target(const std::vector<std::byte> &bytes, std::uint64_t offset) {
    const auto instruction = word_at(bytes, offset);
    if ((instruction & 0xfc00'0000) != 0x1400'0000) return std::nullopt;
    const auto immediate = static_cast<std::int64_t>(static_cast<std::int32_t>(instruction << 6) >> 6);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(offset) + 4 * immediate);
}

/// The metadata, read from the file the way dyld's getsectiondata would find it.
apk_container_info read_info(const std::vector<std::byte> &bytes) {
    mach_header_64 header;
    std::memcpy(&header, bytes.data(), sizeof header);
    std::size_t at = sizeof header;
    for (std::uint32_t index = 0; index < header.ncmds; ++index) {
        load_command command;
        std::memcpy(&command, bytes.data() + at, sizeof command);
        if (command.cmd == LC_SEGMENT_64) {
            segment_command_64 segment;
            std::memcpy(&segment, bytes.data() + at, sizeof segment);
            for (std::uint32_t section_index = 0; section_index < segment.nsects; ++section_index) {
                section_64 section;
                std::memcpy(&section, bytes.data() + at + sizeof segment + section_index * sizeof section,
                            sizeof section);
                if (std::strncmp(section.sectname, APK_CONTAINER_SECTION, sizeof section.sectname) == 0) {
                    apk_container_info info;
                    std::memcpy(&info, bytes.data() + section.offset, sizeof info);
                    return info;
                }
            }
        }
        at += command.cmdsize;
    }
    throw std::runtime_error("no __apk_info section");
}

TEST(converts_a_static_program_into_the_documented_layout) {
    const auto container = convert(static_program());
    const auto &bytes = container.bytes;
    const auto info = read_info(bytes);

    CHECK(std::strcmp(info.magic, APK_CONTAINER_MAGIC) == 0);
    CHECK(info.version == APK_CONTAINER_VERSION);
    CHECK(info.flags == APK_CONTAINER_STATIC);
    CHECK(info.image_offset % 0x4000 == 0);
    CHECK(info.min_vaddr == 0);
    CHECK(info.image_size == 0x8000);  // up to the end of .bss (0x52c0), page aligned
    CHECK(info.entry == 0x200);
    CHECK(info.phdr == 0x40);
    CHECK(info.phnum == 5);
    CHECK(info.phentsize == 56);
    CHECK(info.writable_start == 0x4000);
    CHECK(info.writable_end == 0x8000);
    CHECK(info.runtime_slots == info.image_offset + 0x8000);  // the page after the image
    CHECK(info.thread_pointer_reads == 2);
    CHECK(info.thread_pointer_writes == 1);
    CHECK(info.syscalls == 1);

    CHECK(container.report.is_static);
    CHECK(container.report.code_bytes == sizeof code);
    CHECK(container.report.relocations == 1);
    CHECK(container.report.stub_bytes == 16 + 24 + 24);

    // The ELF image keeps its layout: header at the image start, data at the same distance.
    const auto image = info.image_offset;
    CHECK(std::memcmp(bytes.data() + image, "\x7f" "ELF", 4) == 0);
    std::int64_t tag;
    std::memcpy(&tag, bytes.data() + image + 0x42c0, sizeof tag);
    CHECK(tag == dt_relr);

    // Exactly the patterns changed: branches into the stub area, which lies before the image.
    for (const std::uint64_t site : {0x200u, 0x208u, 0x20cu}) {
        const auto target = branch_target(bytes, image + site);
        CHECK(target.has_value());
        if (target) CHECK(*target < image && *target >= sizeof(mach_header_64));
    }
    CHECK(word_at(bytes, image + 0x204) == code[1]);
    CHECK(word_at(bytes, image + 0x210) == 0xd503'201f);
    CHECK(word_at(bytes, image + 0x214) == code[5]);
    if (const auto island = branch_target(bytes, image + 0x200)) {
        CHECK(word_at(bytes, *island) == 0xd53b'd060);  // mrs x0, TPIDRRO_EL0
        CHECK(branch_target(bytes, *island + 12) == image + 0x204);
    }
    if (const auto veneer = branch_target(bytes, image + 0x208)) {
        CHECK(word_at(bytes, *veneer) == 0xa9bf'7bf0);  // stp x16, x30, [sp, #-16]!
        CHECK(branch_target(bytes, *veneer + 20) == image + 0x20c);
    }
}

TEST(the_same_file_gives_the_same_container) {
    CHECK(convert(static_program()).bytes == convert(static_program()).bytes);
    CHECK(convert(static_program()).bytes != convert(static_program(0x4808)).bytes);
}

TEST(dyld_loads_the_container_with_its_writable_slots) {
    auto container = convert(static_program());
    apk::codesign::sign(container.bytes, {.identifier = "test-program"});
    const auto path = scratch / "test-program.dylib";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(container.bytes.data()),
                   static_cast<std::streamsize>(container.bytes.size()));
    }

    void *const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        apk::testing::fail(std::source_location::current(), "dlopen failed: ", dlerror());
        return;
    }
    const mach_header_64 *header = nullptr;
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        if (path == _dyld_get_image_name(index)) {
            header = reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(index));
        }
    }
    CHECK(header != nullptr);
    if (header == nullptr) return;

    unsigned long size = 0;
    const auto *info = reinterpret_cast<const apk_container_info *>(
        getsectiondata(header, APK_CONTAINER_SEGMENT, APK_CONTAINER_SECTION, &size));
    CHECK(info != nullptr && size == sizeof(apk_container_info));
    if (info == nullptr) return;
    // Signing adds load commands behind the existing ones; the metadata must survive that.
    CHECK(std::strcmp(info->magic, APK_CONTAINER_MAGIC) == 0);
    CHECK(info->version == APK_CONTAINER_VERSION);

    const auto base = reinterpret_cast<std::uintptr_t>(header);
    auto *slots = reinterpret_cast<volatile apk_runtime_slots *>(base + info->runtime_slots);
    CHECK(slots->syscall_entry == 0);
    slots->syscall_entry = 0x1234;
    CHECK(slots->syscall_entry == 0x1234);

    // The zero-filled part of the data segment (.bss) reads as zero and is writable.
    auto *bss = reinterpret_cast<volatile std::uint64_t *>(base + info->image_offset + 0x4800);
    CHECK(*bss == 0);
    *bss = 7;
    CHECK(*bss == 7);
    dlclose(handle);
}

TEST(a_program_that_needs_libraries_is_not_static) {
    auto builder = static_program();
    builder.dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {1 /* DT_NEEDED */, 0}, {0, 0}});
    const auto container = convert(builder);
    CHECK(!container.report.is_static);
    CHECK(read_info(container.bytes).flags == 0);
}

TEST(runs_x18_uses_in_islands) {
    auto builder = static_program();
    builder.put<std::uint32_t>(0x218, 0xaa13'03f2);  // mov x18, x19
    builder.put<std::uint32_t>(0x21c, 0xa949'4c12);  // ldp x18, x19, [x0, #0x90]
    const auto container = convert(builder);
    CHECK(container.report.x18_islands == 2);
    CHECK(container.report.x18_writes == 2);
    CHECK(container.report.x18_saves == 0);
    CHECK(container.report.stub_bytes == 16 + 24 + 24 + 2 * 36);  // two islands of 9 words that only write x18
    const auto image = read_info(container.bytes).image_offset;
    for (const std::uint64_t site : {0x218u, 0x21cu}) {
        const auto island = branch_target(container.bytes, image + site);
        CHECK(island.has_value());
        if (!island) continue;
        CHECK(*island < image && *island >= sizeof(mach_header_64));
        CHECK(branch_target(container.bytes, *island + 32) == image + site + 4);
    }
    CHECK(convert(static_program()).report.x18_islands == 0);

    // ShadowCallStack reads x18 as well; under ADR-0019 that made the file incompatible.
    auto shadow_call_stack = static_program();
    shadow_call_stack.put<std::uint32_t>(0x218, 0xf800'865e);  // str x30, [x18], #8
    shadow_call_stack.put<std::uint32_t>(0x21c, 0x9100'0640);  // add x0, x18, #1
    CHECK(convert(shadow_call_stack).report.x18_islands == 2);
}

// A function with a jump table through x18, as clang 8 builds one (ADR-0020 X2d), at 0x200:
// f(i) returns 10 + i for i from 0 to 3 and -1 otherwise. Its table of 32-bit offsets is at 0x280.
constexpr std::uint32_t jump_table_code[] = {
    0xaa00'03f2,               // 0x200 mov x18, x0
    0xf100'0e5f,               // 0x204 cmp x18, #3
    0x5400'01c8,               // 0x208 b.hi 0x240
    0x9000'0009,               // 0x20c adrp x9, 0x0
    0x910a'0129,               // 0x210 add x9, x9, #0x280
    0xb8b2'7932,               // 0x214 ldrsw x18, [x9, x18, lsl #2]
    0x8b09'0252,               // 0x218 add x18, x18, x9
    0xd61f'0240,               // 0x21c br x18
    0xd280'0140, 0xd65f'03c0,  // 0x220 mov x0, #10; ret
    0xd280'0160, 0xd65f'03c0,  // 0x228 mov x0, #11; ret
    0xd280'0180, 0xd65f'03c0,  // 0x230 mov x0, #12; ret
    0xd280'01a0, 0xd65f'03c0,  // 0x238 mov x0, #13; ret
    0x9280'0000, 0xd65f'03c0,  // 0x240 mov x0, #-1; ret
};
constexpr std::int32_t jump_table[] = {0x220 - 0x280, 0x228 - 0x280, 0x230 - 0x280, 0x238 - 0x280};

ElfBuilder jump_table_program(std::uint64_t table_section_flags = shf_alloc) {
    ElfBuilder builder;
    builder.put<std::uint64_t>(24, 0x200);  // e_entry
    builder
        .segments({
            {pt_phdr, r, 0x40, 0x40, 4 * 56, 4 * 56, 8},
            {pt_load, r | x, 0, 0, 0x290, 0x290, 0x4000},
            {pt_load, r | w, 0x2c0, 0x42c0, 0x40, 0x40, 0x4000},
            {pt_dynamic, r | w, 0x2c0, 0x42c0, 0x40, 0x40, 8},
        })
        .dynamic(0x2c0, {{0, 0}});
    std::memcpy(builder.bytes.data() + 0x200, jump_table_code, sizeof jump_table_code);
    std::memcpy(builder.bytes.data() + 0x280, jump_table, sizeof jump_table);
    builder.sections({
        {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, sizeof jump_table_code},
        {".rodata", sht_progbits, table_section_flags, 0x280, 0x280, sizeof jump_table},
        {".dynamic", sht_dynamic, shf_alloc | shf_write, 0x42c0, 0x2c0, 0x40},
    });
    return builder;
}

/// Darwin TSD slot 11 of the calling thread, where the islands keep the guest's x18 (ADR-0020).
volatile std::uint64_t &guest_x18_slot() {
    std::uint64_t base;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(base));
    return reinterpret_cast<volatile std::uint64_t *>(base & ~std::uint64_t{7})[11];
}

// The whole jump runs on the Mac: mov, cmp, ldrsw and add on x18 in their islands, then the br
// through a landing pad to the case, which returns to this test.
TEST(runs_a_jump_table_through_x18_with_landing_pads) {
    auto container = convert(jump_table_program());
    CHECK(container.report.x18_islands == 5);  // four single instructions and the br
    apk::codesign::sign(container.bytes, {.identifier = "jump-table-program"});
    const auto path = scratch / "jump-table-program.dylib";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(container.bytes.data()),
                   static_cast<std::streamsize>(container.bytes.size()));
    }
    void *const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        apk::testing::fail(std::source_location::current(), "dlopen failed: ", dlerror());
        return;
    }
    std::uintptr_t base = 0;
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        if (path == _dyld_get_image_name(index)) base = reinterpret_cast<std::uintptr_t>(_dyld_get_image_header(index));
    }
    CHECK(base != 0);
    if (base == 0) return;

    const auto image = base + read_info(container.bytes).image_offset;
    const auto function = reinterpret_cast<std::int64_t (*)(std::int64_t)>(image + 0x200);
    auto &x18 = guest_x18_slot();
    const auto saved = x18;
    for (std::int64_t index = 0; index < 4; ++index) {
        CHECK(function(index) == 10 + index);
        CHECK(x18 == image + 0x220 + 8 * static_cast<std::uint64_t>(index));  // the target it computed
    }
    CHECK(function(7) == -1);
    CHECK(x18 == 7);  // no jump: x18 still holds the index
    x18 = saved;
    dlclose(handle);
}

/// The island table of a converted container, after the checks every table must pass (ADR-0020
/// X4): one entry per x18 island, between __apk_info and the stubs, sorted by address and without
/// overlaps, each island inside the stub area and its site a branch to it.
std::vector<apk_island> island_table(const apk::container::Container &container,
                                     std::source_location location = std::source_location::current()) {
    const auto info = read_info(container.bytes);
    const auto &report = container.report;
    if (info.island_count != report.x18_islands) fail(location, "the table has not one entry per x18 island");
    if (report.island_table_bytes != std::uint64_t{info.island_count} * sizeof(apk_island)) {
        fail(location, "the report gives another size of the table");
    }
    if (info.stubs_size != report.stub_bytes || info.stubs + info.stubs_size > info.image_offset) {
        fail(location, "the stub area is not where the report says");
    }
    if (info.islands % 8 != 0 || info.islands + report.island_table_bytes > info.stubs) {
        fail(location, "the table is not between __apk_info and the stubs");
        return {};
    }
    std::vector<apk_island> islands(info.island_count);
    std::memcpy(islands.data(), container.bytes.data() + info.islands, islands.size() * sizeof(apk_island));
    std::uint64_t end = info.stubs;
    for (const auto &island : islands) {
        if (island.stub < end) fail(location, "the entries overlap or are not sorted");
        end = island.stub + 4 * std::uint64_t{island.words};
        if (end > info.stubs + info.stubs_size) fail(location, "an island reaches beyond the stub area");
        if (branch_target(container.bytes, island.site) != island.stub) fail(location, "a site does not branch to its island");
    }
    return islands;
}

/// The (c) R-range table (apk_c_range), read the way the loader finds it from __apk_info (ADR-0020 v6).
std::vector<apk_c_range> c_range_table(const apk::container::Container &container) {
    const auto info = read_info(container.bytes);
    std::vector<apk_c_range> ranges(info.c_range_count);
    if (info.c_range_count != 0) {
        std::memcpy(ranges.data(), container.bytes.data() + info.c_ranges, ranges.size() * sizeof(apk_c_range));
    }
    return ranges;
}

// ADR-0020 X4: the island table has one entry per x18 island, sorted by address, with what the
// runtime needs to send a thread that stopped inside the island back to the guest.
TEST(the_island_table_describes_every_x18_island) {
    auto builder = static_program();
    builder.put<std::uint32_t>(0x218, 0x9100'0640);  // add x0, x18, #1: reads x18
    builder.put<std::uint32_t>(0x21c, 0xaa13'03f2);  // mov x18, x19: writes x18
    const auto singles = convert(builder);
    const auto image = read_info(singles.bytes).image_offset;
    CHECK(read_info(singles.bytes).version == APK_CONTAINER_VERSION);
    const auto islands = island_table(singles);
    CHECK(islands.size() == 2);
    if (islands.size() == 2) {
        // add x0, x18, #1: stur x1; mrs, bic, ldr x1; add x0, x1, #1; ldur x1; b
        CHECK(islands[0].site == image + 0x218);
        CHECK(islands[0].kind == APK_ISLAND_SINGLE && islands[0].words == 7 && islands[0].value == 1 &&
              islands[0].base == APK_ISLAND_NO_REGISTER && islands[0].commit == 4 && islands[0].store == 0 &&
              islands[0].resume == 1);
        // mov x18, x19: stur x0; stur x1; mov x0, x19; mrs, bic, str x0; ldur x0; ldur x1; b
        CHECK(islands[1].site == image + 0x21c);
        CHECK(islands[1].kind == APK_ISLAND_SINGLE && islands[1].words == 9 && islands[1].value == 0 &&
              islands[1].base == 1 && islands[1].commit == 2 && islands[1].store == 5 && islands[1].resume == 1);
    }
    CHECK(island_table(convert(static_program())).empty());  // no x18, no entries

    // An LL/SC loop on x18 as the address: one entry for the whole window.
    auto loop = static_program();
    loop.put<std::uint32_t>(0x200, 0xc85f'7e48);  // ldxr x8, [x18]
    loop.put<std::uint32_t>(0x204, 0x9100'0508);  // add x8, x8, #1
    loop.put<std::uint32_t>(0x208, 0xc809'7e48);  // stxr w9, x8, [x18]
    loop.put<std::uint32_t>(0x20c, 0x35ff'ffa9);  // cbnz w9, 0x200
    const auto loop_container = convert(loop);
    const auto window = island_table(loop_container);
    CHECK(window.size() == 1);
    if (window.size() == 1) {
        // stur x0; mrs, bic, ldr x0; ldxr x8, [x0]; add x8, x8, #1; stxr w9, x8, [x0]; ldur x0; b
        // commit is the preamble length (4 words: stur, mrs, bic, ldr) for a resumable window (X4h).
        CHECK(window[0].site == read_info(loop_container.bytes).image_offset + 0x200);
        CHECK(window[0].kind == APK_ISLAND_EXCLUSIVE && window[0].words == 9 && window[0].value == 0 &&
              window[0].base == APK_ISLAND_NO_REGISTER && window[0].commit == 4 && window[0].store == 0 &&
              window[0].resume == 3);
    }

    // The jump table: four single instructions, then the br with the landing pads of 0x220 to 0x23c.
    const auto jump_container = convert(jump_table_program());
    const auto jump = island_table(jump_container);
    CHECK(jump.size() == 5);
    if (jump.size() == 5) {
        for (std::size_t index = 0; index < 4; ++index) CHECK(jump[index].kind == APK_ISLAND_SINGLE);
        CHECK(jump[4].site == read_info(jump_container.bytes).image_offset + 0x21c);
        CHECK(jump[4].kind == APK_ISLAND_JUMP_TABLE && jump[4].words == 16 + 4 * 8 && jump[4].value == 0 &&
              jump[4].base == 1 && jump[4].commit == APK_ISLAND_NO_COMMIT && jump[4].store == 0);
    }
}

// (b) ADR-0020 C7: an in-sequence x18 use on a live LL/SC path (the back-edge shape: the store-exclusive
// before its load-exclusive, a backward branch) that no whole-window island took becomes a no-save island
// over a register proven dead. The container records it as APK_ISLAND_SINGLE_NOSAVE, and a container that
// holds one needs a v5 runtime (APK_CONTAINER_RECONSTRUCT_VERSION).
TEST(the_container_records_a_no_save_island_at_version_7) {
    auto builder = static_program();
    builder.put<std::uint32_t>(0x200, 0x1400'0003);  // b 0x20c
    builder.put<std::uint32_t>(0x204, 0xc802'7c03);  // stxr w2, x3, [x0]: the store-exclusive, before its ldxr
    builder.put<std::uint32_t>(0x208, 0x3400'00a2);  // cbz w2, 0x21c
    builder.put<std::uint32_t>(0x20c, 0xc85f'7c01);  // ldxr x1, [x0]
    builder.put<std::uint32_t>(0x210, 0x8b12'0023);  // add x3, x1, x18: the in-sequence x18 read use
    builder.put<std::uint32_t>(0x214, 0xd280'0005);  // mov x5, #0: x5 dead after the use (the value register)
    builder.put<std::uint32_t>(0x218, 0x17ff'fffb);  // b 0x204
    builder.put<std::uint32_t>(0x21c, 0xd503'201f);  // nop
    const auto container = convert(builder);
    const auto info = read_info(container.bytes);
    CHECK(info.version == APK_CONTAINER_VERSION);
    CHECK(info.version >= APK_CONTAINER_RECONSTRUCT_VERSION);  // the kind needs a reconstruction-capable runtime
    const auto islands = island_table(container);
    CHECK(islands.size() == 1);
    if (islands.size() == 1) {
        // mrs, bic, ldr x5 (the guest x18); add x3, x1, x5; b -- no red-zone save, no store-back
        CHECK(islands[0].site == info.image_offset + 0x210);
        CHECK(islands[0].kind == APK_ISLAND_SINGLE_NOSAVE && islands[0].words == 5 && islands[0].value == 5 &&
              islands[0].base == APK_ISLAND_NO_REGISTER && islands[0].commit == 3 && islands[0].store == 0 &&
              islands[0].resume == 1);
    }
}

// ADR-0020 option (c) step 4: a sized function with a caller-saved register free across it, holding a hard x18 use no
// island takes (here `ret x18`, a register return), is no longer refused. convert() renames x18 to that register
// across the whole function and records the R-range where the guest x18 lives there instead of in TSD slot 11. The
// function holds no island construct, so (c) and the islands never rewrite the same site.
TEST(renames_x18_across_a_hard_function_instead_of_refusing_it) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x20}});  // fn = [0x200, 0x220)
    prog.put<std::uint32_t>(0x200, 0xd503'201f);  // nop: was mrs TPIDR_EL0 -- remove the thread-pointer island
    prog.put<std::uint32_t>(0x208, 0xd503'201f);  // nop: was svc -- remove the syscall veneer
    prog.put<std::uint32_t>(0x20c, 0xd503'201f);  // nop: was msr TPIDR_EL0 -- remove the thread-pointer island
    prog.put<std::uint32_t>(0x210, 0xd503'201f);  // nop: was mrs xzr, TPIDR_EL0 -- no thread-pointer-read site at all
    prog.put<std::uint32_t>(0x214, 0xd503'201f);  // nop: was ret -- a single exit, at the ret x18
    prog.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18: a register return reading x18 -- refused, and no island
    const auto container = convert(prog);         // before step 4 this throws Incompatible
    const auto info = read_info(container.bytes);
    CHECK(info.version == APK_CONTAINER_VERSION);
    const auto ranges = c_range_table(container);
    CHECK(info.c_range_count == ranges.size() && !ranges.empty());
    // The body after the entry island keeps the guest x18 in the free caller-saved register x9.
    bool body = false;
    for (const auto &range : ranges) {
        if (range.start == info.image_offset + 0x204 && range.end == info.image_offset + 0x220 && range.reg == 9) {
            body = true;
        }
    }
    CHECK(body);
    // The hard x18 site is patched to a branch into its sync island, not left as `ret x18`.
    CHECK(branch_target(container.bytes, info.image_offset + 0x21c).has_value());
    // The table is sorted by start (apk_c_range): the signal path binary-searches it. A function's body range (in
    // the image) and its sync-island stub ranges (in the stub area, below the image) come out of emission unsorted,
    // so the converter sorts the whole table. With fewer than two ranges the order would be trivially right.
    CHECK(ranges.size() >= 2);
    CHECK(std::is_sorted(ranges.begin(), ranges.end(),
                         [](const apk_c_range &a, const apk_c_range &b) { return a.start < b.start; }));
}

// ADR-0020 option (c) step A1: a (c) function may hold ISLANDABLE x18 uses too, not only hard ones. Here a single
// `add x0,x0,x18` (an x18 island on its own) sits next to a hard `ret x18`. The whole-function rename converts every
// x18 use in the body to the free register, so convert() emits NO x18 island inside the function and
// keep_c_functions_clear_of_islands no longer drops it for one. Before A1 the overlap filter dropped the function
// for the add's island and the whole file was refused.
TEST(takes_a_function_with_a_mix_of_islandable_and_hard_x18_uses_and_islands_none_of_them) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x20}});  // fn = [0x200, 0x220)
    prog.put<std::uint32_t>(0x200, 0xd503'201f);  // nop: was mrs TPIDR_EL0 -- remove the thread-pointer island
    prog.put<std::uint32_t>(0x208, 0xd503'201f);  // nop: was svc -- remove the syscall veneer
    prog.put<std::uint32_t>(0x20c, 0xd503'201f);  // nop: was msr TPIDR_EL0 -- remove the thread-pointer island
    prog.put<std::uint32_t>(0x210, 0xd503'201f);  // nop: was mrs xzr, TPIDR_EL0
    prog.put<std::uint32_t>(0x214, 0xd503'201f);  // nop: was ret -- the single exit is the ret x18
    prog.put<std::uint32_t>(0x218, 0x8b12'0000);  // add x0, x0, x18: an ISLANDABLE single x18 read -- today an island
    prog.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18: the HARD x18 use (register return), no island
    const auto container = convert(prog);         // before A1 keep-clear drops fn for the add's island -> Incompatible
    const auto info = read_info(container.bytes);
    CHECK(info.version == APK_CONTAINER_VERSION);
    // (c) took the function: a body range covers the islandable site, with the rename register.
    std::optional<std::uint8_t> reg;
    for (const auto &range : c_range_table(container)) {
        if (range.start <= info.image_offset + 0x218 && info.image_offset + 0x218 < range.end) reg = range.reg;
    }
    CHECK(reg.has_value());
    // The islandable add was RENAMED in place (x18 -> the rename register), not turned into a branch to an island.
    if (reg) CHECK(word_at(container.bytes, info.image_offset + 0x218) == (0x8b00'0000u | (std::uint32_t{*reg} << 16)));
    // No x18 island at all: the rename handled every x18 use of the function.
    CHECK(island_table(container).empty());
}

// ADR-0020 option (c) step A1, the real-world construct: a closed LL/SC window on x18 (islandable on its own, an
// exclusive_region) inside a (c) function. The whole-function rename turns its ldxr/stxr x18 into the free register,
// so the window runs natively on a kernel-preserved register and no exclusive-window island is emitted. This is the
// cluster Unity's protected code has (a hard x18 use next to an exclusive window); before A1 it was refused.
TEST(takes_a_function_with_a_closed_exclusive_window_and_a_hard_x18_use_without_an_island) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x20}});  // fn = [0x200, 0x220)
    prog.put<std::uint32_t>(0x200, 0xd503'201f);  // nop: was mrs TPIDR_EL0
    prog.put<std::uint32_t>(0x204, 0xd503'201f);  // nop: was the stack-guard load
    prog.put<std::uint32_t>(0x208, 0xc85f'7c52);  // ldxr x18, [x2]       : the window opens (x18 the loaded value)
    prog.put<std::uint32_t>(0x20c, 0x9100'0652);  // add  x18, x18, #1
    prog.put<std::uint32_t>(0x210, 0xc803'7c52);  // stxr w3, x18, [x2]   : the window closes -> islandable, not hard
    prog.put<std::uint32_t>(0x214, 0x35ff'ffa3);  // cbnz w3, 0x208       : the normal LL/SC retry (an internal edge)
    prog.put<std::uint32_t>(0x218, 0xd503'201f);  // nop
    prog.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18              : the HARD x18 use (register return)
    const auto container = convert(prog);         // before A1 keep-clear drops fn for the window's island -> refused
    const auto info = read_info(container.bytes);
    CHECK(info.version == APK_CONTAINER_VERSION);
    std::optional<std::uint8_t> reg;
    for (const auto &range : c_range_table(container)) {
        if (range.start <= info.image_offset + 0x208 && info.image_offset + 0x208 < range.end) reg = range.reg;
    }
    CHECK(reg.has_value());
    // The window's ldxr/stxr were renamed in place to the free register (Rt field), not relocated into an island.
    if (reg) {
        // ldxr <reg>, [x2] and stxr w3, <reg>, [x2]
        CHECK(word_at(container.bytes, info.image_offset + 0x208) == (0xc85f'7c40u | std::uint32_t{*reg}));
        CHECK(word_at(container.bytes, info.image_offset + 0x210) == (0xc803'7c40u | std::uint32_t{*reg}));
        // and the add x18, x18, #1 between them became add <reg>, <reg>, #1
        CHECK(word_at(container.bytes, info.image_offset + 0x20c) ==
              (0x9100'0400u | (std::uint32_t{*reg} << 5) | std::uint32_t{*reg}));
    }
    CHECK(island_table(container).empty());  // no exclusive-window island inside the (c) function
}

// ADR-0020 option (c) step A1b: a (c) function absorbs an exclusive window only when the window's WHOLE span lies
// inside it. A window that merely overlaps the function -- starting before begin and running in, or starting inside
// and running past end -- would leave its outside words on physical x18 while the whole-function rename turns its
// inside words into the register, and no island can span both halves. keep_c_functions_clear_of_islands drops such a
// function and leaves its x18 site refused. scan() cannot build a window straddling a taken function (the gate needs
// a hard transfer at begin-4 and end-4, which a window word can never be), so the straddling region is injected here
// to check the defensive drop directly -- the way c_rename_plan is unit-tested, not through convert().
TEST(drops_a_c_function_an_exclusive_window_straddles) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // fn = [0x200, 0x218)
    prog.put<std::uint32_t>(0x200, 0xd503'201f);  // nop: was mrs TPIDR_EL0 -- no thread-pointer island inside fn
    prog.put<std::uint32_t>(0x208, 0xd503'201f);  // nop: was svc           -- no syscall veneer inside fn
    prog.put<std::uint32_t>(0x20c, 0xd503'201f);  // nop: was msr TPIDR_EL0
    prog.put<std::uint32_t>(0x210, 0xd503'201f);  // nop: was mrs xzr
    prog.put<std::uint32_t>(0x214, 0xd65f'0240);  // ret x18 inside fn: the hard use, so (c) takes fn on its own
    const auto scan = apk::container::scan_code(prog.bytes);
    auto plan = apk::container::c_rename_plan(prog.bytes, scan.findings.x18_unsupported);
    CHECK(plan.functions.size() == 1 && plan.functions[0].begin == 0x200 && plan.functions[0].end == 0x218);
    CHECK(plan.covered_sites == std::vector<std::uint64_t>{0x214});

    // A window [0x1f8, 0x204): its load-exclusive sits before fn's begin (0x200) and its store-exclusive runs into
    // fn -- the old gap where the whole-function rename touched the inside words but an island relocated the window.
    apk::rewrite::Findings straddled = scan.findings;
    straddled.exclusive_regions.push_back(apk::rewrite::ExclusiveRegion{
        .address = 0x1f8, .words = {0xc85f'7c52, 0x9100'0652, 0xc803'7c52}});  // ldxr/add/stxr x18, [0x1f8, 0x204)
    apk::container::keep_c_functions_clear_of_islands(plan, straddled);
    CHECK(plan.functions.empty());      // fn dropped: no (c) range covers it
    CHECK(plan.covered_sites.empty());  // its site is no longer covered, so convert() refuses the file
}

// ADR-0020 option (c) step A2: a (c) function may hold a co-resident syscall (svc) -- before A2 keep-clear dropped it
// for the syscall island and the file was refused. A2 keeps it: the site loop patches the svc to a branch into its
// veneer, and because that branch leaves the function it becomes a (c) branch_exit whose sync stores the rename
// register to the guest-x18 TSD slot BEFORE the veneer runs. So slot 11 is current through the veneer, and a signal
// stopped there (which reads the stale slot 11 over A0's path) sees the right x18. The pre-count builds a patched
// preview of the body so it reserves that branch_exit island (Q2/Fix B; the write-of-x18-in-the-veneer case is a
// named limit -- x18 is Android's reserved SCS pointer, and the veneer returns straight to site+4 with no reload).
TEST(takes_a_c_function_with_a_co_resident_syscall) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // fn = [0x200, 0x218)
    prog.put<std::uint32_t>(0x200, 0xd503'201f);  // nop: was mrs TPIDR_EL0
    prog.put<std::uint32_t>(0x204, 0xd503'201f);  // nop: was the stack-guard load
    prog.put<std::uint32_t>(0x208, 0xd400'0001);  // svc #0: the co-resident syscall, inside the (c) function
    prog.put<std::uint32_t>(0x20c, 0xd503'201f);  // nop: was msr TPIDR_EL0
    prog.put<std::uint32_t>(0x210, 0xd503'201f);  // nop: was mrs xzr
    prog.put<std::uint32_t>(0x214, 0xd65f'0240);  // ret x18: the hard x18 use, so (c) takes fn
    const auto container = convert(prog);         // before A2 keep-clear drops fn for the svc island -> Incompatible
    const auto info = read_info(container.bytes);
    CHECK(info.version == APK_CONTAINER_VERSION);
    // (c) took the function: a body range covers it with the rename register.
    std::optional<std::uint8_t> reg;
    for (const auto &range : c_range_table(container)) {
        if (range.start <= info.image_offset + 0x214 && info.image_offset + 0x214 < range.end) reg = range.reg;
    }
    CHECK(reg.has_value());
    // The svc was patched to a branch into its (c) branch_exit sync island, not left in place.
    const auto sync = branch_target(container.bytes, info.image_offset + 0x208);
    CHECK(sync.has_value());
    // That island stores the rename register to the guest-x18 slot (str <reg>, [x16, #APKRT_GUEST_X18_OFFSET]) before
    // branching on to the veneer, so slot 11 is current while the thread is in the veneer stub.
    if (sync && reg) {
        const std::uint32_t store =
            0xf900'0000u | ((APKRT_GUEST_X18_OFFSET / 8u) << 10) | (16u << 5) | std::uint32_t{*reg};
        bool synced = false;
        for (std::uint64_t word = 0; word < 6; ++word) {
            if (word_at(container.bytes, *sync + word * 4) == store) synced = true;
        }
        CHECK(synced);
    }
}

// ADR-0020 option (c) step A3: a (c) function whose FIRST instruction becomes a veneer branch -- a thread-pointer read
// into a real register, or a thread-pointer write -- is refused. The gate's c_entry_runs_first_instruction lets a
// mrs/msr through (it only blocks a branch, a pc-relative word or a literal load), so the site loop patches that begin
// instruction to `b <veneer>`; c_sync_points classifies offset 0 as the entry, not a branch_exit, so emit_c_entry runs
// that branch as the entry's "first instruction" with NO R->slot-11 store before it. A signal in that veneer would
// read slot 11 relying on it still holding the entry x18, and re-basing the entry's branch is an unverified corner.
// Refuse it (a named limit; a function seldom starts with a thread-pointer read or write). A svc at begin needs no
// handling here -- is_branch(svc) is true, so c_entry_runs_first_instruction already makes the gate refuse it. scan()
// builds this directly, and before A3 keep-clear kept the function (A2), so this is red until the begin-drop lands.
TEST(drops_a_c_function_whose_first_instruction_is_a_thread_pointer_read) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // fn = [0x200, 0x218)
    prog.put<std::uint32_t>(0x200, 0xd53b'd040);  // mrs x0, TPIDR_EL0 AS the first instruction: becomes a veneer branch
    prog.put<std::uint32_t>(0x204, 0xd503'201f);  // nop: clear the stack-guard load
    // nop: clear the default svc, so the begin read is the only veneer site
    prog.put<std::uint32_t>(0x208, 0xd503'201f);
    prog.put<std::uint32_t>(0x20c, 0xd503'201f);  // nop: was msr TPIDR_EL0
    prog.put<std::uint32_t>(0x210, 0xd503'201f);  // nop: was mrs xzr
    prog.put<std::uint32_t>(0x214, 0xd65f'0240);  // ret x18: the hard x18 use, so the gate takes fn on its own
    const auto scan = apk::container::scan_code(prog.bytes);
    auto plan = apk::container::c_rename_plan(prog.bytes, scan.findings.x18_unsupported);
    CHECK(plan.functions.size() == 1 && plan.functions[0].begin == 0x200);  // the gate takes it (mrs is not a branch)
    CHECK(plan.covered_sites == std::vector<std::uint64_t>{0x214});
    apk::container::keep_c_functions_clear_of_islands(plan, scan.findings);
    CHECK(plan.functions.empty());      // A3 drops it: the veneer branch coincides with the entry
    CHECK(plan.covered_sites.empty());  // its site is no longer covered, so convert() refuses the file
}

// A3 refuses only a site that becomes a veneer BRANCH at begin. A thread-pointer read into xzr (mrs xzr) is patched to
// a nop with no stub (emit() returns nop for a read into xzr), so there is no entry/veneer clash -- emit_c_entry just
// re-bases the nop. The function stays taken; A3 must not over-refuse it (all apps by default). This goes red only if
// A3 drops a begin-coincident into-xzr read, so it guards the carve-out.
TEST(keeps_a_c_function_whose_first_instruction_is_a_thread_pointer_read_into_xzr) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // fn = [0x200, 0x218)
    // mrs xzr, TPIDR_EL0 AS the first instruction: a no-stub nop, no branch
    prog.put<std::uint32_t>(0x200, 0xd53b'd05f);
    prog.put<std::uint32_t>(0x204, 0xd503'201f);  // nop
    prog.put<std::uint32_t>(0x208, 0xd503'201f);  // nop: clear the default svc
    prog.put<std::uint32_t>(0x20c, 0xd503'201f);  // nop: was msr TPIDR_EL0
    prog.put<std::uint32_t>(0x210, 0xd503'201f);  // nop
    prog.put<std::uint32_t>(0x214, 0xd65f'0240);  // ret x18: the hard x18 use, so (c) takes fn
    const auto scan = apk::container::scan_code(prog.bytes);
    auto plan = apk::container::c_rename_plan(prog.bytes, scan.findings.x18_unsupported);
    CHECK(plan.functions.size() == 1 && plan.functions[0].begin == 0x200);
    apk::container::keep_c_functions_clear_of_islands(plan, scan.findings);
    CHECK(plan.functions.size() == 1);  // kept: the into-xzr read is a nop, not a veneer branch at the entry
    CHECK(plan.covered_sites == std::vector<std::uint64_t>{0x214});
}

TEST(moves_a_4_kib_image_so_that_its_writable_part_starts_a_16_kib_page) {
    // ADR-0007 decision 2, as amended: the whole image moves by a multiple of 4 KiB, as AOSP's 16 KiB compat mode
    // moves its load bias, and nothing in it changes.
    const std::pair<std::uint64_t, std::uint64_t> cases[] = {{0x42c0, 0}, {0x32c0, 0x1000}, {0x22c0, 0x2000},
                                                             {0x12c0, 0x3000}};
    for (const auto &[writable, shift] : cases) {
        const auto builder = four_kib_program(writable);
        const auto container = convert(builder);
        const auto &bytes = container.bytes;
        const auto info = read_info(bytes);
        CHECK(container.report.image_shift == shift);
        CHECK(container.report.image_offset == info.image_offset);
        CHECK(info.image_offset % 0x4000 == shift);
        CHECK(info.min_vaddr == 0);
        CHECK(info.writable_start == (writable & ~std::uint64_t{0xfff}));
        CHECK((info.image_offset + info.writable_start - info.min_vaddr) % 0x4000 == 0);  // code below, data above
        CHECK(info.writable_end == writable - 0x2c0 + 0x4000);  // .bss's end, the data in whole 16 KiB pages
        CHECK(info.image_size == info.writable_end);
        CHECK(info.runtime_slots == info.image_offset + info.image_size);  // the 16 KiB page after the data
        // The ELF's bytes at their addresses: its header at the start, the dynamic table in the data.
        CHECK(std::memcmp(bytes.data() + info.image_offset, builder.bytes.data(), 64) == 0);
        CHECK(std::memcmp(bytes.data() + info.image_offset + writable, builder.bytes.data() + 0x2c0, 0x40) == 0);
    }
}

/// four_kib_program with a read-only segment after the data at `read_only` (0x100 into its file), a second boundary
/// between code and data, and GNU_RELRO over the dynamic table; five program headers, as four_kib_program's, so that
/// they end before its RELR table.
ElfBuilder four_kib_program_with_read_only_after(std::uint64_t read_only) {
    auto builder = four_kib_program(0x12c0);
    builder.segments({
        {pt_load, r | x, 0, 0, 0x220, 0x220, 0x1000},
        {pt_load, r | w, 0x2c0, 0x12c0, 0x40, 0x1000, 0x1000},
        {pt_load, r, 0x100, read_only, 0x40, 0x40, 0x1000},
        {pt_dynamic, r | w, 0x2c0, 0x12c0, 0x40, 0x40, 8},
        {pt_gnu_relro, r, 0x2c0, 0x12c0, 0x40, 0x40, 1},
    });
    return builder;
}

/// The runs of the container's info (ADR-0008 amendment): start, end and whether writable.
std::vector<apk_run> runs_of(const std::vector<std::byte> &bytes) {
    const auto info = read_info(bytes);
    std::vector<apk_run> runs(info.run_count);
    std::memcpy(runs.data(), bytes.data() + info.runs, runs.size() * sizeof(apk_run));
    return runs;
}

/// The Mach-O segments of a container: name, address, size and protection.
struct MachSegment {
    std::string name;
    std::uint64_t address;
    std::uint64_t size;
    std::uint32_t protection;
    std::uint64_t file_offset;
    std::uint64_t file_size;
};

std::vector<MachSegment> mach_segments(const std::vector<std::byte> &bytes) {
    mach_header_64 header;
    std::memcpy(&header, bytes.data(), sizeof header);
    std::vector<MachSegment> segments;
    std::size_t at = sizeof header;
    for (std::uint32_t index = 0; index < header.ncmds; ++index) {
        load_command command;
        std::memcpy(&command, bytes.data() + at, sizeof command);
        if (command.cmd == LC_SEGMENT_64) {
            segment_command_64 segment;
            std::memcpy(&segment, bytes.data() + at, sizeof segment);
            segments.push_back({std::string(segment.segname, strnlen(segment.segname, sizeof segment.segname)),
                                segment.vmaddr, segment.vmsize, static_cast<std::uint32_t>(segment.initprot),
                                segment.fileoff, segment.filesize});
        }
        at += command.cmdsize;
    }
    return segments;
}

constexpr std::uint32_t rx = 5, rw = 3;  // VM protections

/// Where container address `address` lies in the file, as dyld maps it; zero-filled pages lie nowhere.
std::optional<std::uint64_t> file_offset_of(const std::vector<std::byte> &bytes, std::uint64_t address) {
    for (const auto &segment : mach_segments(bytes)) {
        if (address >= segment.address && address < segment.address + segment.file_size) {
            return segment.file_offset + (address - segment.address);
        }
    }
    return std::nullopt;
}

/// The bytes at container address `address`, read where dyld maps them from.
const std::byte *mapped(const std::vector<std::byte> &bytes, std::uint64_t address) {
    const auto offset = file_offset_of(bytes, address);
    if (!offset) throw std::runtime_error(std::format("{:#x} is not in the container's file", address));
    return bytes.data() + *offset;
}

/// The word at container address `address` and the container address a B there branches to.
std::uint32_t mapped_word(const std::vector<std::byte> &bytes, std::uint64_t address) {
    std::uint32_t value;
    std::memcpy(&value, mapped(bytes, address), sizeof value);
    return value;
}

std::optional<std::uint64_t> mapped_branch_target(const std::vector<std::byte> &bytes, std::uint64_t address) {
    const auto instruction = mapped_word(bytes, address);
    if ((instruction & 0xfc00'0000) != 0x1400'0000) return std::nullopt;
    const auto immediate = static_cast<std::int64_t>(static_cast<std::int32_t>(instruction << 6) >> 6);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(address) + 4 * immediate);
}

/// the Unity test APK v2's libil2cpp.so in small (ADR-0008 amendment): built for 16 KiB pages, r-- with the headers and the
/// RELR table, r-x, rw- with the dynamic table (GNU_RELRO), rw- zero filled, then a second r-x, an r-- and an rw- with
/// a relocated word and zero-filled bytes. `relocated` is the address in the RELR table, `relro_size` GNU_RELRO's.
ElfBuilder segment_runs_program(std::uint64_t relocated = 0x18500, std::uint64_t relro_size = 0x40) {
    ElfBuilder builder;
    builder.bytes.resize(0x800);
    builder.put<std::uint64_t>(24, 0x4400);  // e_entry
    builder
        .segments({
            {pt_phdr, r, 0x40, 0x40, 11 * 56, 11 * 56, 8},
            {pt_load, r, 0, 0, 0x2b8, 0x2b8, 0x4000},
            {pt_load, r | x, 0x400, 0x4400, 0x20, 0x20, 0x4000},
            {pt_load, r | w, 0x4c0, 0x84c0, 0x40, 0x40, 0x4000},
            {pt_load, r | w, 0x500, 0xc500, 0, 0x100, 0x4000},
            {pt_load, r | x, 0x440, 0x10440, 0x20, 0x20, 0x4000},
            {pt_load, r, 0x480, 0x14480, 0x40, 0x40, 0x4000},
            {pt_load, r | w, 0x500, 0x18500, 0x10, 0x1e0, 0x4000},
            {pt_dynamic, r | w, 0x4c0, 0x84c0, 0x40, 0x40, 8},
            {pt_gnu_relro, r, 0x4c0, 0x84c0, 0x40, relro_size, 1},
            {pt_gnu_stack, r | w, 0, 0, 0, 0, 0},
        })
        .put<std::uint64_t>(0x2b0, relocated)
        .dynamic(0x4c0, {{dt_relr, 0x2b0}, {dt_relrsz, 8}, {dt_relrent, 8}, {0, 0}})
        .text(0x480, "read-only data after the data   ")
        .put<std::uint64_t>(0x500, 0x1234);
    std::memcpy(builder.bytes.data() + 0x400, code, sizeof code);
    std::memcpy(builder.bytes.data() + 0x440, code, sizeof code);
    builder.sections({
        {".relr.dyn", sht_relr, shf_alloc, 0x2b0, 0x2b0, 8},
        {".text", sht_progbits, shf_alloc | shf_execinstr, 0x4400, 0x400, sizeof code},
        {".dynamic", sht_dynamic, shf_alloc | shf_write, 0x84c0, 0x4c0, 0x40},
        {".bss", sht_nobits, shf_alloc | shf_write, 0xc500, 0x500, 0x100},
        {".text.after", sht_progbits, shf_alloc | shf_execinstr, 0x10440, 0x440, sizeof code},
        {".rodata", sht_progbits, shf_alloc, 0x14480, 0x480, 0x40},
        {".data", sht_progbits, shf_alloc | shf_write, 0x18500, 0x500, 0x10},
        {".bss.after", sht_nobits, shf_alloc | shf_write, 0x18510, 0x510, 0x1d0},
    });
    return builder;
}

/// Code, data with `zero_filled` bytes of .bss, then code again on the 16 KiB page after them.
ElfBuilder data_then_code(std::uint64_t zero_filled) {
    const std::uint64_t later = ((0x42c0 + zero_filled + 0x3fff) & ~std::uint64_t{0x3fff}) + 0x300;
    ElfBuilder builder;
    builder.bytes.resize(0x400);
    builder.put<std::uint64_t>(24, 0x200);  // e_entry
    builder
        .segments({
            {pt_load, r | x, 0, 0, 0x220, 0x220, 0x4000},
            {pt_load, r | w, 0x2c0, 0x42c0, 0x40, zero_filled, 0x4000},
            {pt_load, r | x, 0x300, later, 0x20, 0x20, 0x4000},
            {pt_dynamic, r | w, 0x2c0, 0x42c0, 0x40, 0x40, 8},
        })
        .dynamic(0x2c0, {{0, 0}});
    for (std::size_t at = 0x200; at < 0x220; at += 4) builder.put<std::uint32_t>(at, 0xd503'201f);  // nop
    for (std::size_t at = 0x300; at < 0x320; at += 4) builder.put<std::uint32_t>(at, 0xd503'201f);
    builder.sections({
        {".text", sht_progbits, shf_alloc | shf_execinstr, 0x200, 0x200, 0x20},
        {".text.after", sht_progbits, shf_alloc | shf_execinstr, later, 0x300, 0x20},
    });
    return builder;
}

TEST(splits_an_image_into_runs_of_protection) {
    // ADR-0008 amendment: every run of pages with one protection is a Mach-O segment of its own, in address order,
    // under one load bias. Non-writable pages are code (r-x), read-only data included.
    const auto builder = segment_runs_program();
    const auto container = convert(builder);
    const auto &bytes = container.bytes;
    const auto info = read_info(bytes);
    CHECK(info.version == APK_CONTAINER_VERSION);
    CHECK(info.min_vaddr == 0);
    CHECK(container.report.image_shift == 0);
    CHECK(info.image_size == 0x1c000);
    CHECK(info.writable_start == 0x8000);  // the first data run
    CHECK(info.writable_end == 0x1c000);   // the end of the last one
    CHECK(info.runtime_slots == info.image_offset + 0x1c000);

    const auto runs = runs_of(bytes);
    CHECK(runs.size() == 4);
    if (runs.size() == 4) {
        CHECK(runs[0].start == 0 && runs[0].end == 0x8000 && runs[0].flags == 0);
        CHECK(runs[1].start == 0x8000 && runs[1].end == 0x10000 && runs[1].flags == APK_RUN_WRITABLE);
        CHECK(runs[2].start == 0x10000 && runs[2].end == 0x18000 && runs[2].flags == 0);
        CHECK(runs[3].start == 0x18000 && runs[3].end == 0x1c000 && runs[3].flags == APK_RUN_WRITABLE);
    }

    const auto image = info.image_offset;
    const auto segments = mach_segments(bytes);
    CHECK(segments.size() == 5);
    if (segments.size() == 5) {
        CHECK(segments[0].name == "__TEXT" && segments[0].address == 0 && segments[0].size == image + 0x8000 &&
              segments[0].protection == rx);
        CHECK(segments[1].name == "__DATA" && segments[1].address == image + 0x8000 && segments[1].size == 0x8000 &&
              segments[1].protection == rw);
        CHECK(segments[2].name == "__TEXT_1" && segments[2].address == image + 0x10000 &&
              segments[2].size == 0x8000 && segments[2].protection == rx);
        CHECK(segments[3].name == "__DATA_1" && segments[3].address == image + 0x18000 &&
              segments[3].size == 0x8000 && segments[3].protection == rw);  // with the runtime slots' page
        CHECK(segments[4].name == "__LINKEDIT" && segments[4].address == image + 0x20000);
    }

    // The data runs are in the file up to their last file byte's page only: dyld fills the rest with zeros.
    if (segments.size() == 5) {
        CHECK(segments[1].file_size == 0x4000 && segments[2].file_offset == segments[1].file_offset + 0x4000);
        CHECK(segments[3].file_size == 0x4000);
    }
    // Every byte at its ELF address; both executable sections rewritten, whichever run they lie in.
    CHECK(std::memcmp(mapped(bytes, image), builder.bytes.data(), 64) == 0);
    CHECK(std::memcmp(mapped(bytes, image + 0x14480), "read-only data after the data", 29) == 0);
    std::int64_t tag;
    std::memcpy(&tag, mapped(bytes, image + 0x84c0), sizeof tag);
    CHECK(tag == dt_relr);
    CHECK(mapped_word(bytes, image + 0x18500) == 0x1234);
    CHECK(!file_offset_of(bytes, image + 0xc500));  // .bss: zero filled
    for (const std::uint64_t text : {0x4400u, 0x10440u}) {
        const auto target = mapped_branch_target(bytes, image + text);
        CHECK(target.has_value() && *target < image);
        CHECK(mapped_word(bytes, image + text + 4) == code[1]);
    }
    CHECK(info.syscalls == 2 && info.thread_pointer_reads == 4 && info.thread_pointer_writes == 2);
    CHECK(container.report.relocations == 1);
}

TEST(dyld_maps_each_run_with_its_protection) {
    auto container = convert(segment_runs_program());
    apk::codesign::sign(container.bytes, {.identifier = "segment-runs"});
    const auto path = scratch / "segment-runs.dylib";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(container.bytes.data()),
                   static_cast<std::streamsize>(container.bytes.size()));
    }
    void *const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        apk::testing::fail(std::source_location::current(), "dlopen failed: ", dlerror());
        return;
    }
    const mach_header_64 *header = nullptr;
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        if (path == _dyld_get_image_name(index)) {
            header = reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(index));
        }
    }
    CHECK(header != nullptr);
    if (header == nullptr) return;
    const auto info = read_info(container.bytes);
    const auto image = reinterpret_cast<std::uintptr_t>(header) + info.image_offset;
    const auto protection = [](std::uintptr_t address) {
        vm_address_t region = address;
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t basic{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                         reinterpret_cast<vm_region_info_t>(&basic), &count, &object) != KERN_SUCCESS ||
            region > address) {
            return -1;
        }
        return static_cast<int>(basic.protection);
    };
    CHECK(protection(image + 0x4400) == static_cast<int>(rx));
    CHECK(protection(image + 0x8000) == static_cast<int>(rw));
    CHECK(protection(image + 0x10440) == static_cast<int>(rx));
    CHECK(protection(image + 0x14480) == static_cast<int>(rx));
    CHECK(protection(image + 0x18500) == static_cast<int>(rw));
    CHECK(std::memcmp(reinterpret_cast<const void *>(image + 0x14480), "read-only data after the data", 29) == 0);
    // The zero-filled parts of both data runs read as zero and are writable, as are the runtime slots.
    for (const std::uint64_t zero : {0xc500u, 0x18600u}) {
        auto *word = reinterpret_cast<volatile std::uint64_t *>(image + zero);
        CHECK(*word == 0);
        *word = 7;
        CHECK(*word == 7);
    }
    auto *slots = reinterpret_cast<volatile apk_runtime_slots *>(reinterpret_cast<std::uintptr_t>(header) +
                                                                info.runtime_slots);
    slots->syscall_entry = 0x1234;
    CHECK(slots->syscall_entry == 0x1234);
    dlclose(handle);
}

TEST(moves_a_4_kib_image_so_that_every_run_starts_a_16_kib_page) {
    // One shift for the whole image must put every boundary between runs on a 16 KiB page (ADR-0008 amendment,
    // decision 4): the data at 0x1000 and the read-only segment at 0x5000 are both 0x1000 into a 16 KiB page.
    const auto builder = four_kib_program_with_read_only_after(0x5100);
    const auto container = convert(builder);
    const auto &bytes = container.bytes;
    const auto info = read_info(bytes);
    CHECK(container.report.image_shift == 0x3000);
    const auto runs = runs_of(bytes);
    CHECK(runs.size() == 3);
    if (runs.size() == 3) {
        CHECK(runs[0].start == 0 && runs[0].end == 0x1000 && runs[0].flags == 0);
        CHECK(runs[1].start == 0x1000 && runs[1].end == 0x5000 && runs[1].flags == APK_RUN_WRITABLE);
        CHECK(runs[2].start == 0x5000 && runs[2].end == 0x9000 && runs[2].flags == 0);
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        CHECK((info.image_offset + runs[index].start - info.min_vaddr) % 0x4000 == 0);  // the first is in __TEXT
    }
    CHECK(info.image_size == 0x9000);
    CHECK((info.image_offset + info.image_size) % 0x4000 == 0);
    // The image ends with code, so the runtime slots get a data segment of their own.
    const auto segments = mach_segments(bytes);
    CHECK(segments.size() == 5);
    if (segments.size() == 5) {
        CHECK(segments[2].name == "__TEXT_1" && segments[2].protection == rx);
        CHECK(segments[3].name == "__DATA_1" && segments[3].protection == rw &&
              segments[3].address == info.runtime_slots && segments[3].size == 0x4000);
    }
    CHECK(std::memcmp(mapped(bytes, info.image_offset + 0x5100), builder.bytes.data() + 0x100, 0x40) == 0);
}

TEST(refuses_a_4_kib_layout_no_shift_fits) {
    // Code and data on one 4 KiB page.
    CHECK_THROWS(Incompatible, convert(four_kib_program(0x2c0)), "share a 4 KiB page");

    // A read-only segment after the data whose boundary lies elsewhere in its 16 KiB page than the data's: no one
    // shift puts both on a 16 KiB page. The refusal names every LOAD segment and GNU_RELRO, so that an app's import
    // shows its layout (the Unity test APK v2's libil2cpp.so).
    const auto read_only_after = four_kib_program_with_read_only_after(0x3100);
    CHECK_THROWS(Incompatible, convert(read_only_after), "no shift puts every boundary between code and data");
    CHECK_THROWS(Incompatible, convert(read_only_after),
                 "(segments: LOAD r-x 0x0 size 0x220 align 0x1000, LOAD rw- 0x12c0 size 0x1000 align 0x1000, "
                 "LOAD r-- 0x3100 size 0x40 align 0x1000, GNU_RELRO 0x12c0 size 0x40)");

    // Aligned to less than 4 KiB.
    auto small = four_kib_program(0x42c0);
    small.put<std::uint64_t>(64 + 56 + 48, 0x800);  // p_align of the code segment
    CHECK_THROWS(Incompatible, convert(small), "4 KiB pages");
}

// A library built for 4 KiB pages but post-processed to 16 KiB: only the code segment's p_align is 0x4000 (lld gives
// every PT_LOAD the same, so a mix is a realignment tool's mark), and nothing inside asks for more than 4 KiB. A
// whole-image shift by a 4 KiB multiple keeps every section aligned, so it converts exactly as the 4 KiB library it
// is -- the user's Ren'Py test case (librenpython.so, ADR-0007). Android maps such a library at 4 KiB on a 4 KiB
// device too, so this gives up no alignment Android promises.
TEST(a_16_kib_p_align_the_sections_do_not_need_takes_a_4_kib_shift) {
    auto mixed = four_kib_program(0x22c0);
    mixed.put<std::uint64_t>(64 + 56 + 48, 0x4000);  // the code segment's p_align, as a 16 KiB realignment tool sets it
    const auto container = convert(mixed);
    CHECK(container.report.image_shift == 0x2000);          // converts, shifting 8 KiB like the 4 KiB library it is
    CHECK(container.report.moved_sections_note.empty());    // nothing asked for more than 4 KiB, so no note
}

// An allocated section's sh_addralign above 4 KiB (here .text asks for 16 KiB) no longer refuses the file: the
// 4 KiB-multiple shift keeps every relative offset and the run-based 16 KiB boundaries, and bionic maps such a
// section at 4 KiB on a 4 KiB device too (ADR-0007). The section is named in report.moved_sections_note so a later
// misbehaving app shows what was moved.
TEST(a_section_that_asks_for_16_kib_takes_the_shift_with_a_note) {
    auto needs = four_kib_program(0x22c0, 0x4000);   // .text's sh_addralign asks for 16 KiB
    needs.put<std::uint64_t>(64 + 56 + 48, 0x4000);  // and the code segment's p_align matches
    const auto container = convert(needs);
    CHECK(container.report.image_shift == 0x2000);
    CHECK(container.report.moved_sections_note == ".text (addr 0x200, size 0x20, align 0x4000)");
}

// The TLS block still refuses (unlike a section, above): a sixth program header, PT_TLS over the data, asking for
// 16 KiB. Our per-thread TLS must honour its p_align, which a shift off a 16 KiB boundary would break. The refusal
// names the TLS block and its alignment.
TEST(a_16_kib_tls_block_keeps_the_refusal) {
    auto tls = four_kib_program(0x22c0);
    tls.put<std::uint64_t>(64 + 56 + 48, 0x4000);                       // the code segment's p_align
    const std::size_t sixth = 64 + 5 * 56;                             // right after four_kib_program's five headers
    tls.put<std::uint32_t>(sixth, 7).put<std::uint32_t>(sixth + 4, 4);  // p_type PT_TLS, p_flags R
    tls.put<std::uint64_t>(sixth + 8, 0x2c0).put<std::uint64_t>(sixth + 16, 0x22c0);  // p_offset, p_vaddr (in the data)
    tls.put<std::uint64_t>(sixth + 32, 0x40).put<std::uint64_t>(sixth + 40, 0x40);    // p_filesz, p_memsz
    tls.put<std::uint64_t>(sixth + 48, 0x4000);                        // p_align: the TLS block wants 16 KiB
    tls.put<std::uint16_t>(56, 6);                                     // e_phnum
    CHECK_THROWS(Incompatible, convert(tls), "the TLS block (align 0x4000)");
}

// The refusal names the enclosing symbol (.symtab/.dynsym: name + offset, and the section) so an import of a
// protected library shows which component an x18 use sits in, not just an address. A stripped library names no
// symbol and the message is unchanged (the other refusal tests build no symbol table).
TEST(a_refusal_names_the_enclosing_symbol) {
    auto named = static_program(0x4800, {{"guest_atomic", 0x200, 0x20}});
    named.put<std::uint32_t>(0x214, 0x17ff'fffb);  // b 0x200 (begin): a non-call branch to begin -> (c) cannot take fn
    named.put<std::uint32_t>(0x218, 0xd61f'0240);  // br x18: an indirect island, not refused
    named.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18: the sole refused site, no island follows a return
    CHECK_THROWS(Incompatible, convert(named), "x18 at 0x21c in guest_atomic+0x1c (.text) cannot run on iOS");
}

// A mangled or hostile symbol name is capped so it cannot grow the reason and the import report without bound.
TEST(a_refusal_caps_a_long_symbol_name) {
    const std::string long_name(200, 'z');
    auto named = static_program(0x4800, {{long_name, 0x200, 0x20}});
    named.put<std::uint32_t>(0x214, 0x17ff'fffb);  // b 0x200 (begin): a non-call branch to begin -> (c) cannot take fn
    named.put<std::uint32_t>(0x218, 0xd61f'0240);  // br x18: an indirect island, not refused
    named.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18: the sole refused site, no island follows a return
    CHECK_THROWS(Incompatible, convert(named), std::string(157, 'z') + "...");
}

// The refused x18 sites are counted (ADR-0020 (c)) by whether their enclosing function has a free register. The
// function bounds come from the symbol that contains the site, or, with none, the run between the returns around it.
TEST(x18_sites_with_a_free_function_register_uses_the_enclosing_symbol) {
    auto named = static_program(0x4800, {{"small_fn", 0x200, 0x20}});
    named.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18 (refused: a register branch)
    const auto scan = apk::container::scan_code(named.bytes);
    CHECK(scan.findings.x18_unsupported.size() == 1);
    CHECK(apk::container::x18_sites_with_a_free_function_register(named.bytes, scan.findings.x18_unsupported) == 1);
}

TEST(x18_sites_with_a_free_function_register_falls_back_to_the_returns_around_the_site) {
    auto stripped = static_program();  // no symbols
    stripped.put<std::uint32_t>(0x218, 0xd65f'0240);  // ret x18 (refused), after the ret at 0x214
    const auto scan = apk::container::scan_code(stripped.bytes);
    CHECK(scan.findings.x18_unsupported.size() == 1);
    CHECK(apk::container::x18_sites_with_a_free_function_register(stripped.bytes, scan.findings.x18_unsupported) == 1);
}

// Option (c) renames x18 only across bounds a sized symbol gives exactly. A RET-to-RET guess is good enough to count
// whether a site has a free register, but renaming past a guessed end could rewrite x18 in the next function; so the
// rename bounds come from a sized function symbol or not at all (must-have #2).
TEST(sized_function_bounds_are_the_containing_symbol_or_nothing) {
    const std::vector<apk::elf::Symbol> symbols = {
        {"outer", 0x200, 0x80},  // 0x200..0x280
        {"inner", 0x240, 0x20},  // 0x240..0x260, the tighter one over 0x250
        {"sizeless", 0x300, 0},  // st_size 0: never a boundary
    };
    using Bounds = std::pair<std::uint64_t, std::uint64_t>;
    using apk::container::sized_function_bounds;
    CHECK(sized_function_bounds(symbols, 0x200, 0x1000, 0x250) == std::optional<Bounds>(Bounds(0x240, 0x260)));
    CHECK(sized_function_bounds(symbols, 0x200, 0x1000, 0x2c0) == std::nullopt);  // past every sized symbol
    CHECK(sized_function_bounds(symbols, 0x200, 0x1000, 0x300) == std::nullopt);  // only a size-0 symbol here
    // Clamped to the section: a symbol running past the section end stops at `limit`.
    const std::vector<apk::elf::Symbol> spans = {{"spans", 0x200, 0x1000}};  // would end at 0x1200
    CHECK(sized_function_bounds(spans, 0x200, 0x800, 0x300) == std::optional<Bounds>(Bounds(0x200, 0x800)));
}

// c_rename_register is option (c)'s gate: it takes a site only when a sized symbol bounds its function (must-have 2),
// a caller-saved register is free across it (must-have 1) and nothing branches into its interior from outside
// (must-have 3). Here fn is [0x200, 0x218): it starts the section (nothing falls into it) and ends at the ret at
// 0x214 (nothing falls out), x9-x15 are untouched, and no outside word branches in.
TEST(c_rename_register_takes_a_bounded_function_with_a_spare_register_and_no_outside_entry) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});
    const auto taken = apk::container::c_rename_register(prog.bytes, 0x210);
    CHECK(taken.has_value());
    CHECK(taken && taken->begin == 0x200 && taken->end == 0x218 && taken->reg == 9u);
}

TEST(c_rename_register_refuses_without_a_sized_symbol) {
    auto stripped = static_program();  // no symbols: the rename has no bounds it can trust (must-have 2)
    CHECK(apk::container::c_rename_register(stripped.bytes, 0x210) == std::nullopt);
}

TEST(c_rename_register_refuses_a_branch_into_the_function_interior) {
    auto prog = static_program(0x4800, {{"fn", 0x208, 0x10}});  // the function is [0x208, 0x218), ret at 0x214
    prog.put<std::uint32_t>(0x204, 0xd65f'03c0);  // ret before fn, so no word falls into begin -- only the branch refuses
    prog.put<std::uint32_t>(0x200, 0x1400'0004);  // b 0x210: from outside into the interior, not the entry 0x208
    CHECK(apk::container::c_rename_register(prog.bytes, 0x210) == std::nullopt);
}

// Hole 1 (lead review): "tightest by start" can pick a nested or overlapping sized symbol (an inner label), renaming
// only part of a real function. Refuse unless every sized symbol overlapping the chosen bounds has exactly them: here
// inner [0x208, 0x210) is chosen but outer [0x200, 0x218) overlaps it. The rets at 0x204 and 0x20c keep nothing
// falling into or out of inner, so only the overlap refuses.
TEST(c_rename_register_refuses_a_nested_or_overlapping_sized_symbol) {
    auto prog = static_program(0x4800, {{"outer", 0x200, 0x18}, {"inner", 0x208, 0x8}});
    prog.put<std::uint32_t>(0x204, 0xd65f'03c0);  // ret: nothing falls into inner's begin (0x208)
    prog.put<std::uint32_t>(0x20c, 0xd65f'03c0);  // ret: inner's last word does not fall past its end (0x210)
    CHECK(apk::container::c_rename_register(prog.bytes, 0x208) == std::nullopt);
}

// Hole 1: a word that falls through into begin means begin is mid-function; the renamed code would run with the
// register still unset. fn is [0x208, 0x218) and the word at 0x204 (the default ldr) falls straight into it; the
// ret at 0x214 ends fn, so nothing else refuses.
TEST(c_rename_register_refuses_a_fallthrough_into_the_function) {
    auto prog = static_program(0x4800, {{"fn", 0x208, 0x10}});
    CHECK(apk::container::c_rename_register(prog.bytes, 0x210) == std::nullopt);
}

// Hole 1: a last word that falls through past end lets the renamed register escape into the next function. fn is
// [0x200, 0x210); its last word at 0x20c (the default msr) falls straight out. It starts the section, so nothing
// falls in.
TEST(c_rename_register_refuses_a_fallthrough_past_the_function) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x10}});
    CHECK(apk::container::c_rename_register(prog.bytes, 0x208) == std::nullopt);
}

// Hole 3 (lead review): begin's instruction becomes a branch to the entry island, which reloads the rename register
// from TSD slot 11. A non-call branch inside the function back to begin would rerun that reload mid-function and
// clobber the live register with a stale slot 11 (stores happen only at exits and calls). Refuse such a function.
TEST(c_rename_register_refuses_an_internal_branch_back_to_begin) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // [0x200, 0x218), ret at 0x214
    prog.put<std::uint32_t>(0x210, 0x17ff'fffc);  // b 0x200 (begin): a loop to the top, not through a store
    CHECK(apk::container::c_rename_register(prog.bytes, 0x208) == std::nullopt);
}

// A self-recursive call to begin is fine: a BL is a call, so its island stores the register to slot 11 before the
// branch, leaving the entry reload current. The function is otherwise renamable, so (c) takes it.
TEST(c_rename_register_allows_a_recursive_call_to_begin) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // [0x200, 0x218), ret at 0x214
    prog.put<std::uint32_t>(0x210, 0x97ff'fffc);  // bl 0x200 (begin): a call, so the store comes first
    CHECK(apk::container::c_rename_register(prog.bytes, 0x208).has_value());
}

// Hole 5 (lead review): a call island leaves x30 pointing into the stub. A `bl .+4` PC-getter (its next instruction
// reads x30 as its own address) would then compute a wrong address, so (c) refuses the function.
TEST(c_rename_register_refuses_a_pc_getter_call) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // [0x200, 0x218), ret at 0x214
    prog.put<std::uint32_t>(0x208, 0x9400'0001);  // bl .+4: x30 would point into the call island, not at 0x20c
    CHECK(apk::container::c_rename_register(prog.bytes, 0x210) == std::nullopt);
}

// Hole 5 rule 3 (lead review): a direct BL to a sized in-file callee that reads x30 as a value is refused -- that
// callee runs with x30 pointing into our call island and would read it as data (inline data after a call).
TEST(c_rename_register_refuses_a_bl_to_a_callee_that_reads_x30) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}, {"callee", 0x218, 0x8}});
    prog.put<std::uint32_t>(0x210, 0x9400'0002);  // bl 0x218 (callee), a direct call inside fn
    prog.put<std::uint32_t>(0x218, 0xaa1e'03e0);  // callee: mov x0, x30 -- reads the link register as a value
    prog.put<std::uint32_t>(0x21c, 0xd65f'03c0);  // callee: ret
    CHECK(apk::container::c_rename_register(prog.bytes, 0x208) == std::nullopt);
}

// A direct BL to a clean sized callee (never reads x30 as a value) does not refuse the caller.
TEST(c_rename_register_allows_a_bl_to_a_clean_callee) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}, {"callee", 0x218, 0x8}});
    prog.put<std::uint32_t>(0x210, 0x9400'0002);  // bl 0x218 (callee)
    prog.put<std::uint32_t>(0x218, 0xd503'201f);  // callee: nop
    prog.put<std::uint32_t>(0x21c, 0xd65f'03c0);  // callee: ret -- never reads x30 as a value
    CHECK(apk::container::c_rename_register(prog.bytes, 0x208).has_value());
}

// ADR-0020 option (c): the entry island runs the function's first instruction, so a function whose first
// instruction is a control transfer (here a bl) is refused -- the island cannot run it and reach the body.
TEST(c_rename_register_refuses_a_control_flow_first_instruction) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // [0x200, 0x218)
    prog.put<std::uint32_t>(0x200, 0x9400'0010);  // first instruction is a bl: the entry island cannot run it
    CHECK(apk::container::c_rename_register(prog.bytes, 0x210) == std::nullopt);
}

// ADR-0020 option (c): a function whose x18 use has no register field (a CASP pair) cannot be renamed, so the gate
// refuses it -- the count stays honest and emit_c_rename never throws on a gated function.
TEST(c_rename_register_refuses_a_function_with_an_unrenamable_x18_use) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // [0x200, 0x218)
    prog.put<std::uint32_t>(0x210, 0x4832'7c82);  // casp x18, x19, x2, x3, [x4]: x18 has no register field
    CHECK(apk::container::c_rename_register(prog.bytes, 0x208) == std::nullopt);
}

// Step 4b: c_rename_plan lists the functions (c) renames and the refused sites they cover. fn (c) takes holds one
// `ret x18`; another `ret x18` is in no sized function, so it is not covered and convert() will still refuse it.
TEST(c_rename_plan_lists_the_functions_c_takes_and_the_sites_in_them) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});
    prog.put<std::uint32_t>(0x214, 0xd65f'0240);  // ret x18 inside fn: covered
    prog.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18 in no sized function: not covered
    const auto scan = apk::container::scan_code(prog.bytes);
    const auto plan = apk::container::c_rename_plan(prog.bytes, scan.findings.x18_unsupported);
    CHECK(plan.functions.size() == 1);
    CHECK(plan.functions.size() == 1 && plan.functions[0].begin == 0x200 && plan.functions[0].end == 0x218 &&
          plan.functions[0].reg == 9u);
    CHECK(plan.covered_sites == std::vector<std::uint64_t>{0x214});
}

// Step 4b: of the refused x18 sites, x18_sites_c_would_take counts those in a function (c) would take. Here one
// `ret x18` sits in the sized function fn (c) accepts; another sits in no sized function, so (c) cannot take it.
TEST(x18_sites_c_would_take_counts_the_refused_sites_in_functions_c_accepts) {
    auto prog = static_program(0x4800, {{"fn", 0x200, 0x18}});  // fn = [0x200, 0x218)
    prog.put<std::uint32_t>(0x214, 0xd65f'0240);  // ret x18: a refused x18 use, inside fn, which (c) takes
    prog.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18: refused too, but in no sized function
    const auto scan = apk::container::scan_code(prog.bytes);
    CHECK(scan.findings.x18_unsupported.size() == 2);
    CHECK(apk::container::x18_sites_c_would_take(prog.bytes, scan.findings.x18_unsupported) == 1);
}

// The per-site facts ride in the refusal reason itself: on the iPhone the library never runs through apk-convert,
// so the Incompatible string is the only way the (b)/(c) counts reach an import.
TEST(the_refusal_text_carries_the_per_site_facts) {
    auto prog = static_program();
    const std::uint32_t window[] = {
        0xc85f'7db1,  // 0x200 ldxr x17, [x13]
        0x9100'025f,  // 0x204 mov sp, x18: no island moves sp from x18, so the window's use stays refused
        0x8b0e'0232,  // 0x208 add x18, x17, x14
        0xc810'fdb2,  // 0x20c stlxr w16, x18, [x13]
        0x35ff'ff90,  // 0x210 cbnz w16, ldxr
        0xd65f'03c0,  // 0x214 ret
    };
    for (std::size_t i = 0; i < std::size(window); ++i) prog.put<std::uint32_t>(0x200 + 4 * i, window[i]);
    CHECK_THROWS(Incompatible, convert(prog),
                 "of the 1: 0 defined in their loop, 1 live into it, 0 in no window; 1 in a function with a free "
                 "register");
}

// ADR-0020 X7 (c2): an x18 use that only an exclusive access refuses (here a store-exclusive with x18 as its data and no
// load-exclusive of its window in sight) runs in an ordinary island whose own accesses lie inside a guest LL/SC; the
// container then carries the flag the loader asks the exclusive-monitor probe about, and the report counts the island. A
// library without such a site has neither.
TEST(a_container_with_an_island_inside_an_ll_sc_sequence_carries_the_monitor_flag) {
    auto inside = static_program();
    inside.put<std::uint32_t>(0x200, 0xc802'7c32);  // stxr w2, x18, [x1]
    inside.put<std::uint32_t>(0x204, 0xd65f'03c0);  // ret
    const auto marked = convert(inside);
    CHECK(marked.report.monitor_inside_islands == 1);
    CHECK((read_info(marked.bytes).flags & APK_CONTAINER_MONITOR_INSIDE) != 0);
    CHECK(read_info(marked.bytes).version == 9);
    const auto plain = convert(static_program());
    CHECK(plain.report.monitor_inside_islands == 0);
    CHECK((read_info(plain.bytes).flags & APK_CONTAINER_MONITOR_INSIDE) == 0);
}

// Linux ignores the immediate of svc (proved on Android 15): every svc #imm is a syscall, rewritten to a branch into
// the same veneer as svc #0, and nothing about it is refused.
TEST(rewrites_a_svc_with_any_immediate_like_svc_zero) {
    for (const std::uint32_t immediate : {0u, 1u, 0x80u, 0xe4d3u, 0xffffu}) {
        auto prog = static_program();
        prog.put<std::uint32_t>(0x218, 0xd400'0001u | (immediate << 5));
        const auto container = convert(prog);
        const auto info = read_info(container.bytes);
        CHECK(container.report.syscalls == 2);  // the program's own svc #0 at 0x208 and this one
        CHECK(branch_target(container.bytes, info.image_offset + 0x218).has_value());
        CHECK(apk::container::refusals(prog.bytes).empty());
    }
}

// A svc word that a literal load reads is data: it stays as it is in the image and is not counted as a syscall.
TEST(keeps_a_svc_word_that_a_literal_load_reads_as_data) {
    for (const std::uint32_t word : {0xd400'0001u, 0xd401'a601u}) {
        auto prog = static_program();
        prog.put<std::uint32_t>(0x20c, 0x5800'0040);  // ldr x0, .+8: reads the word at 0x214
        prog.put<std::uint32_t>(0x214, word);
        const auto container = convert(prog);
        const auto info = read_info(container.bytes);
        CHECK(container.report.syscalls == 1);  // only the program's own svc #0 at 0x208
        CHECK(word_at(container.bytes, info.image_offset + 0x214) == word);
        CHECK(!branch_target(container.bytes, info.image_offset + 0x214).has_value());
    }
}

TEST(refuses_what_cannot_run) {
    auto tpidrro_read = static_program();
    tpidrro_read.put<std::uint32_t>(0x218, 0xd53b'd065);  // mrs x5, TPIDRRO_EL0: reads the Darwin TSD base
    CHECK_THROWS(Incompatible, convert(tpidrro_read), "TPIDRRO_EL0 at 0x218");

    auto register_branch = static_program();
    register_branch.put<std::uint32_t>(0x218, 0xd61f'0240);  // br x18: an indirect island now (X2e)
    register_branch.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18: still refused, no island follows it
    CHECK_THROWS(Incompatible, convert(register_branch), "x18 at 0x21c cannot run on iOS: it is a register branch");
    // The refusal names the exact encoding of the site: its word in hex and its kind (ret x18 is d65f0240).
    CHECK_THROWS(Incompatible, convert(register_branch), "[word 0xd65f0240, branch]");
    // A jump table whose table lies in executable code stays refused: the x18 pass scans the table as
    // code and could rewrite an entry, so the br must not degrade to an indirect island (step C C2b).
    CHECK_THROWS(Incompatible, convert(jump_table_program(shf_alloc | shf_execinstr)),
                 "x18 at 0x21c cannot run on iOS: it is a register branch whose table is not in read-only data");
    // More than one use: every reason with its count and its first addresses, the most common first, so that one
    // refusal names all that a library still needs.
    auto many = static_program();
    many.put<std::uint32_t>(0x204, 0x4822'7e44);  // casp x2, x3, x4, x5, [x18]: x18 as the address
    many.put<std::uint32_t>(0x208, 0x4822'7e44);
    for (const std::uint32_t at : {0x20cu, 0x210u, 0x214u, 0x218u}) {
        many.put<std::uint32_t>(at, 0x9100'025f);  // mov sp, x18
    }
    CHECK_THROWS(Incompatible, convert(many),
                 "x18 cannot run on iOS in 6 places: 4 where it moves the stack pointer, below which the island keeps "
                 "its registers (0x20c [word 0x9100025f, stack pointer use], "
                 "0x210 [word 0x9100025f, stack pointer use], "
                 "0x214 [word 0x9100025f, stack pointer use] and 1 more); 2 where x18 is the address of a pair "
                 "compare-and-swap: it has no register field of its own that an island could stand in for "
                 "(0x204 [word 0x48227e44, pair compare-and-swap] and 0x208 [word 0x48227e44, pair compare-and-swap])");

    CHECK_THROWS(Incompatible, convert(static_program(0x100)), "text relocation");
    CHECK_THROWS(Incompatible, convert(static_program(0x9000)), "outside");

    auto text_relocations = static_program();
    text_relocations.dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {22 /* DT_TEXTREL */, 0}, {0, 0}});
    CHECK_THROWS(Incompatible, convert(text_relocations), "text relocation");
    // A relocation into the code after the data, and GNU_RELRO reaching from a data run into it (ADR-0008
    // amendment, decision 3).
    CHECK_THROWS(Incompatible, convert(segment_runs_program(0x10440)), "text relocation at 0x10440");
    CHECK_THROWS(Incompatible, convert(segment_runs_program(0x18500, 0x8000)),
                 "GNU_RELRO at 0x84c0 size 0x8000 is not inside one run of writable pages");

    // Hostile sizes (the reviewer's finding): a segment whose end wraps around 2^64, a later one that wraps, and an
    // image larger than 4 GiB, whose zero-filled data before code would all go into the file.
    auto wrapping = static_program();
    wrapping.put<std::uint64_t>(64 + 56 + 40, ~std::uint64_t{0} - 0x3fff);  // p_memsz of the code segment
    CHECK_THROWS(Incompatible, convert(wrapping), "ends beyond the address space");
    auto wrapping_data = static_program();
    wrapping_data.put<std::uint64_t>(64 + 2 * 56 + 40, ~std::uint64_t{0} - 0x4000);  // p_memsz of the data segment
    CHECK_THROWS(Incompatible, convert(wrapping_data), "ends beyond the address space");
    auto huge = static_program();
    huge.put<std::uint64_t>(64 + 2 * 56 + 40, std::uint64_t{5} << 30);
    CHECK_THROWS(Incompatible, convert(huge), "spans more than 4 GiB");
    CHECK_THROWS(Incompatible, convert(segment_runs_program(0x18500, ~std::uint64_t{0} - 0x84bf)),
                 "ends beyond the address space");

    // A writable and executable LOAD segment, which Android's linker refuses for apps targeting API 26 or later
    // (ElfReader::LoadSegments) and iOS cannot run; an executable section beyond its segment's file part, whose
    // rewritten sites would lie outside the container's bytes (the review's finding).
    auto writable_code = static_program();
    writable_code.put<std::uint32_t>(64 + 56 + 4, r | w | x);  // p_flags of the code segment
    CHECK_THROWS(Incompatible, convert(writable_code), "LOAD segment at 0x0 is writable and executable");
    auto code_beyond_file = static_program();
    code_beyond_file.put<std::uint64_t>(64 + 56 + 32, 0x210);  // p_filesz of the code segment, .text ends at 0x220
    CHECK_THROWS(Incompatible, convert(code_beyond_file), "the executable section .text reaches beyond");
    // Zero-filled data in the middle stays out of the file; a container far larger than its file is refused.
    CHECK(convert(data_then_code(std::uint64_t{256} << 20)).bytes.size() < (1 << 20));
    auto zero_code = static_program((std::uint64_t{256} << 20) + 0x4800);
    zero_code.put<std::uint64_t>(64 + 56 + 40, std::uint64_t{256} << 20);  // p_memsz of the code segment
    zero_code.put<std::uint64_t>(64 + 2 * 56 + 16, (std::uint64_t{256} << 20) + 0x42c0);  // the data after it
    zero_code.put<std::uint64_t>(64 + 3 * 56 + 16, (std::uint64_t{256} << 20) + 0x42c0);  // PT_DYNAMIC
    CHECK_THROWS(Incompatible, convert(zero_code), "the container would take");

    auto x86 = static_program();
    x86.put<std::uint16_t>(18, 62);
    CHECK_THROWS(Incompatible, convert(x86), "x86-64");

    ElfBuilder without_sections;
    without_sections.segments({{pt_load, r | x, 0, 0, 0x220, 0x220, 0x4000}});
    CHECK_THROWS(Incompatible, convert(without_sections), "section headers");
}

bool contains(const std::string &text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// The refusal names what the library gives for a code/data split: whether there is a .symtab, how many mapping symbols
// it holds, how many of them mark data ($d) in an executable section, and how many of the refused sites lie inside a $d
// range (counts only).
TEST(an_x18_refusal_counts_the_mapping_symbols) {
    const apk::rewrite::Unsupported use{0x100, "x", 0xd65f0240};
    const apk::container::X18SiteCounts counts;
    const std::vector<apk::elf::Section> stripped = {
        {".text", 1, apk::elf::section_allocated | apk::elf::section_executable, 0x100, 0x100, 0x100, 4}};
    CHECK(contains(apk::container::x18_refusal(std::span(&use, 1), {}, stripped, counts),
                   "; no .symtab, 0 mapping symbols, 0 data ranges in executable sections, "
                   "0 of the 1 refused sites inside a $d range"));
    auto with_symtab = stripped;
    with_symtab.push_back({".symtab", apk::elf::section_type_symtab, 0, 0, 0x300, 0x60, 8});
    const std::vector<apk::elf::Symbol> symbols = {{"$x", 0x100, 0}, {"$d", 0x140, 0}, {"$x.1", 0x180, 0},
                                                   {"$d.2", 0x1c0, 0}, {"$d.3", 0x400, 0}, {"main", 0x100, 4}};
    CHECK(contains(apk::container::x18_refusal(std::span(&use, 1), symbols, with_symtab, counts),
                   "; .symtab, 5 mapping symbols, 2 data ranges in executable sections, "
                   "0 of the 1 refused sites inside a $d range"));
    // A refused site between a $d and the next mapping symbol of its section lies in data; one after a $x does not,
    // and nor does one past the section's end.
    const apk::rewrite::Unsupported in_data{0x150, "x", 0xd65f0240};
    const apk::rewrite::Unsupported in_code{0x190, "x", 0xd65f0240};
    const std::vector<apk::rewrite::Unsupported> uses = {use, in_data, in_code};
    CHECK(contains(apk::container::x18_refusal(uses, symbols, with_symtab, counts),
                   "; .symtab, 5 mapping symbols, 2 data ranges in executable sections, "
                   "1 of the 3 refused sites inside a $d range"));
}

/// What convert() throws for `builder`, "" when it converts.
std::string refusal_of(const ElfBuilder &builder) {
    try {
        (void)convert(builder);
    } catch (const Incompatible &refusal) {
        return refusal.what();
    }
    return "";
}

// An x18 refusal also carries what the library tells without symbols (data_hints.h): its unwind tables, the PC-relative
// loads of its code and its section layout, counts only.
TEST(an_x18_refusal_carries_the_hints_that_need_no_symbols) {
    auto stripped = static_program();
    stripped.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18, refused
    const auto text = refusal_of(stripped);
    CHECK(contains(text, "; unwind: .eh_frame_hdr no, .eh_frame no, 0 FDEs, 1 of the 1 sites outside every FDE; "
                         "PC-relative loads: 0 of the 1 sites loaded from code; sections: 1 executable, 8 bytes of "
                         "other sections in executable segments, 1 of the 1 sites in executable sections; refused "
                         "words: 0x21c (.text, outside every FDE, not loaded, "));
}

/// static_program with a RELR table of `entries` words in a third, read-only LOAD segment: the first an address in
/// the code, the rest bitmaps with every bit set, so that each word names 63 relocations into places outside any
/// segment or into the code, all refused.
ElfBuilder program_with_relocation_flood(std::size_t entries) {
    ElfBuilder builder = static_program();
    const std::uint64_t table = 0x1000;
    builder.bytes.resize(table + entries * 8);
    builder
        .segments({
            {pt_phdr, r, 0x40, 0x40, 6 * 56, 6 * 56, 8},
            {pt_load, r | x, 0, 0, 0x220, 0x220, 0x4000},
            {pt_load, r | w, 0x2c0, 0x42c0, 0x40, 0x1000, 0x4000},
            {pt_load, r, table, 0x20000, entries * 8, entries * 8, 0x4000},
            {pt_dynamic, r | w, 0x2c0, 0x42c0, 0x40, 0x40, 8},
            {pt_gnu_stack, r | w, 0, 0, 0, 0, 0},
        })
        .dynamic(0x2c0, {{dt_relr, 0x20000}, {dt_relrsz, static_cast<std::int64_t>(entries * 8)}, {dt_relrent, 8},
                         {0, 0}});
    builder.put<std::uint64_t>(table, 0x200);
    for (std::size_t index = 1; index < entries; ++index) {
        builder.put<std::uint64_t>(table + index * 8, ~std::uint64_t{0});
    }
    return builder;
}

TEST(a_flood_of_refused_relocations_costs_a_count_not_a_message_each) {
    // 4 MiB of bitmaps name some 26 million relocations; the reason is made once, for the first (the review:
    // 25 s with a message each).
    const auto flood = program_with_relocation_flood(std::size_t{4} << 17);
    const auto start = std::chrono::steady_clock::now();
    const auto reasons = apk::container::refusals(flood.bytes);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(!reasons.empty());
    CHECK(contains(reasons.at(0), "text relocation at 0x200"));
    CHECK(contains(reasons.at(0), "more)"));
    CHECK(seconds < 30.0);  // about 4 s under the sanitizers, 2 minutes with a message each
}

TEST(a_bad_relocation_is_named_before_a_later_table_that_cannot_be_read) {
    // convert() named the first bad relocation before it looked at the later tables (the review): a RELR
    // text relocation, then a RELA table outside the file.
    auto first = static_program();
    first.put<std::uint64_t>(0x160, 0x200);  // a relocation into the code
    first.dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {dt_rela, 0x90000000}, {dt_relasz, 24}, {0, 0}});
    CHECK(contains(refusal_of(first), "text relocation at 0x200"));
    CHECK(contains(apk::container::refusals(first.bytes).at(0), "text relocation at 0x200"));
    // The same with an entry size that is not 24.
    auto sized = static_program();
    sized.put<std::uint64_t>(0x160, 0x200);
    sized.dynamic(0x2c0,
                  {{dt_relr, 0x160}, {dt_relrsz, 8}, {dt_rela, 0x160}, {dt_relasz, 24}, {dt_relaent, 16}, {0, 0}});
    CHECK(contains(refusal_of(sized), "text relocation at 0x200"));
}

TEST(refusals_name_every_reason_a_file_has) {
    using apk::container::refusals;

    // What converts has no reason.
    CHECK(refusals(static_program().bytes).empty());

    // A TPIDRRO_EL0 read and a register branch on x18: two reasons, in convert()'s order, each as convert() words it.
    // (br x18 would now be an indirect island, so the still-refused register branch here is ret x18.)
    auto both = static_program();
    both.put<std::uint32_t>(0x218, 0xd53b'd065);  // mrs x5, TPIDRRO_EL0
    both.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18
    const auto reasons = refusals(both.bytes);
    CHECK(reasons.size() == 2);
    CHECK(contains(reasons.at(0), "TPIDRRO_EL0 at 0x218"));
    CHECK(contains(reasons.at(1), "x18 at 0x21c cannot run on iOS: it is a register branch"));

    // Text relocations come before the code's reasons, as in convert().
    auto text_relocations = static_program();
    text_relocations.dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {22 /* DT_TEXTREL */, 0}, {0, 0}});
    text_relocations.put<std::uint32_t>(0x218, 0xd53b'd065);
    const auto relocation_reasons = refusals(text_relocations.bytes);
    CHECK(relocation_reasons.size() == 2);
    CHECK(contains(relocation_reasons.at(0), "text relocation"));
    CHECK(contains(relocation_reasons.at(1), "TPIDRRO_EL0 at 0x218"));

    // A layout reason (a segment both writable and executable) and a TPIDRRO_EL0 read: both are named.
    auto writable_code = static_program();
    writable_code.put<std::uint32_t>(64 + 56 + 4, r | w | x);  // p_flags of the code segment
    writable_code.put<std::uint32_t>(0x218, 0xd53b'd065);
    const auto layout_reasons = refusals(writable_code.bytes);
    CHECK(layout_reasons.size() == 2);
    CHECK(contains(layout_reasons.at(0), "LOAD segment at 0x0 is writable and executable"));
    CHECK(contains(layout_reasons.at(1), "TPIDRRO_EL0 at 0x218"));
    CHECK(layout_reasons.front() == refusal_of(writable_code));

    // Not AArch64: that one reason, as the code of another machine says nothing about the rest.
    auto x86 = static_program();
    x86.put<std::uint16_t>(18, 62);
    CHECK(refusals(x86.bytes).size() == 1);
    CHECK(contains(refusals(x86.bytes).at(0), "x86-64"));

    // No section headers: the layout and the relocations still count, the code scan has nothing to read.
    ElfBuilder without_sections;
    without_sections.segments({{pt_load, r | x, 0, 0, 0x220, 0x220, 0x4000}});
    const auto headerless = refusals(without_sections.bytes);
    CHECK(!headerless.empty());
    CHECK(contains(headerless.back(), "section headers"));

    // What convert() refuses only after the scan (here the container's size) shows too (the review).
    auto big_container = static_program((std::uint64_t{256} << 20) + 0x4800);
    big_container.put<std::uint64_t>(64 + 56 + 40, std::uint64_t{256} << 20);
    big_container.put<std::uint64_t>(64 + 2 * 56 + 16, (std::uint64_t{256} << 20) + 0x42c0);
    big_container.put<std::uint64_t>(64 + 3 * 56 + 16, (std::uint64_t{256} << 20) + 0x42c0);
    CHECK(refusals(big_container.bytes).size() == 1);
    CHECK(contains(refusals(big_container.bytes).at(0), "the container would take"));
    CHECK(refusals(big_container.bytes).front() == refusal_of(big_container));

    // An executable (ET_EXEC) AArch64 file with a TPIDRRO_EL0 read: that one reason, the type's; nothing of its code.
    auto executable = static_program();
    executable.put<std::uint16_t>(16, 2);
    executable.put<std::uint32_t>(0x218, 0xd53b'd065);
    CHECK(refusals(executable.bytes).size() == 1);
    CHECK(contains(refusals(executable.bytes).at(0), "not position independent"));

    // No section headers and a layout reason: both.
    ElfBuilder headerless_layout;
    headerless_layout.segments({{pt_load, r | w | x, 0, 0, 0x220, 0x220, 0x4000}});
    const auto both_reasons = refusals(headerless_layout.bytes);
    CHECK(both_reasons.size() == 2);
    CHECK(contains(both_reasons.at(0), "writable and executable"));
    CHECK(contains(both_reasons.at(1), "section headers"));

    // A scan that fails shows its own reason and nothing from the code it could not read.
    auto beyond = static_program();
    beyond.put<std::uint64_t>(64 + 56 + 32, 0x210);  // p_filesz of the code segment, .text ends at 0x220
    beyond.put<std::uint32_t>(0x218, 0xd53b'd065);
    CHECK(refusals(beyond.bytes).size() == 1);
    CHECK(contains(refusals(beyond.bytes).at(0), "reaches beyond"));

    // Garbage and a file cut short: one reason each, never a throw.
    CHECK(refusals(std::vector<std::byte>(40, std::byte{0x41})).size() == 1);
    const auto whole = static_program();
    const auto cut = std::vector<std::byte>(whole.bytes.begin(), whole.bytes.begin() + 100);
    CHECK(refusals(cut).size() == 1);

    // A name from the file never breaks a reason into lines (the review): control characters become spaces.
    auto named = static_program();
    named.put<std::uint64_t>(64 + 56 + 32, 0x210);
    auto patched = named.bytes;
    const std::string wanted = ".text";
    for (std::size_t at = 0; at + wanted.size() < patched.size(); ++at) {
        if (std::memcmp(patched.data() + at, wanted.data(), wanted.size()) == 0 && at > 0x300) {
            patched[at + 3] = static_cast<std::byte>('\n');
            break;
        }
    }
    const auto one_line = refusals(patched);
    CHECK(one_line.size() == 1);
    CHECK(one_line.at(0).find('\n') == std::string::npos);

    // The count of what repeats: TPIDRRO_EL0 reads, and relocations into code.
    auto many_tpidrro = static_program();
    many_tpidrro.put<std::uint32_t>(0x218, 0xd53b'd065);  // mrs x5, TPIDRRO_EL0
    many_tpidrro.put<std::uint32_t>(0x21c, 0xd53b'd066);  // mrs x6, TPIDRRO_EL0
    CHECK(contains(refusals(many_tpidrro.bytes).at(0), "TPIDRRO_EL0 at 0x218"));
    CHECK(contains(refusals(many_tpidrro.bytes).at(0), "and 1 more"));
    auto two_relocations = static_program();
    two_relocations.dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 16}, {0, 0}});
    two_relocations.put<std::uint64_t>(0x160, 0x200);
    two_relocations.put<std::uint64_t>(0x168, 0x3);
    const auto relocation_text = refusals(two_relocations.bytes);
    CHECK(relocation_text.size() >= 1);
    CHECK(contains(relocation_text.at(0), "text relocation at 0x200"));
    CHECK(contains(relocation_text.at(0), "and 1 more"));

    // The first reason is the one convert() throws, word for word, for each file above that it refuses.
    CHECK(refusals(both.bytes).front() == refusal_of(both));
    CHECK(refusals(text_relocations.bytes).front() == refusal_of(text_relocations));
    CHECK(refusals(x86.bytes).front() == refusal_of(x86));
    CHECK(refusals(without_sections.bytes).front() == refusal_of(without_sections));
    // And a file convert() takes, refusals() gives nothing for.
    CHECK(refusal_of(static_program()).empty());
}

// ADR-0072 decision 3 b: a library that exports ffi_closure_alloc, ffi_closure_free and ffi_prep_closure_loc has their
// first instructions redirected, so that the loader's override reaches calls by direct branch, which no relocation
// names.
// The entry becomes a branch to the hook stub, which loads its runtime slot and branches on; the original stub, 8 bytes
// before it, runs the displaced instruction and continues at entry + 4.
constexpr std::uint32_t ffi_first_words[] = {
    0xa9bf'7bfd,  // stp x29, x30, [sp, #-16]!
    0xd65f'03c0,  // ret
    0xa9bf'7bfd,  // stp x29, x30, [sp, #-16]!
    0xd65f'03c0,  // ret
    0xb940'0028,  // ldr w8, [x1]
    0xd65f'03c0,  // ret
    0xd503'201f,  // nop
    0xd503'201f,  // nop
};

ElfBuilder ffi_program(std::initializer_list<SymbolEntry> symbols) {
    auto builder = static_program(0x4800, symbols);
    std::memcpy(builder.bytes.data() + 0x200, ffi_first_words, sizeof ffi_first_words);
    return builder;
}

std::vector<apk_entry_hook> entry_hook_table(const apk_container_info &info, const std::vector<std::byte> &bytes) {
    std::vector<apk_entry_hook> hooks(info.entry_hook_count);
    if (!hooks.empty()) {
        std::memcpy(hooks.data(), bytes.data() + info.entry_hooks, hooks.size() * sizeof(apk_entry_hook));
    }
    return hooks;
}

TEST(hooks_the_three_libffi_entries_of_a_library_that_exports_them) {
    const auto container = convert(ffi_program({{"ffi_closure_alloc", 0x200, 8},
                                                {"ffi_closure_free", 0x208, 8},
                                                {"ffi_prep_closure_loc", 0x210, 8}}));
    const auto info = read_info(container.bytes);
    const auto hooks = entry_hook_table(info, container.bytes);
    CHECK(info.version >= APK_CONTAINER_ENTRY_HOOKS_VERSION);
    CHECK(hooks.size() == 3);
    const std::uint64_t slots = info.runtime_slots;
    for (std::size_t index = 0; index < hooks.size(); ++index) {
        const auto &hook = hooks[index];
        const std::uint64_t address = index == 0 ? 0x200 : index == 1 ? 0x208 : 0x210;
        CHECK(hook.entry == info.image_offset + address);
        // The entry is a branch to the hook stub, 12 bytes after the original stub.
        CHECK(branch_target(container.bytes, hook.entry) == std::optional<std::uint64_t>(hook.original + 12));
        // The original stub: the displaced word, then a branch to entry + 4, then a nop.
        const std::uint32_t first = ffi_first_words[(address - 0x200) / 4];
        CHECK(word_at(container.bytes, hook.original) == first);
        CHECK(branch_target(container.bytes, hook.original + 4) == std::optional<std::uint64_t>(hook.entry + 4));
        CHECK(word_at(container.bytes, hook.original + 8) == 0xd503'201f);
        // The hook stub: adrp x16 and ldr x16, [x16, #slot] of this hook's slot, then br x16.
        const std::uint64_t slot = slots + offsetof(apk_runtime_slots, hook) + 8 * index;
        const std::uint64_t at = hook.original + 12;
        const auto adrp = word_at(container.bytes, at);
        const auto ldr = word_at(container.bytes, at + 4);
        CHECK((adrp & 0x9f00'001f) == 0x9000'0010);
        CHECK(ldr == (0xf940'0000u | (static_cast<std::uint32_t>((slot & 0xfff) / 8) << 10) | (16u << 5) | 16u));
        CHECK(word_at(container.bytes, at + 8) == 0xd61f'0200);  // br x16
    }
    if (hooks.size() == 3) {
        CHECK(std::string(hooks[0].name) == "ffi_closure_alloc");
        CHECK(std::string(hooks[1].name) == "ffi_closure_free");
        CHECK(std::string(hooks[2].name) == "ffi_prep_closure_loc");
    }
    // The three words are the only ones changed at the entries.
    CHECK(word_at(container.bytes, info.image_offset + 0x204) == 0xd65f'03c0);
}

TEST(runs_a_conditional_branch_as_the_first_instruction_of_a_libffi_entry_in_its_stub) {
    // clang starts ffi_closure_alloc with `cbz x1, <return NULL>`: taken, the stub goes where the function would; not
    // taken, on to the function's second word.
    auto builder = ffi_program({{"ffi_closure_alloc", 0x200, 8}, {"ffi_closure_free", 0x208, 8},
                                {"ffi_prep_closure_loc", 0x210, 8}});
    builder.put<std::uint32_t>(0x200, 0xb400'00c1);  // cbz x1, 0x218
    const auto container = convert(builder);
    const auto info = read_info(container.bytes);
    const auto hooks = entry_hook_table(info, container.bytes);
    CHECK(hooks.size() == 3);
    if (hooks.size() == 3) {
        const auto original = hooks[0].original;
        CHECK(word_at(container.bytes, original) == (0xb400'0001u | (2u << 5)));  // cbz x1, two words ahead
        CHECK(branch_target(container.bytes, original + 4) == std::optional<std::uint64_t>(hooks[0].entry + 4));
        CHECK(branch_target(container.bytes, original + 8) == std::optional<std::uint64_t>(info.image_offset + 0x218));
    }
}

TEST(leaves_a_library_without_all_three_libffi_names_unhooked) {
    const auto two = convert(ffi_program({{"ffi_closure_alloc", 0x200, 8}, {"ffi_closure_free", 0x208, 8}}));
    CHECK(read_info(two.bytes).entry_hook_count == 0);
    CHECK(word_at(two.bytes, read_info(two.bytes).image_offset + 0x200) == 0xa9bf'7bfd);
    const auto none = convert(static_program());
    CHECK(read_info(none.bytes).entry_hook_count == 0);
}

TEST(leaves_a_library_unhooked_whose_libffi_entry_cannot_run_in_a_stub) {
    // A pc-relative first instruction would run at the wrong address in the original stub: no hooks at all, the
    // library is converted as it is.
    auto builder = ffi_program({{"ffi_closure_alloc", 0x200, 8}, {"ffi_closure_free", 0x208, 8},
                                {"ffi_prep_closure_loc", 0x210, 8}});
    builder.put<std::uint32_t>(0x208, 0x9000'0008);  // adrp x8, ...
    const auto container = convert(builder);
    const auto info = read_info(container.bytes);
    CHECK(info.entry_hook_count == 0);
    CHECK(word_at(container.bytes, info.image_offset + 0x200) == 0xa9bf'7bfd);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: container_test <scratch-directory>\n");
        return 2;
    }
    // dyld records the canonical path of a loaded image, and the tests look images up by path.
    std::filesystem::create_directories(argv[1]);
    scratch = std::filesystem::canonical(argv[1]);
    return apk::testing::run_all();
}
