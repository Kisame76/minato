// The converter: one ELF file of an APK in, one unsigned container dylib out (ADR-0008, ADR-0010,
// ADR-0019). The layout is documented in APKRuntime/container_format.h, which the loader shares.
#pragma once

#include "apkconv/elf.h"
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
    std::uint32_t x18_writes = 0;    // instructions that only write x18
    std::uint32_t x18_saves = 0;     // x18 stored in a dump of all registers
    std::uint32_t x18_islands = 0;   // instructions run with a scratch register for x18 (ADR-0020)
    std::uint64_t relocations = 0;   // RELR addresses plus RELA entries, all in writable memory
    std::uint64_t image_offset = 0;  // where the ELF image starts in the container
    std::uint64_t image_shift = 0;   // how far a 4 KiB layout moved it past a 16 KiB page (ADR-0007)
    std::uint64_t stub_bytes = 0;    // islands and veneers
    std::uint64_t island_table_bytes = 0;  // one entry per x18 island (ADR-0020 X4)
};

struct Container {
    std::vector<std::byte> bytes;  // unsigned; codesign.h signs it (ADR-0022)
    Report report;
};

/// The file is a valid ELF file, but it cannot run on iOS as it is. The message says why, for
/// the import log and the "Incompatible" status.
class Incompatible : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Where an ELF image goes in 16 KiB pages (ADR-0007 decision 2, as amended 2026-10-07). Its LOAD
/// segments are aligned to 4 KiB or more, the non-writable ones on 4 KiB pages below the writable
/// ones. The whole image moves by `shift` so that its first writable 4 KiB page starts a 16 KiB page,
/// as AOSP's 16 KiB compat mode moves its load bias: ADRP counts 4 KiB pages, so no instruction or
/// table in the image changes. Pages below that boundary hold code, those above it data.
struct PageLayout {
    std::uint64_t min_vaddr = 0;       // the first LOAD segment's 16 KiB page
    std::uint64_t writable_start = 0;  // the first writable LOAD segment's page, else `end`
    std::uint64_t end = 0;             // past the last LOAD segment, the data in whole 16 KiB pages
    std::uint64_t shift = 0;           // 0, 4, 8 or 12 KiB
};

/// The layout of `file`. Throws Incompatible with the reason when no shift fits it.
PageLayout page_layout(const elf::File &file);

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
