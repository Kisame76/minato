#include "apkconv/macho.h"

#include <mach-o/loader.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace apk::macho {
namespace {

/// Load commands are padded to 8 bytes, and so is the path in LC_ID_DYLIB.
std::size_t padded(std::size_t size) {
    return (size + 7) & ~std::size_t{7};
}

std::size_t segment_command_size(const Segment &segment) {
    return sizeof(segment_command_64) + segment.sections.size() * sizeof(section_64);
}

std::size_t id_dylib_command_size(const Dylib &dylib) {
    return padded(sizeof(dylib_command) + dylib.install_name.size() + 1);
}

/// dyld 27 refuses a dylib that does not depend on libSystem ("missing LC_LOAD_DYLIB (must link
/// with at least libSystem.dylib)"). The container binds no symbols from it.
constexpr char libsystem_path[] = "/usr/lib/libSystem.B.dylib";

std::size_t load_libsystem_command_size() {
    return padded(sizeof(dylib_command) + sizeof libsystem_path);
}

/// Copies a segment or section name into its fixed-size field. Names shorter than the field keep a
/// terminating zero, a name of exactly 16 characters fills it, as Mach-O allows.
void set_name(char (&field)[16], const std::string &name, const char *what) {
    if (name.size() > sizeof field) {
        throw FormatError(std::format("{} name \"{}\" is longer than 16 characters", what, name));
    }
    std::memset(field, 0, sizeof field);
    std::memcpy(field, name.data(), name.size());
}

/// Writes structures one after another into the image.
class Cursor {
public:
    Cursor(std::span<std::byte> image, std::size_t limit) : image_(image), limit_(limit) {}

    template <typename T>
    std::byte *write(const T &value) {
        if (offset_ + sizeof value > limit_) throw FormatError("the load commands do not fit");
        std::byte *const at = image_.data() + offset_;
        std::memcpy(at, &value, sizeof value);
        offset_ += sizeof value;
        return at;
    }

    void write_bytes(const void *data, std::size_t size, std::size_t padded_size) {
        if (offset_ + padded_size > limit_) throw FormatError("the load commands do not fit");
        std::memcpy(image_.data() + offset_, data, size);
        std::memset(image_.data() + offset_ + size, 0, padded_size - size);
        offset_ += padded_size;
    }

    std::size_t offset() const { return offset_; }

private:
    std::span<std::byte> image_;
    std::size_t limit_;
    std::size_t offset_ = 0;
};

/// Everything dyld has to be able to map, plus what the loader relies on.
void validate(const Dylib &dylib, std::size_t image_size) {
    if (dylib.segments.size() < 2) throw FormatError("a container needs __TEXT and __LINKEDIT");
    if (dylib.segments.back().name != "__LINKEDIT") {
        throw FormatError("the last segment must be __LINKEDIT, not " + dylib.segments.back().name);
    }
    if (dylib.segments.front().file_offset != 0) {
        throw FormatError("the first segment must start at file offset 0, it holds the Mach-O header");
    }

    const Segment *previous = nullptr;
    for (const auto &segment : dylib.segments) {
        const bool is_last = &segment == &dylib.segments.back();
        if (segment.address % page_size != 0 || segment.vm_size % page_size != 0 ||
            segment.file_offset % page_size != 0) {
            throw FormatError(std::format("segment {} is not on a {} KiB page boundary", segment.name,
                                          page_size / 1024));
        }
        if (segment.file_size > segment.vm_size) {
            throw FormatError(std::format("segment {} has more bytes in the file than in memory", segment.name));
        }
        // Everything dyld maps beyond file_size must read as zero, and mmap works in whole pages.
        // Only the last segment may end inside a page, because nothing follows it.
        if (segment.file_size % page_size != 0 && !is_last) {
            throw FormatError(std::format("segment {} ends inside a page but is not the last one", segment.name));
        }
        if (previous != nullptr) {
            if (segment.address < previous->address + previous->vm_size ||
                segment.file_offset < previous->file_offset + previous->file_size) {
                throw FormatError(std::format("segments {} and {} are out of order or overlap", previous->name,
                                              segment.name));
            }
        }
        for (const auto &section : segment.sections) {
            if (section.address < segment.address ||
                section.address + section.size > segment.address + segment.file_size) {
                throw FormatError(
                    std::format("section {} lies outside the file part of {}", section.name, segment.name));
            }
            const auto offset = segment.file_offset + (section.address - segment.address);
            if (offset < header_size(dylib) + signature_headroom) {
                throw FormatError(std::format("section {} starts at {:#x}, inside the room the code signature needs "
                                              "behind the load commands",
                                              section.name, offset));
            }
        }
        previous = &segment;
    }

    const auto &last = dylib.segments.back();
    if (image_size < last.file_offset + last.file_size) {
        throw FormatError(std::format("the image is too small: {} bytes for a container of {}", image_size,
                                      last.file_offset + last.file_size));
    }
    if (image_size < header_size(dylib)) {
        throw FormatError(std::format("the image is too small for the load commands: {} bytes", image_size));
    }
}

}  // namespace

