// Reader for the ELF files the converter takes in: 64-bit, little-endian (ELF gABI, AArch64 ELF ABI).
// It reads the file header and the program headers; what the loader needs from the rest follows in M2 step 4.
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
    std::vector<Segment> segments;
    std::string interpreter;  // path from PT_INTERP, empty without one
};

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Reads the file header and the program headers. Throws FormatError when `bytes` is not a 64-bit
/// little-endian ELF file or when a header points outside of it.
File read(std::span<const std::byte> bytes);

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
