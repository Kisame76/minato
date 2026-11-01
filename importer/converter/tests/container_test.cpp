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
ElfBuilder static_program(std::uint64_t relocated_address = 0x4800) {
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
    });
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

// ADR-0020 X4: the island table has one entry per x18 island, sorted by address, with what the
// runtime needs to send a thread that stopped inside the island back to the guest.
TEST(the_island_table_describes_every_x18_island) {
    auto builder = static_program();
    builder.put<std::uint32_t>(0x218, 0x9100'0640);  // add x0, x18, #1: reads x18
    builder.put<std::uint32_t>(0x21c, 0xaa13'03f2);  // mov x18, x19: writes x18
    const auto singles = convert(builder);
    const auto image = read_info(singles.bytes).image_offset;
    CHECK(read_info(singles.bytes).version == 5);
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
TEST(the_container_records_a_no_save_island_at_version_5) {
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
    CHECK(info.version == 5);
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
    CHECK(info.version == APK_CONTAINER_VERSION && APK_CONTAINER_VERSION == 5);
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

TEST(refuses_what_cannot_run) {

    auto darwin_syscall = static_program();
    darwin_syscall.put<std::uint32_t>(0x218, 0xd400'1001);  // svc #0x80
    CHECK_THROWS(Incompatible, convert(darwin_syscall), "svc #0x80 at 0x218");

    auto tpidrro_read = static_program();
    tpidrro_read.put<std::uint32_t>(0x218, 0xd53b'd065);  // mrs x5, TPIDRRO_EL0: reads the Darwin TSD base
    CHECK_THROWS(Incompatible, convert(tpidrro_read), "TPIDRRO_EL0 at 0x218");

    auto register_branch = static_program();
    register_branch.put<std::uint32_t>(0x218, 0xd61f'0240);  // br x18: an indirect island now (X2e)
    register_branch.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18: still refused, no island follows it
    CHECK_THROWS(Incompatible, convert(register_branch), "x18 at 0x21c cannot run on iOS: it is a register branch");
    // A jump table whose table lies in executable code stays refused: the x18 pass scans the table as
    // code and could rewrite an entry, so the br must not degrade to an indirect island (step C C2b).
    CHECK_THROWS(Incompatible, convert(jump_table_program(shf_alloc | shf_execinstr)),
                 "x18 at 0x21c cannot run on iOS: it is a register branch whose table is not in read-only data");
    // More than one use: every reason with its count and its first addresses, the most common first, so that one
    // refusal names all that a library still needs.
    auto many = static_program();
    many.put<std::uint32_t>(0x204, 0x4832'7c82);  // casp x18, x19, x2, x3, [x4]
    many.put<std::uint32_t>(0x208, 0x4832'7c82);
    for (const std::uint32_t at : {0x20cu, 0x210u, 0x214u, 0x218u}) {
        many.put<std::uint32_t>(at, 0x9100'025f);  // mov sp, x18
    }
    CHECK_THROWS(Incompatible, convert(many),
                 "x18 cannot run on iOS in 6 places: 4 where it moves the stack pointer, below which the island keeps "
                 "its registers (0x20c, 0x210, 0x214 and 1 more); 2 where x18 has no register field of its own in it "
                 "(0x204 and 0x208)");

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
    // rewritten sites would lie outside the container's bytes (the lead's reviewer's finding).
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

/// What convert() throws for `builder`, "" when it converts.
std::string refusal_of(const ElfBuilder &builder) {
    try {
        (void)convert(builder);
    } catch (const Incompatible &refusal) {
        return refusal.what();
    }
    return "";
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
    // 4 MiB of bitmaps name some 26 million relocations; the reason is made once, for the first (the lead's reviewer:
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
    // convert() named the first bad relocation before it looked at the later tables (the lead's reviewer): a RELR
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

    // A Darwin syscall and a register branch on x18: two reasons, in convert()'s order, each as convert() words it.
    // (br x18 would now be an indirect island, so the still-refused register branch here is ret x18.)
    auto both = static_program();
    both.put<std::uint32_t>(0x218, 0xd400'1001);  // svc #0x80
    both.put<std::uint32_t>(0x21c, 0xd65f'0240);  // ret x18
    const auto reasons = refusals(both.bytes);
    CHECK(reasons.size() == 2);
    CHECK(contains(reasons.at(0), "svc #0x80 at 0x218"));
    CHECK(contains(reasons.at(1), "x18 at 0x21c cannot run on iOS: it is a register branch"));

    // Text relocations come before the code's reasons, as in convert().
    auto text_relocations = static_program();
    text_relocations.dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {22 /* DT_TEXTREL */, 0}, {0, 0}});
    text_relocations.put<std::uint32_t>(0x218, 0xd400'1001);
    const auto relocation_reasons = refusals(text_relocations.bytes);
    CHECK(relocation_reasons.size() == 2);
    CHECK(contains(relocation_reasons.at(0), "text relocation"));
    CHECK(contains(relocation_reasons.at(1), "svc #0x80 at 0x218"));

    // A layout reason (a segment both writable and executable) and a Darwin syscall: both are named.
    auto writable_code = static_program();
    writable_code.put<std::uint32_t>(64 + 56 + 4, r | w | x);  // p_flags of the code segment
    writable_code.put<std::uint32_t>(0x218, 0xd400'1001);
    const auto layout_reasons = refusals(writable_code.bytes);
    CHECK(layout_reasons.size() == 2);
    CHECK(contains(layout_reasons.at(0), "LOAD segment at 0x0 is writable and executable"));
    CHECK(contains(layout_reasons.at(1), "svc #0x80 at 0x218"));
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

    // What convert() refuses only after the scan (here the container's size) shows too (the lead's reviewer).
    auto big_container = static_program((std::uint64_t{256} << 20) + 0x4800);
    big_container.put<std::uint64_t>(64 + 56 + 40, std::uint64_t{256} << 20);
    big_container.put<std::uint64_t>(64 + 2 * 56 + 16, (std::uint64_t{256} << 20) + 0x42c0);
    big_container.put<std::uint64_t>(64 + 3 * 56 + 16, (std::uint64_t{256} << 20) + 0x42c0);
    CHECK(refusals(big_container.bytes).size() == 1);
    CHECK(contains(refusals(big_container.bytes).at(0), "the container would take"));
    CHECK(refusals(big_container.bytes).front() == refusal_of(big_container));

    // An executable (ET_EXEC) AArch64 file with a Darwin syscall: that one reason, the type's; nothing about its code.
    auto executable = static_program();
    executable.put<std::uint16_t>(16, 2);
    executable.put<std::uint32_t>(0x218, 0xd400'1001);
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
    beyond.put<std::uint32_t>(0x218, 0xd400'1001);
    CHECK(refusals(beyond.bytes).size() == 1);
    CHECK(contains(refusals(beyond.bytes).at(0), "reaches beyond"));

    // Garbage and a file cut short: one reason each, never a throw.
    CHECK(refusals(std::vector<std::byte>(40, std::byte{0x41})).size() == 1);
    const auto whole = static_program();
    const auto cut = std::vector<std::byte>(whole.bytes.begin(), whole.bytes.begin() + 100);
    CHECK(refusals(cut).size() == 1);

    // A name from the file never breaks a reason into lines (the lead's reviewer): control characters become spaces.
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

    // The count of what repeats: Darwin syscalls, and relocations into code.
    auto many_svc = static_program();
    many_svc.put<std::uint32_t>(0x218, 0xd400'1001);
    many_svc.put<std::uint32_t>(0x21c, 0xd400'1001);
    CHECK(contains(refusals(many_svc.bytes).at(0), "svc #0x80 at 0x218"));
    CHECK(contains(refusals(many_svc.bytes).at(0), "and 1 more"));
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