std::size_t header_size(const Dylib &dylib) {
    std::size_t size = sizeof(mach_header_64) + id_dylib_command_size(dylib) + sizeof(build_version_command) +
                       sizeof(symtab_command) + sizeof(uuid_command) + load_libsystem_command_size();
    for (const auto &segment : dylib.segments) size += segment_command_size(segment);
    return size;
}

void write_header(const Dylib &dylib, std::span<std::byte> image) {
    validate(dylib, image.size());

    const std::size_t commands_size = header_size(dylib) - sizeof(mach_header_64);
    Cursor cursor(image, header_size(dylib));

    mach_header_64 header{};
    header.magic = MH_MAGIC_64;
    header.cputype = CPU_TYPE_ARM64;
    header.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    header.filetype = MH_DYLIB;
    header.ncmds = static_cast<std::uint32_t>(dylib.segments.size() + 5);
    header.sizeofcmds = static_cast<std::uint32_t>(commands_size);
    // No undefined symbols, no two-level namespace lookups to do: the container imports nothing.
    header.flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL;
    cursor.write(header);

    for (const auto &segment : dylib.segments) {
        segment_command_64 command{};
        command.cmd = LC_SEGMENT_64;
        command.cmdsize = static_cast<std::uint32_t>(segment_command_size(segment));
        set_name(command.segname, segment.name, "segment");
        command.vmaddr = segment.address;
        command.vmsize = segment.vm_size;
        command.fileoff = segment.file_offset;
        command.filesize = segment.file_size;
        // The loader never changes protections, so maxprot is what the segment starts with.
        command.maxprot = static_cast<vm_prot_t>(segment.protection);
        command.initprot = static_cast<vm_prot_t>(segment.protection);
        command.nsects = static_cast<std::uint32_t>(segment.sections.size());
        command.flags = 0;
        cursor.write(command);

        for (const auto &section : segment.sections) {
            section_64 entry{};
            set_name(entry.sectname, section.name, "section");
            set_name(entry.segname, segment.name, "segment");
            entry.addr = section.address;
            entry.size = section.size;
            entry.offset = static_cast<std::uint32_t>(segment.file_offset + (section.address - segment.address));
            entry.align = 3;  // the container builder keeps metadata 8-byte aligned
            entry.flags = S_REGULAR;
            cursor.write(entry);
        }
    }

    dylib_command id{};
    id.cmd = LC_ID_DYLIB;
    id.cmdsize = static_cast<std::uint32_t>(id_dylib_command_size(dylib));
    id.dylib.name.offset = sizeof(dylib_command);
    id.dylib.timestamp = 1;
    id.dylib.current_version = 1 << 16;
    id.dylib.compatibility_version = 1 << 16;
    cursor.write(id);
    cursor.write_bytes(dylib.install_name.data(), dylib.install_name.size(),
                       id_dylib_command_size(dylib) - sizeof(dylib_command));

    dylib_command libsystem{};
    libsystem.cmd = LC_LOAD_DYLIB;
    libsystem.cmdsize = static_cast<std::uint32_t>(load_libsystem_command_size());
    libsystem.dylib.name.offset = sizeof(dylib_command);
    libsystem.dylib.timestamp = 2;
    libsystem.dylib.current_version = 1 << 16;
    libsystem.dylib.compatibility_version = 1 << 16;
    cursor.write(libsystem);
    cursor.write_bytes(libsystem_path, sizeof libsystem_path, load_libsystem_command_size() - sizeof(dylib_command));

    uuid_command identity{};
    identity.cmd = LC_UUID;
    identity.cmdsize = sizeof identity;
    std::memcpy(identity.uuid, dylib.uuid.data(), dylib.uuid.size());
    cursor.write(identity);

    build_version_command build{};
    build.cmd = LC_BUILD_VERSION;
    build.cmdsize = sizeof build;
    build.platform = static_cast<std::uint32_t>(dylib.platform);
    build.minos = dylib.version;
    build.sdk = dylib.version;
    build.ntools = 0;
    cursor.write(build);

    // Empty, but dyld and codesign expect the tables to point into __LINKEDIT.
    const auto &linkedit = dylib.segments.back();
    symtab_command symbols{};
    symbols.cmd = LC_SYMTAB;
    symbols.cmdsize = sizeof symbols;
    symbols.symoff = static_cast<std::uint32_t>(linkedit.file_offset);
    symbols.nsyms = 0;
    symbols.stroff = static_cast<std::uint32_t>(linkedit.file_offset);
    symbols.strsize = static_cast<std::uint32_t>(std::min<std::uint64_t>(linkedit.file_size, 16));
    cursor.write(symbols);

    if (cursor.offset() != header_size(dylib)) {
        throw FormatError("the load commands did not fill the space header_size() reported");
    }
}

}  // namespace apk::macho
