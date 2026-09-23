// Checks that the container writer produces a dylib dyld really loads: the contents land at the
// right addresses, code in __TEXT runs, __DATA is writable and its zero-filled tail is zero, and
// the metadata section is found the way the loader finds it (ADR-0008, ADR-0019).
#include "apkconv/macho.h"

#include "check.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace apk::macho;

constexpr std::uint64_t text_size = page_size;
constexpr std::uint64_t info_offset = 0x800;  // after the header and the load commands
constexpr std::uint64_t code_offset = 0x900;
constexpr char info_content[] = "APKCONT-test";

// mov w0, #42 ; ret
constexpr std::uint32_t answer_code[] = {0x5280'0540, 0xd65f'03c0};

std::filesystem::path scratch;

/// A container with one page of __TEXT, two pages of __DATA (the second one zero filled) and a
/// small __LINKEDIT, as the real converter lays it out.
Dylib test_dylib() {
    return Dylib{
        .install_name = "@rpath/apk-macho-test.dylib",
        .platform = Platform::macos,
        .version = (26u << 16),
        .uuid = {0x41, 0x50, 0x4b, 0x01, 0x02, 0x03, 0x40, 0x04, 0x80, 0x05, 6, 7, 8, 9, 10, 11},
        .segments =
            {
                Segment{.name = "__TEXT",
                        .address = 0,
                        .vm_size = text_size,
                        .file_offset = 0,
                        .file_size = text_size,
                        .protection = protection_read | protection_execute,
                        .sections = {Section{.name = "__apk_info",
                                             .address = info_offset,
                                             .size = sizeof info_content}}},
                Segment{.name = "__DATA",
                        .address = text_size,
                        .vm_size = 2 * page_size,
                        .file_offset = text_size,
                        .file_size = page_size,
                        .protection = protection_read | protection_write,
                        .sections = {}},
                Segment{.name = "__LINKEDIT",
                        .address = text_size + 2 * page_size,
                        .vm_size = page_size,
                        .file_offset = text_size + page_size,
                        .file_size = 16,
                        .protection = protection_read,
                        .sections = {}},
            },
    };
}

std::vector<std::byte> build_image(const Dylib &dylib) {
    const auto &last = dylib.segments.back();
    std::vector<std::byte> image(last.file_offset + last.file_size, std::byte{0});
    std::memcpy(image.data() + info_offset, info_content, sizeof info_content);
    std::memcpy(image.data() + code_offset, answer_code, sizeof answer_code);
    // A marker in the file-backed part of __DATA; the second page has none and must read as zero.
    image[text_size] = std::byte{0xa5};
    write_header(dylib, image);
    return image;
}

/// Writes `image`, signs it ad hoc (nothing runs on arm64 unsigned, F1) and returns the path.
std::filesystem::path write_and_sign(const std::vector<std::byte> &image, const char *name) {
    const auto path = scratch / name;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!file) throw std::runtime_error("cannot write " + path.string());
    }
    const auto command = "codesign --force --sign - --timestamp=none '" + path.string() + "' 2>&1";
    if (std::system(command.c_str()) != 0) throw std::runtime_error("codesign failed for " + path.string());
    return path;
}

const mach_header_64 *find_loaded_image(const std::filesystem::path &path) {
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        if (path == _dyld_get_image_name(index)) {
            return reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(index));
        }
    }
    return nullptr;
}

TEST(header_size_matches_what_is_written) {
    const auto dylib = test_dylib();
    const auto image = build_image(dylib);
    mach_header_64 header;
    std::memcpy(&header, image.data(), sizeof header);
    CHECK(header.magic == MH_MAGIC_64);
    CHECK(header.filetype == MH_DYLIB);
    CHECK(header.ncmds == 8);  // three segments, LC_ID_DYLIB, LC_LOAD_DYLIB, LC_UUID, LC_BUILD_VERSION, LC_SYMTAB
    CHECK(sizeof header + header.sizeofcmds == header_size(dylib));
    CHECK(header_size(dylib) < info_offset);  // the writer must not run into the contents
}

TEST(the_container_loads_and_its_contents_are_where_the_loader_expects_them) {
    const auto dylib = test_dylib();
    const auto path = write_and_sign(build_image(dylib), "apk-macho-test.dylib");

    void *const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        apk::testing::fail(std::source_location::current(), "dlopen failed: ", dlerror());
        return;
    }

    const auto *header = find_loaded_image(path);
    CHECK(header != nullptr);
    if (header == nullptr) return;

    // The loader reads the metadata exactly this way.
    unsigned long size = 0;
    const std::uint8_t *info = getsectiondata(header, "__TEXT", "__apk_info", &size);
    CHECK(info != nullptr);
    CHECK(size == sizeof info_content);
    if (info != nullptr && size == sizeof info_content) {
        CHECK(std::memcmp(info, info_content, sizeof info_content) == 0);
    }

    const auto base = reinterpret_cast<std::uintptr_t>(header);

    // Code in __TEXT runs, so the segment really is executable.
    auto answer = reinterpret_cast<int (*)()>(base + code_offset);
    CHECK(answer() == 42);

    // __DATA keeps its file contents, is writable, and its tail beyond file_size reads as zero.
    auto *data = reinterpret_cast<volatile std::uint8_t *>(base + text_size);
    CHECK(data[0] == 0xa5);
    data[0] = 0x5a;
    CHECK(data[0] == 0x5a);
    CHECK(data[page_size] == 0);
    CHECK(data[2 * page_size - 1] == 0);

    dlclose(handle);
}

TEST(a_layout_dyld_cannot_map_is_rejected) {
    auto unaligned = test_dylib();
    unaligned.segments[1].address += 8;
    std::vector<std::byte> image(1 << 16, std::byte{0});
    CHECK_THROWS(FormatError, write_header(unaligned, image), "page");

    auto overlapping = test_dylib();
    overlapping.segments[1].address = 0;
    CHECK_THROWS(FormatError, write_header(overlapping, image), "order");

    auto without_linkedit = test_dylib();
    without_linkedit.segments.pop_back();
    CHECK_THROWS(FormatError, write_header(without_linkedit, image), "__LINKEDIT");

    auto too_small = test_dylib();
    std::vector<std::byte> tiny(64, std::byte{0});
    CHECK_THROWS(FormatError, write_header(too_small, tiny), "too small");

    // codesign and zsign add LC_CODE_SIGNATURE right behind the load commands, over whatever lies
    // there. A section that starts too close would lose its first bytes to the signature.
    auto no_room = test_dylib();
    no_room.segments[0].sections[0].address = header_size(no_room) + 8;
    CHECK_THROWS(FormatError, write_header(no_room, image), "code signature");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: macho_test <scratch-directory>\n");
        return 2;
    }
    // dyld records the canonical path of a loaded image, and the tests look images up by path.
    std::filesystem::create_directories(argv[1]);
    scratch = std::filesystem::canonical(argv[1]);
    return apk::testing::run_all();
}
