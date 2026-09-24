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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

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
    CHECK(read_info(singles.bytes).version == 2);
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
        CHECK(window[0].site == read_info(loop_container.bytes).image_offset + 0x200);
        CHECK(window[0].kind == APK_ISLAND_EXCLUSIVE && window[0].words == 9 && window[0].value == 0 &&
              window[0].base == APK_ISLAND_NO_REGISTER && window[0].commit == 6 && window[0].store == 0 &&
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

TEST(refuses_what_cannot_run) {
    auto four_kib = static_program();
    four_kib.put<std::uint64_t>(64 + 56 + 48, 0x1000);  // p_align of the code segment
    CHECK_THROWS(Incompatible, convert(four_kib), "16 KiB");

    auto darwin_syscall = static_program();
    darwin_syscall.put<std::uint32_t>(0x218, 0xd400'1001);  // svc #0x80
    CHECK_THROWS(Incompatible, convert(darwin_syscall), "svc #0x80 at 0x218");

    auto register_branch = static_program();
    register_branch.put<std::uint32_t>(0x218, 0xb400'0052);  // cbz x18, #+8: an island now
    register_branch.put<std::uint32_t>(0x21c, 0xd61f'0240);  // br x18: still refused
    CHECK_THROWS(Incompatible, convert(register_branch), "x18 at 0x21c cannot run on iOS: it is a register branch");
    // A jump table whose table lies in code: the rewriter could have changed it as an instruction.
    CHECK_THROWS(Incompatible, convert(jump_table_program(shf_alloc | shf_execinstr)),
                 "x18 at 0x21c cannot run on iOS: it is a register branch whose table is not in read-only data");

    CHECK_THROWS(Incompatible, convert(static_program(0x100)), "text relocation");
    CHECK_THROWS(Incompatible, convert(static_program(0x9000)), "outside");

    auto text_relocations = static_program();
    text_relocations.dynamic(0x2c0, {{dt_relr, 0x160}, {dt_relrsz, 8}, {22 /* DT_TEXTREL */, 0}, {0, 0}});
    CHECK_THROWS(Incompatible, convert(text_relocations), "text relocation");

    auto x86 = static_program();
    x86.put<std::uint16_t>(18, 62);
    CHECK_THROWS(Incompatible, convert(x86), "x86-64");

    ElfBuilder without_sections;
    without_sections.segments({{pt_load, r | x, 0, 0, 0x220, 0x220, 0x4000}});
    CHECK_THROWS(Incompatible, convert(without_sections), "section headers");
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
