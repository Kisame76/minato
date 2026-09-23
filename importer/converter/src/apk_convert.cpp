// apk-convert: the converter as a command-line tool on the Mac (M2a).
//   apk-convert inspect <elf-file>...        what the loader will see in an ELF file
//   apk-convert convert <elf-file> <dylib>   write the container dylib, unsigned (ADR-0019)
#include "apkconv/container.h"
#include "apkconv/elf.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <filesystem>
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

    // What the rewriter finds in the executable sections (ADR-0010, ADR-0019).
    try {
        const auto scan = apk::container::scan_code(bytes);
        std::size_t thread_pointer_reads = 0, thread_pointer_writes = 0, syscalls = 0;
        for (const auto &site : scan.findings.sites) {
            switch (site.kind) {
                case apk::rewrite::SiteKind::thread_pointer_read: ++thread_pointer_reads; break;
                case apk::rewrite::SiteKind::thread_pointer_write: ++thread_pointer_writes; break;
                case apk::rewrite::SiteKind::syscall: ++syscalls; break;
            }
        }
        const auto &findings = scan.findings;
        std::cout << std::format("  code: {} bytes; TPIDR_EL0: {} reads, {} writes; svc #0: {}, other svc: {}\n",
                                 scan.code_bytes, thread_pointer_reads, thread_pointer_writes, syscalls,
                                 findings.other_syscalls.size())
                  << std::format("  x18: {} reads, {} write{}, {} saved in register dumps", findings.x18_reads.size(),
                                 findings.x18_writes.size(), findings.x18_writes.size() == 1 ? "" : "s",
                                 findings.x18_saves.size());
        if (!findings.x18_reads.empty()) std::cout << std::format(" (first read at {:#x})", findings.x18_reads.front());
        std::cout << '\n';
    } catch (const apk::container::Incompatible &error) {
        std::cout << "  code: " << error.what() << '\n';
    }
}

void write_file(const std::string &path, const std::vector<std::byte> &bytes) {
    // Replace, never overwrite in place: the kernel caches signatures per file (ADR-0008).
    const auto temporary = path + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("cannot write " + temporary);
    }
    std::filesystem::rename(temporary, path);
}

apk::macho::Platform parse_platform(const std::string &name) {
    if (name == "macos") return apk::macho::Platform::macos;
    if (name == "ios") return apk::macho::Platform::ios;
    if (name == "ios-simulator") return apk::macho::Platform::ios_simulator;
    throw std::runtime_error("unknown platform " + name + " (macos, ios, ios-simulator)");
}

int convert(const std::vector<std::string> &arguments) {
    if (arguments.size() < 3) {
        std::cerr << "usage: apk-convert convert <elf-file> <dylib> [--platform macos|ios|ios-simulator]\n";
        return 2;
    }
    const auto &input = arguments[1];
    const auto &output = arguments[2];
    apk::container::Options options{
        .install_name = "@rpath/" + std::filesystem::path(output).filename().string(),
    };
    for (std::size_t index = 3; index < arguments.size(); ++index) {
        if (arguments[index] == "--platform" && index + 1 < arguments.size()) {
            options.platform = parse_platform(arguments[++index]);
        } else {
            throw std::runtime_error("unknown option " + arguments[index]);
        }
    }

    const auto bytes = read_file(input);
    const auto start = std::chrono::steady_clock::now();
    const auto container = apk::container::convert(bytes, options);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    write_file(output, container.bytes);

    const auto &report = container.report;
    std::cout << std::format("{} -> {} ({} bytes, unsigned)\n", input, output, container.bytes.size())
              << std::format("  {}, {} bytes of code scanned\n", report.is_static ? "static" : "dynamic",
                             report.code_bytes)
              << std::format("  rewritten: {} thread pointer reads, {} writes, {} syscalls ({} bytes of stubs)\n",
                             report.thread_pointer_reads, report.thread_pointer_writes, report.syscalls,
                             report.stub_bytes)
              << std::format("  x18: never read; {} write{}, {} saved in register dumps\n", report.x18_writes,
                             report.x18_writes == 1 ? "" : "s", report.x18_saves)
              << std::format("  relocations: {}, ELF image at {:#x}\n", report.relocations, report.image_offset)
              << std::format("  converted in {:.2f} ms\n", elapsed.count());
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    if (!arguments.empty() && arguments[0] == "convert") {
        try {
            return convert(arguments);
        } catch (const apk::container::Incompatible &error) {
            std::cerr << std::format("apk-convert: incompatible: {}\n", error.what());
            return 1;
        } catch (const std::exception &error) {
            std::cerr << std::format("apk-convert: {}\n", error.what());
            return 1;
        }
    }
    if (arguments.size() < 2 || arguments[0] != "inspect") {
        std::cerr << "usage: apk-convert inspect <elf-file>...\n"
                     "       apk-convert convert <elf-file> <dylib> [--platform macos|ios|ios-simulator]\n";
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
