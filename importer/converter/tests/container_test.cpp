// Checks the converter end to end on small ELF files: the container has the layout the loader
// expects (APKRuntime/container_format.h), the code is rewritten exactly at the patterns, dyld
// loads the result, and files that cannot run are refused with a reason.
#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "apkconv/elf.h"

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

TEST(counts_what_only_writes_or_saves_x18) {
    auto builder = static_program();
    builder.put<std::uint32_t>(0x218, 0xaa13'03f2);  // mov x18, x19
    builder.put<std::uint32_t>(0x21c, 0xa949'4c12);  // ldp x18, x19, [x0, #0x90]
    const auto container = convert(builder);
    CHECK(container.report.x18_writes == 2);
    CHECK(container.report.x18_saves == 0);
    CHECK(convert(static_program()).report.x18_writes == 0);
}

TEST(refuses_what_cannot_run) {
    auto four_kib = static_program();
    four_kib.put<std::uint64_t>(64 + 56 + 48, 0x1000);  // p_align of the code segment
    CHECK_THROWS(Incompatible, convert(four_kib), "16 KiB");

    auto darwin_syscall = static_program();
    darwin_syscall.put<std::uint32_t>(0x218, 0xd400'1001);  // svc #0x80
    CHECK_THROWS(Incompatible, convert(darwin_syscall), "svc #0x80 at 0x218");

    auto shadow_call_stack = static_program();
    shadow_call_stack.put<std::uint32_t>(0x218, 0xf800'865e);  // str x30, [x18], #8
    shadow_call_stack.put<std::uint32_t>(0x21c, 0x9100'0640);  // add x0, x18, #1
    CHECK_THROWS(Incompatible, convert(shadow_call_stack), "x18 is read at 0x218 and 1 more place");

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
