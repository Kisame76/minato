// Reader for the ELF files the converter takes in: 64-bit, little-endian (ELF gABI, AArch64 ELF ABI).
// It reads the file header, the program headers, the section headers and the dynamic table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace apk::elf {

/// e_type
enum class FileType : std::uint16_t {
    none = 0,
    relocatable = 1,
    executable = 2,  // placed at fixed addresses
    shared = 3,      // shared object or position-independent executable
    core = 4,
};

/// e_machine of arm64-v8a; other machines are kept as numbers.
inline constexpr std::uint16_t machine_aarch64 = 183;

/// p_type
enum class SegmentType : std::uint32_t {
    null = 0,
    load = 1,
    dynamic = 2,
    interp = 3,
    note = 4,
    shlib = 5,
    phdr = 6,
    tls = 7,
    gnu_eh_frame = 0x6474'e550,
    gnu_stack = 0x6474'e551,
    gnu_relro = 0x6474'e552,
    gnu_property = 0x6474'e553,
};

/// p_flags
inline constexpr std::uint32_t segment_executable = 0x1;
inline constexpr std::uint32_t segment_writable = 0x2;
inline constexpr std::uint32_t segment_readable = 0x4;

struct Segment {
    SegmentType type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t address;  // p_vaddr
    std::uint64_t file_size;
    std::uint64_t memory_size;
    std::uint64_t alignment;
};

struct File {
    FileType type;
    std::uint16_t machine;
    std::uint64_t entry;
    std::uint64_t program_header_offset;  // e_phoff; the loader passes their address as AT_PHDR
    std::vector<Segment> segments;
    std::string interpreter;  // path from PT_INTERP, empty without one
};

/// sh_type values the converter looks at.
inline constexpr std::uint32_t section_type_null = 0;
inline constexpr std::uint32_t section_type_symtab = 2;   // .symtab
inline constexpr std::uint32_t section_type_nobits = 8;
inline constexpr std::uint32_t section_type_dynsym = 11;  // .dynsym

/// sh_flags
inline constexpr std::uint64_t section_writable = 0x1;
inline constexpr std::uint64_t section_allocated = 0x2;
inline constexpr std::uint64_t section_executable = 0x4;

struct Section {
    std::string name;
    std::uint32_t type;
    std::uint64_t flags;
    std::uint64_t address;  // sh_addr
    std::uint64_t offset;
    std::uint64_t size;
    std::uint64_t addralign;  // sh_addralign, the alignment the section's content needs (0 or a power of two)
};

/// A named, defined symbol (Elf64_Sym), for naming the code a refusal points into -- not for linking.
struct Symbol {
    std::string name;
    std::uint64_t value = 0;  // st_value: the address in a shared object
    std::uint64_t size = 0;   // st_size: 0 when the symbol carries none
};

/// One entry of the dynamic table (Elf64_Dyn).
struct DynamicEntry {
    std::int64_t tag;
    std::uint64_t value;
};

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Reads the file header and the program headers. Throws FormatError when `bytes` is not a 64-bit
/// little-endian ELF file or when a header points outside of it.
File read(std::span<const std::byte> bytes);

/// Reads the section headers and their names. Empty when the file has none. The converter only
/// rewrites code inside executable sections (ADR-0010), so it needs them; Android's linker also
/// insists on a valid table for apps that target API 26 and later.
std::vector<Section> read_sections(std::span<const std::byte> bytes);

/// The named, defined symbols of `bytes` from .symtab, else .dynsym, else empty (a stripped library). For naming
/// the code a refusal falls in, not for linking: undefined, nameless and non-section (absolute/common) entries are
/// left out. Never throws: a broken or partial table yields the entries read so far, or none.
std::vector<Symbol> read_symbols(std::span<const std::byte> bytes);

/// The entries of PT_DYNAMIC up to DT_NULL. Empty without PT_DYNAMIC. `file` comes from read(bytes).
std::vector<DynamicEntry> read_dynamic(const File &file, std::span<const std::byte> bytes);

/// The LOAD segment that holds [address, address + size) in memory, including its zero-filled
/// part. Null when no single LOAD segment covers the whole range.
const Segment *load_containing(const File &file, std::uint64_t address, std::uint64_t size);

/// Page size of iOS and of Android devices with 16 KB pages (ADR-0007).
inline constexpr std::uint64_t page_size_16k = 16 * 1024;

/// The first LOAD segment that cannot be mapped with pages of `page_size`: aligned to less, or at an
/// address that differs from its file offset modulo `page_size`. Null if every LOAD segment fits.
const Segment *first_misaligned_load(const File &file, std::uint64_t page_size = page_size_16k);

/// "LOAD", "GNU_STACK", …; empty for types this reader does not know.
std::string_view name(SegmentType type);

/// "AArch64", "x86-64", …; "machine <number>" for others.
std::string machine_name(std::uint16_t machine);

}  // namespace apk::elf
