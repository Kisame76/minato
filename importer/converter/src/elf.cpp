#include "apkconv/elf.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace apk::elf {
namespace {

static_assert(std::endian::native == std::endian::little, "the reader copies little-endian fields as they are");

// On-disk layouts from the ELF gABI (Elf64_Ehdr and Elf64_Phdr).
struct RawFileHeader {
    unsigned char ident[16];
    std::uint16_t type;
    std::uint16_t machine;
    std::uint32_t version;
    std::uint64_t entry;
    std::uint64_t program_header_offset;
    std::uint64_t section_header_offset;
    std::uint32_t flags;
    std::uint16_t header_size;
    std::uint16_t program_header_size;
    std::uint16_t program_header_count;
    std::uint16_t section_header_size;
    std::uint16_t section_header_count;
    std::uint16_t section_name_index;
};
static_assert(sizeof(RawFileHeader) == 64);

struct RawProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t virtual_address;
    std::uint64_t physical_address;
    std::uint64_t file_size;
    std::uint64_t memory_size;
    std::uint64_t alignment;
};
static_assert(sizeof(RawProgramHeader) == 56);

constexpr unsigned char magic[] = {0x7f, 'E', 'L', 'F'};
constexpr std::size_t ident_class = 4;
constexpr std::size_t ident_data = 5;
constexpr std::size_t ident_version = 6;
constexpr unsigned char class_32 = 1;
constexpr unsigned char class_64 = 2;
constexpr unsigned char data_little_endian = 1;
constexpr unsigned char version_current = 1;
constexpr std::uint16_t program_header_count_extended = 0xffff;  // PN_XNUM

/// Whether [offset, offset + size) lies within `file_size` bytes, without overflowing.
bool fits(std::uint64_t offset, std::uint64_t size, std::uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

/// Copies a T from `offset`; the caller has checked that it fits.
template <typename T>
T copy_at(std::span<const std::byte> bytes, std::uint64_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

Segment read_segment(std::span<const std::byte> bytes, std::uint64_t header_offset, std::size_t index) {
    const auto raw = copy_at<RawProgramHeader>(bytes, header_offset);
    const Segment segment{
        .type = SegmentType{raw.type},
        .flags = raw.flags,
        .offset = raw.offset,
        .address = raw.virtual_address,
        .file_size = raw.file_size,
        .memory_size = raw.memory_size,
        .alignment = raw.alignment,
    };
    if (segment.type == SegmentType::null) return segment;

    if (!fits(segment.offset, segment.file_size, bytes.size())) {
        throw FormatError(std::format("segment #{} ({:#x} bytes at offset {:#x}) lies outside the file", index,
                                      segment.file_size, segment.offset));
    }
    if (segment.type == SegmentType::load) {
        if (segment.file_size > segment.memory_size) {
            throw FormatError(std::format("LOAD segment #{} has more bytes in the file than in memory", index));
        }
        if (segment.alignment > 1 && !std::has_single_bit(segment.alignment)) {
            throw FormatError(
                std::format("LOAD segment #{} has alignment {:#x}, not a power of two", index, segment.alignment));
        }
    }
    return segment;
}

std::string read_interpreter(std::span<const std::byte> bytes, const Segment &segment) {
    const auto path = bytes.subspan(segment.offset, segment.file_size);
    const auto end = std::ranges::find(path, std::byte{0});
    if (end == path.end()) throw FormatError("the interpreter path in PT_INTERP has no terminating zero");
    return std::string(reinterpret_cast<const char *>(path.data()), static_cast<std::size_t>(end - path.begin()));
}

}  // namespace

File read(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(RawFileHeader)) {
        throw FormatError(std::format("{} bytes are too few for an ELF file header", bytes.size()));
    }
    const auto header = copy_at<RawFileHeader>(bytes, 0);
    if (std::memcmp(header.ident, magic, sizeof magic) != 0) throw FormatError("not an ELF file");
    if (header.ident[ident_class] == class_32) {
        throw FormatError("32-bit ELF file; only 64-bit code (arm64-v8a) is supported");
    }
    if (header.ident[ident_class] != class_64) throw FormatError("unknown ELF class");
    if (header.ident[ident_data] != data_little_endian) throw FormatError("big-endian ELF file");
    if (header.ident[ident_version] != version_current) throw FormatError("unknown ELF version");

    File file{
        .type = FileType{header.type},
        .machine = header.machine,
        .entry = header.entry,
        .segments = {},
        .interpreter = {},
    };

    const std::uint16_t count = header.program_header_count;
    if (count == program_header_count_extended) {
        throw FormatError("more than 65534 program headers are not supported");
    }
    if (count > 0) {
        if (header.program_header_size != sizeof(RawProgramHeader)) {
            throw FormatError(std::format("program headers have {} bytes instead of {}", header.program_header_size,
                                          sizeof(RawProgramHeader)));
        }
        if (!fits(header.program_header_offset, std::uint64_t{count} * sizeof(RawProgramHeader), bytes.size())) {
            throw FormatError("the program header table lies outside the file");
        }
    }
    file.segments.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        file.segments.push_back(
            read_segment(bytes, header.program_header_offset + index * sizeof(RawProgramHeader), index));
    }

    const auto interpreter = std::ranges::find(file.segments, SegmentType::interp, &Segment::type);
    if (interpreter != file.segments.end()) file.interpreter = read_interpreter(bytes, *interpreter);
    return file;
}

const Segment *first_misaligned_load(const File &file, std::uint64_t page_size) {
    for (const auto &segment : file.segments) {
        if (segment.type != SegmentType::load) continue;
        // Modulo 2^64 the difference stays correct, because page_size is a power of two.
        if (segment.alignment < page_size || (segment.address - segment.offset) % page_size != 0) return &segment;
    }
    return nullptr;
}

std::string_view name(SegmentType type) {
    switch (type) {
        case SegmentType::null: return "NULL";
        case SegmentType::load: return "LOAD";
        case SegmentType::dynamic: return "DYNAMIC";
        case SegmentType::interp: return "INTERP";
        case SegmentType::note: return "NOTE";
        case SegmentType::shlib: return "SHLIB";
        case SegmentType::phdr: return "PHDR";
        case SegmentType::tls: return "TLS";
        case SegmentType::gnu_eh_frame: return "GNU_EH_FRAME";
        case SegmentType::gnu_stack: return "GNU_STACK";
        case SegmentType::gnu_relro: return "GNU_RELRO";
        case SegmentType::gnu_property: return "GNU_PROPERTY";
    }
    return {};
}

std::string machine_name(std::uint16_t machine) {
    switch (machine) {
        case 3: return "x86";
        case 40: return "ARM";
        case 62: return "x86-64";
        case machine_aarch64: return "AArch64";
        case 243: return "RISC-V";
        default: return std::format("machine {}", machine);
    }
}

}  // namespace apk::elf
