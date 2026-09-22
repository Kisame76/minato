// apk-convert: the converter as a command-line tool on the Mac (M2a).
// So far it only reads: `apk-convert inspect` shows what the loader will see in an ELF file.
#include "apkconv/elf.h"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using apk::elf::FileType;
using apk::elf::Segment;
using apk::elf::SegmentType;

std::vector<std::byte> read_file(const std::string &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::system_error(errno, std::generic_category(), "cannot open");
    const auto size = stream.tellg();
    if (size < 0) throw std::runtime_error("cannot determine the size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot read the whole file");
    return bytes;
}

std::string describe(FileType type) {
    switch (type) {
        case FileType::shared: return "shared object or position-independent executable";
        case FileType::executable: return "executable at fixed addresses";
        case FileType::relocatable: return "relocatable object";
        case FileType::core: return "core dump";
        case FileType::none: break;
    }
    return std::format("type {:#x}", static_cast<std::uint16_t>(type));
}

std::string permissions(std::uint32_t flags) {
    std::string text = "---";
    if (flags & apk::elf::segment_readable) text[0] = 'r';
    if (flags & apk::elf::segment_writable) text[1] = 'w';
    if (flags & apk::elf::segment_executable) text[2] = 'x';
    return text;
}

std::string page_verdict(const apk::elf::File &file) {
    const auto *segment = apk::elf::first_misaligned_load(file);
    if (segment == nullptr) {
        const auto loads = std::ranges::count(file.segments, SegmentType::load, &Segment::type);
        return std::format("yes, all {} LOAD segments fit", loads);
    }
    const auto index = segment - file.segments.data();
    if (segment->alignment < apk::elf::page_size_16k) {
        return std::format("no, segment #{} is aligned to only {:#x}", index, segment->alignment);
    }
    return std::format("no, segment #{} starts at {:#x} but at offset {:#x} in the file", index, segment->address,
                       segment->offset);
}

void inspect(const std::string &path) {
    const auto bytes = read_file(path);
    const auto file = apk::elf::read(bytes);

    std::cout << path << '\n'
              << std::format("  ELF64 little-endian, {}, {}\n", apk::elf::machine_name(file.machine),
                             describe(file.type))
              << std::format("  entry {:#x}, interpreter: {}\n", file.entry,
                             file.interpreter.empty() ? "none" : file.interpreter)
              << std::format("  {:<3}{:<14}{:>10}{:>12}{:>12}{:>12}{:>9}  {}\n", "#", "segment", "offset", "address",
                             "file size", "mem size", "align", "flags");
    for (std::size_t index = 0; index < file.segments.size(); ++index) {
        const auto &segment = file.segments[index];
        auto type = std::string(apk::elf::name(segment.type));
        if (type.empty()) type = std::format("{:#x}", static_cast<std::uint32_t>(segment.type));
        std::cout << std::format("  {:<3}{:<14}{:>#10x}{:>#12x}{:>#12x}{:>#12x}{:>#9x}  {}\n", index, type,
                                 segment.offset, segment.address, segment.file_size, segment.memory_size,
                                 segment.alignment, permissions(segment.flags));
    }
    std::cout << "  16 KiB pages: " << page_verdict(file) << '\n';
}

}  // namespace

int main(int argc, char **argv) {
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.size() < 2 || arguments[0] != "inspect") {
        std::cerr << "usage: apk-convert inspect <elf-file>...\n";
        return 2;
    }
    int status = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (index > 1) std::cout << '\n';
        try {
            inspect(arguments[index]);
        } catch (const std::exception &error) {
            std::cerr << std::format("apk-convert: {}: {}\n", arguments[index], error.what());
            status = 1;
        }
    }
    return status;
}
