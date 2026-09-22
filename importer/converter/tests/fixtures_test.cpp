// Checks what the loader relies on in the NDK fixtures from tools/build-fixtures.sh: static PIEs for
// AArch64 that fit 16 KiB pages. Skipped (exit code 77) when the fixtures are not built.
#include "apkconv/elf.h"

#include "check.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

std::filesystem::path fixtures;

std::vector<std::byte> read_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::byte> bytes(std::filesystem::file_size(path));
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

void check_static_pie(const char *name) {
    const auto file = apk::elf::read(read_file(fixtures / name));
    CHECK(file.machine == apk::elf::machine_aarch64);
    CHECK(file.type == apk::elf::FileType::shared);  // position independent: the loader picks the address
    CHECK(file.interpreter.empty());                 // static: no /system/bin/linker64
    CHECK(apk::elf::first_misaligned_load(file) == nullptr);
    CHECK(std::ranges::any_of(file.segments, [&](const apk::elf::Segment &segment) {
        return segment.type == apk::elf::SegmentType::load && (segment.flags & apk::elf::segment_executable) &&
               segment.address <= file.entry && file.entry - segment.address < segment.memory_size;
    }));
}

TEST(raw_hello_is_a_static_pie) {
    check_static_pie("raw-hello");
}

TEST(hello_is_a_static_pie) {
    check_static_pie("hello");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2 || !std::filesystem::is_directory(argv[1])) {
        std::printf("skipped: no fixtures in %s, build them with tools/build-fixtures.sh\n", argc == 2 ? argv[1] : "?");
        return 77;
    }
    fixtures = argv[1];
    return apk::testing::run_all();
}
