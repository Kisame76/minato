// M6 b) (ADR-0053): an oat file converts into a container that dyld loads, signed as a .so is. The file is
// fixtures/oat/verify.odex, written by dex2oat64 of apk-runner-35 with --compiler-filter=verify for a test app
// (tools/make-oat.sh): ET_DYN, 16 KiB segments, no relocations, no DT_NEEDED, "oat\n244" at oatdata (0x4000), and a
// NOBITS segment .dex (oatdex, 0x8000) over which ART maps the vdex with MAP_FIXED. The container keeps every ELF
// address at its distance from the others, so the loaded image holds oatdata and the .dex range as the oat file
// says (the vdex of ART lands where the oat header expects it).
#include "apkconv/codesign.h"
#include "apkconv/container.h"

#include "APKRuntime/container_format.h"
#include "check.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace apk::testing;

std::filesystem::path scratch;
std::filesystem::path odex_path;

// The oat file's symbols, as llvm-readelf shows them for fixtures/oat/verify.odex.
constexpr std::uint64_t oatdata = 0x4000, oatdex = 0x8000, oatdex_size = 0x60, page = 0x4000;

std::vector<std::byte> read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> result(bytes.size());
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

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

int protection(std::uintptr_t address) {
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
}

TEST(an_oat_file_converts_loads_and_keeps_the_distance_to_its_dex_range) {
    const auto elf = read_file(odex_path);
    CHECK(!elf.empty());
    if (elf.empty()) return;
    apk::container::Container container;
    try {
        container = apk::container::convert(elf, {.install_name = "@rpath/base.odex.dylib"});
    } catch (const std::exception &failure) {
        apk::testing::fail(std::source_location::current(), "the converter refused the oat file: ", failure.what());
        return;
    }
    apk::codesign::sign(container.bytes, {.identifier = "oat-test"});
    const auto path = scratch / "base.odex.dylib";
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
    const auto at = [&](std::uint64_t address) {
        return reinterpret_cast<std::uintptr_t>(header) + info.image_offset + (address - info.min_vaddr);
    };
    // oatdata: the oat header's magic and version, as ART's OatHeader::IsValid reads them.
    CHECK(std::memcmp(reinterpret_cast<const void *>(at(oatdata)), "oat\n244\0", 8) == 0);
    // The .dex range lies exactly where the ELF puts it, a page after the oat data's last word, and is mapped
    // (zero-filled) and not writable; the page before it is the oat data's.
    CHECK(at(oatdex) - at(oatdata) == oatdex - oatdata);
    // The container's __TEXT run holds the first read-only data and dyld maps it r-x, so "not writable and readable".
    CHECK((protection(at(oatdex)) & (VM_PROT_READ | VM_PROT_WRITE)) == VM_PROT_READ);
    CHECK((protection(at(oatdex - 1)) & (VM_PROT_READ | VM_PROT_WRITE)) == VM_PROT_READ);
    for (std::uint64_t offset = 0; offset < oatdex_size; offset += 8) {
        CHECK(*reinterpret_cast<const std::uint8_t *>(at(oatdex) + offset) == 0);
    }
    // ART maps the vdex over that range with MAP_FIXED, private, read-only: it must work and land in place.
    void *const mapped = mmap(reinterpret_cast<void *>(at(oatdex)), page, PROT_READ, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                              -1, 0);
    CHECK(mapped == reinterpret_cast<void *>(at(oatdex)));
    // And the oat data before it is untouched.
    CHECK(std::memcmp(reinterpret_cast<const void *>(at(oatdata)), "oat\n244\0", 8) == 0);
    dlclose(handle);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: oat_test <scratch folder> <verify.odex>\n");
        return 2;
    }
    scratch = argv[1];
    odex_path = argv[2];
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    return apk::testing::run_all();
}
