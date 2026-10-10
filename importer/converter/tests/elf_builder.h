// Builds small ELF files for the converter's tests, field by field at the offsets from the ELF
// gABI, independent of the reader's structs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apk::testing {

constexpr std::uint32_t pt_load = 1;
constexpr std::uint32_t pt_dynamic = 2;
constexpr std::uint32_t pt_interp = 3;
constexpr std::uint32_t pt_phdr = 6;
constexpr std::uint32_t pt_gnu_stack = 0x6474'e551;
constexpr std::uint32_t pt_gnu_relro = 0x6474'e552;
constexpr std::uint32_t r = 4;
constexpr std::uint32_t w = 2;
constexpr std::uint32_t x = 1;

constexpr std::uint32_t sht_progbits = 1;
constexpr std::uint32_t sht_nobits = 8;
constexpr std::uint64_t shf_write = 1;
constexpr std::uint64_t shf_alloc = 2;
constexpr std::uint64_t shf_execinstr = 4;

struct SectionHeader {
    std::string_view name;
    std::uint32_t type;
    std::uint64_t flags;
    std::uint64_t address;
    std::uint64_t offset;
    std::uint64_t size;
};

struct ProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t address;
    std::uint64_t file_size;
    std::uint64_t memory_size;
    std::uint64_t alignment;
};

class ElfBuilder {
public:
    std::vector<std::byte> bytes = std::vector<std::byte>(0x300);

    ElfBuilder() {
        const unsigned char ident[] = {0x7f, 'E', 'L', 'F', 2 /* 64-bit */, 1 /* little-endian */, 1 /* version */};
        std::memcpy(bytes.data(), ident, sizeof ident);
        put<std::uint16_t>(16, 3);       // e_type: ET_DYN
        put<std::uint16_t>(18, 183);     // e_machine: EM_AARCH64
        put<std::uint32_t>(20, 1);       // e_version
        put<std::uint64_t>(24, 0x4200);  // e_entry
        put<std::uint16_t>(52, 64);      // e_ehsize
        put<std::uint16_t>(54, 56);      // e_phentsize
    }

    template <typename T>
    ElfBuilder &put(std::size_t offset, T value) {
        std::memcpy(bytes.data() + offset, &value, sizeof value);
        return *this;
    }

    /// Writes the program header table right after the file header.
    ElfBuilder &segments(std::initializer_list<ProgramHeader> headers) {
        put<std::uint64_t>(32, 64);  // e_phoff
        put<std::uint16_t>(56, static_cast<std::uint16_t>(headers.size()));
        std::size_t at = 64;
        for (const auto &header : headers) {
            put(at, header.type).put(at + 4, header.flags).put(at + 8, header.offset).put(at + 16, header.address);
            put(at + 24, header.address);  // p_paddr
            put(at + 32, header.file_size).put(at + 40, header.memory_size).put(at + 48, header.alignment);
            at += 56;
        }
        return *this;
    }

    ElfBuilder &text(std::size_t offset, std::string_view text) {
        std::memcpy(bytes.data() + offset, text.data(), text.size());
        return *this;
    }

    /// Appends a section name table and the section header table (null section first, the name
    /// table last) to the end of the file, as linkers do.
    ElfBuilder &sections(std::initializer_list<SectionHeader> headers) {
        std::string names(1, '\0');
        std::vector<std::uint32_t> name_offsets;
        for (const auto &header : headers) {
            name_offsets.push_back(static_cast<std::uint32_t>(names.size()));
            names.append(header.name).push_back('\0');
        }
        const auto names_name = static_cast<std::uint32_t>(names.size());
        names.append(".shstrtab").push_back('\0');

        const std::size_t names_offset = bytes.size();
        const std::size_t table_offset = (names_offset + names.size() + 7) & ~std::size_t{7};
        const std::size_t count = headers.size() + 2;
        bytes.resize(table_offset + count * 64);
        text(names_offset, names);

        put<std::uint64_t>(40, table_offset);  // e_shoff
        put<std::uint16_t>(58, 64);            // e_shentsize
        put<std::uint16_t>(60, static_cast<std::uint16_t>(count));
        put<std::uint16_t>(62, static_cast<std::uint16_t>(count - 1));  // e_shstrndx
        std::size_t at = table_offset + 64;  // entry 0 stays all zero
        std::size_t index = 0;
        for (const auto &header : headers) {
            section(at, name_offsets[index++], header.type, header.flags, header.address, header.offset, header.size);
            at += 64;
        }
        section(at, names_name, 3 /* SHT_STRTAB */, 0, 0, names_offset, names.size());
        return *this;
    }

    ElfBuilder &section(std::size_t at, std::uint32_t name, std::uint32_t type, std::uint64_t flags,
                        std::uint64_t address, std::uint64_t offset, std::uint64_t size) {
        put(at, name).put(at + 4, type).put(at + 8, flags).put(at + 16, address).put(at + 24, offset);
        put(at + 32, size).put<std::uint64_t>(at + 48, 1);  // sh_addralign
        return *this;
    }

    /// Writes dynamic entries (tag, value) at `offset`.
    ElfBuilder &dynamic(std::size_t offset, std::initializer_list<std::pair<std::int64_t, std::uint64_t>> entries) {
        for (const auto &[tag, value] : entries) {
            put(offset, tag).put(offset + 8, value);
            offset += 16;
        }
        return *this;
    }
};

}  // namespace apk::testing
