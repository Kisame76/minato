// The converter: one ELF file of an APK in, one unsigned container dylib out (ADR-0008, ADR-0010,
// ADR-0019). The layout is documented in APKRuntime/container_format.h, which the loader shares.
#pragma once

#include "apkconv/macho.h"
#include "apkconv/rewrite.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace apk::container {

struct Options {
    /// LC_ID_DYLIB, e.g. "@rpath/libgame.so.dylib".
    std::string install_name;
    macho::Platform platform = macho::Platform::macos;
    std::uint32_t platform_version = 26u << 16;
};

/// What the converter did with one file. ADR-0010: every change to the code is counted.
struct Report {
    bool is_static = false;          // no PT_INTERP and no DT_NEEDED
    std::uint64_t code_bytes = 0;    // bytes of the executable sections, all of them scanned
    std::uint32_t thread_pointer_reads = 0;
    std::uint32_t thread_pointer_writes = 0;
    std::uint32_t syscalls = 0;
    std::uint32_t x18_writes = 0;    // harmless: XNU may clear x18, but nothing reads it (ADR-0019)
    std::uint32_t x18_saves = 0;     // harmless: x18 stored in a dump of all registers
    std::uint64_t relocations = 0;   // RELR addresses plus RELA entries, all in writable memory
    std::uint64_t image_offset = 0;  // where the ELF image starts in the container
    std::uint64_t stub_bytes = 0;    // islands and veneers
};

struct Container {
    std::vector<std::byte> bytes;  // unsigned: codesign on the Mac, zsign on the iPhone (M2b)
    Report report;
};

/// The file is a valid ELF file, but it cannot run on iOS as it is. The message says why, for
/// the import log and the "Incompatible" status.
class Incompatible : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Converts `elf`. Throws Incompatible, or elf::FormatError when the file is broken.
Container convert(std::span<const std::byte> elf, const Options &options);

/// The code of a file as the rewriter sees it: what it finds in the executable sections, the only
/// bytes known to be instructions (R1).
struct CodeScan {
    rewrite::Findings findings;
    std::uint64_t code_bytes = 0;
};

/// Scans the executable sections of `elf` without converting it. Throws Incompatible when there are
/// no section headers or a section is not where its segment says, or elf::FormatError.
CodeScan scan_code(std::span<const std::byte> elf);

}  // namespace apk::container
