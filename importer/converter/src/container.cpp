#include "apkconv/container.h"

#include "APKRuntime/container_format.h"
#include "apkconv/elf.h"
#include "apkconv/rewrite.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>

namespace apk::container {
namespace {

using macho::page_size;

// Dynamic tags the converter looks at (ELF gABI, AArch64 ELF ABI, Android's bionic/linker).
constexpr std::int64_t dt_needed = 1;
constexpr std::int64_t dt_pltrelsz = 2;
constexpr std::int64_t dt_rela = 7;
constexpr std::int64_t dt_relasz = 8;
constexpr std::int64_t dt_relaent = 9;
constexpr std::int64_t dt_rel = 17;
constexpr std::int64_t dt_textrel = 22;
constexpr std::int64_t dt_jmprel = 23;
constexpr std::int64_t dt_flags = 30;
constexpr std::int64_t dt_relrsz = 35;
constexpr std::int64_t dt_relr = 36;
constexpr std::int64_t dt_android_rel = 0x6000'000f;
constexpr std::int64_t dt_android_rela = 0x6000'0011;
constexpr std::int64_t dt_android_relr = 0x6fff'e000;
constexpr std::int64_t dt_android_relrsz = 0x6fff'e001;
constexpr std::uint64_t df_textrel = 0x4;

constexpr std::uint64_t rela_entry_size = 24;
constexpr std::uint64_t symbol_table_size = 16;  // the empty string table in __LINKEDIT

std::uint64_t page_down(std::uint64_t value) {
    return value & ~(page_size - 1);
}

std::uint64_t page_up(std::uint64_t value) {
    return page_down(value + page_size - 1);
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::optional<std::uint64_t> dynamic_value(const std::vector<elf::DynamicEntry> &entries, std::int64_t tag) {
    const auto entry = std::ranges::find(entries, tag, &elf::DynamicEntry::tag);
    if (entry == entries.end()) return std::nullopt;
    return entry->value;
}

/// The standard tag, or Android's older one with the same meaning.
std::optional<std::uint64_t> dynamic_value(const std::vector<elf::DynamicEntry> &entries, std::int64_t tag,
                                           std::int64_t android_tag) {
    if (auto value = dynamic_value(entries, tag)) return value;
    return dynamic_value(entries, android_tag);
}

/// Bytes of the file at ELF address `address`, which must be backed by the file.
std::span<const std::byte> file_bytes_at(const elf::File &file, std::span<const std::byte> bytes,
                                         std::uint64_t address, std::uint64_t size, const char *what) {
    const auto *segment = elf::load_containing(file, address, size);
    if (segment == nullptr || address - segment->address + size > segment->file_size) {
        throw Incompatible(std::format("the {} at {:#x} is not in the file part of a LOAD segment", what, address));
    }
    return bytes.subspan(static_cast<std::size_t>(segment->offset + (address - segment->address)),
                         static_cast<std::size_t>(size));
}

/// The loader writes relocations into memory at run time. That works only in writable segments:
/// the code is signed and cannot change after the Mac or the iPhone signed it (ADR-0010).
class RelocationCheck {
public:
    RelocationCheck(const elf::File &file) : file_(file) {}

    void operator()(std::uint64_t address) {
        const auto *segment = elf::load_containing(file_, address, 8);
        if (segment == nullptr) {
            throw Incompatible(std::format("a relocation at {:#x} lies outside the image", address));
        }
        if ((segment->flags & elf::segment_writable) == 0) {
            throw Incompatible(std::format(
                "text relocation at {:#x}: signed code cannot be changed at run time", address));
        }
        ++count;
    }

    std::uint64_t count = 0;

private:
    const elf::File &file_;
};

/// Walks a RELR table (the compact list of relative relocations): an even entry is an address, an
/// odd entry a bitmap of the 63 words after the last address.
void check_relr(std::span<const std::byte> table, RelocationCheck &check) {
    std::uint64_t where = 0;
    for (std::size_t at = 0; at + 8 <= table.size(); at += 8) {
        std::uint64_t entry;
        std::memcpy(&entry, table.data() + at, sizeof entry);
        if ((entry & 1) == 0) {
            where = entry;
            check(where);
            where += 8;
        } else {
            for (unsigned bit = 1; bit < 64; ++bit) {
                if ((entry >> bit) & 1) check(where + (bit - 1) * 8);
            }
            where += 63 * 8;
        }
    }
}

void check_rela(std::span<const std::byte> table, RelocationCheck &check) {
    for (std::size_t at = 0; at + rela_entry_size <= table.size(); at += rela_entry_size) {
        std::uint64_t offset;
        std::memcpy(&offset, table.data() + at, sizeof offset);
        check(offset);
    }
}

/// The relocations the loader will apply, all checked against the writable segments.
std::uint64_t check_relocations(const elf::File &file, std::span<const std::byte> bytes,
                                const std::vector<elf::DynamicEntry> &dynamic) {
    for (const auto &entry : dynamic) {
        if (entry.tag == dt_textrel || (entry.tag == dt_flags && (entry.value & df_textrel) != 0)) {
            throw Incompatible("the file has text relocations (DT_TEXTREL): signed code cannot be changed at run time");
        }
        if (entry.tag == dt_rel) throw Incompatible("REL relocations: AArch64 files use RELA");
        if (entry.tag == dt_android_rel || entry.tag == dt_android_rela) {
            throw Incompatible("packed Android relocations (APS2) are not supported yet");
        }
    }

    RelocationCheck check(file);
    if (const auto relr = dynamic_value(dynamic, dt_relr, dt_android_relr)) {
        const auto size = dynamic_value(dynamic, dt_relrsz, dt_android_relrsz).value_or(0);
        check_relr(file_bytes_at(file, bytes, *relr, size, "RELR table"), check);
    }
    if (const auto rela = dynamic_value(dynamic, dt_rela)) {
        if (dynamic_value(dynamic, dt_relaent).value_or(rela_entry_size) != rela_entry_size) {
            throw Incompatible("RELA entries are not 24 bytes long");
        }
        check_rela(file_bytes_at(file, bytes, *rela, dynamic_value(dynamic, dt_relasz).value_or(0), "RELA table"),
                   check);
    }
    if (const auto jmprel = dynamic_value(dynamic, dt_jmprel)) {
        check_rela(file_bytes_at(file, bytes, *jmprel, dynamic_value(dynamic, dt_pltrelsz).value_or(0), "PLT relocations"),
                   check);
    }
    return check.count;
}

/// Where the program headers are in memory: the LOAD segment whose file part holds them.
std::uint64_t program_header_address(const elf::File &file) {
    const std::uint64_t size = file.segments.size() * 56;
    for (const auto &segment : file.segments) {
        if (segment.type != elf::SegmentType::load) continue;
        if (file.program_header_offset >= segment.offset &&
            file.program_header_offset + size <= segment.offset + segment.file_size) {
            return segment.address + (file.program_header_offset - segment.offset);
        }
    }
    return 0;
}

std::vector<elf::Section> sections_for_scan(std::span<const std::byte> elf_bytes) {
    auto sections = elf::read_sections(elf_bytes);
    if (sections.empty()) {
        throw Incompatible("the file has no section headers; the rewriter needs them to tell code from data");
    }
    return sections;
}

CodeScan scan_sections(const elf::File &file, std::span<const std::byte> elf_bytes,
                       const std::vector<elf::Section> &sections) {
    CodeScan scan;
    for (const auto &section : sections) {
        if ((section.flags & (elf::section_allocated | elf::section_executable)) !=
                (elf::section_allocated | elf::section_executable) ||
            section.type == elf::section_type_nobits || section.size == 0) {
            continue;
        }
        const auto *segment = elf::load_containing(file, section.address, section.size);
        if (segment == nullptr || (segment->flags & elf::segment_executable) == 0 ||
            section.offset - segment->offset != section.address - segment->address) {
            throw Incompatible(std::format("the executable section {} is not where its segment says", section.name));
        }
        rewrite::scan(elf_bytes.subspan(static_cast<std::size_t>(section.offset), static_cast<std::size_t>(section.size)),
                      section.address, scan.findings);
        scan.code_bytes += section.size;
    }
    return scan;
}

/// A deterministic LC_UUID: SHA-256 of the contents and the install name, as an RFC 4122 name-based UUID.
std::array<std::uint8_t, 16> content_uuid(std::span<const std::byte> image, const std::string &install_name) {
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    CC_SHA256_Update(&context, image.data(), static_cast<CC_LONG>(image.size()));
    CC_SHA256_Update(&context, install_name.data(), static_cast<CC_LONG>(install_name.size()));
    std::uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    std::array<std::uint8_t, 16> uuid;
    std::memcpy(uuid.data(), digest, uuid.size());
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0f) | 0x50);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3f) | 0x80);
    return uuid;
}

}  // namespace

Container convert(std::span<const std::byte> elf_bytes, const Options &options) {
    const auto file = elf::read(elf_bytes);
    if (file.machine != elf::machine_aarch64) {
        throw Incompatible(std::format("machine {}: only AArch64 (arm64-v8a) code runs", elf::machine_name(file.machine)));
    }
    if (file.type != elf::FileType::shared) {
        throw Incompatible("not position independent (no ET_DYN): nothing can be mapped below 4 GiB on iOS");
    }
    if (const auto *segment = elf::first_misaligned_load(file)) {
        throw Incompatible(std::format("LOAD segment at {:#x} does not fit 16 KiB pages (ADR-0007)", segment->address));
    }

    std::vector<const elf::Segment *> loads;
    for (const auto &segment : file.segments) {
        if (segment.type == elf::SegmentType::load && segment.memory_size > 0) loads.push_back(&segment);
    }
    if (loads.empty()) throw Incompatible("the file has no LOAD segments");
    std::ranges::sort(loads, {}, &elf::Segment::address);

    const auto sections = sections_for_scan(elf_bytes);
    const auto dynamic = elf::read_dynamic(file, elf_bytes);

    Report report;
    report.is_static = file.interpreter.empty() && !dynamic_value(dynamic, dt_needed);
    report.relocations = check_relocations(file, elf_bytes, dynamic);

    // Layout of the ELF image: the non-writable segments first, the writable ones on later pages.
    const std::uint64_t min_vaddr = page_down(loads.front()->address);
    std::uint64_t end = 0;
    std::optional<std::uint64_t> first_writable;
    std::uint64_t writable_file_end = 0;
    for (const auto *segment : loads) {
        end = std::max(end, page_up(segment->address + segment->memory_size));
        if (segment->flags & elf::segment_writable) {
            if (!first_writable) first_writable = page_down(segment->address);
            if (segment->file_size > 0) {
                writable_file_end = std::max(writable_file_end, page_up(segment->address + segment->file_size));
            }
        }
    }
    const std::uint64_t writable_start = first_writable.value_or(end);
    for (const auto *segment : loads) {
        if ((segment->flags & elf::segment_writable) == 0 && page_up(segment->address + segment->memory_size) > writable_start) {
            throw Incompatible(std::format(
                "the non-writable segment at {:#x} comes after or shares a page with a writable one", segment->address));
        }
    }
    writable_file_end = std::max(writable_file_end, writable_start);

    // Scan the executable sections: only there are the bytes known to be instructions (R1).
    const auto scan = scan_sections(file, elf_bytes, sections);
    const auto &findings = scan.findings;
    report.code_bytes = scan.code_bytes;
    if (!findings.other_syscalls.empty()) {
        const auto address = findings.other_syscalls.front();
        const auto word = file_bytes_at(file, elf_bytes, address, 4, "instruction");
        std::uint32_t instruction;
        std::memcpy(&instruction, word.data(), sizeof instruction);
        throw Incompatible(std::format("svc #{:#x} at {:#x} is no Linux syscall and would run a Darwin one",
                                       (instruction >> 5) & 0xffff, address));
    }
    // XNU sets x18 to zero at every context switch (F4). Every use of x18 runs in an island with a
    // scratch register instead (ADR-0020); a form no island handles makes the file incompatible.
    if (!findings.x18_unsupported.empty()) {
        const auto more = findings.x18_unsupported.size() - 1;
        const auto &first = findings.x18_unsupported.front();
        throw Incompatible(std::format(
            "x18 at {:#x} cannot run on iOS: {}{}", first.address, first.reason,
            more == 0 ? "" : std::format(" (and {} more x18 use{} no island handles)", more, more == 1 ? "" : "s")));
    }
    report.x18_writes = static_cast<std::uint32_t>(findings.x18_writes.size());
    report.x18_saves = static_cast<std::uint32_t>(findings.x18_saves.size());

    // Layout of the container (APKRuntime/container_format.h).
    macho::Dylib dylib{
        .install_name = options.install_name,
        .platform = options.platform,
        .version = options.platform_version,
        .uuid = {},
        .segments = {macho::Segment{.name = "__TEXT",
                                    .protection = macho::protection_read | macho::protection_execute,
                                    .sections = {macho::Section{.name = APK_CONTAINER_SECTION,
                                                                .size = sizeof(apk_container_info)}}},
                     macho::Segment{.name = "__DATA", .protection = macho::protection_read | macho::protection_write},
                     macho::Segment{.name = "__LINKEDIT", .protection = macho::protection_read}},
    };
    const std::uint64_t info_offset = align_up(macho::header_size(dylib) + macho::signature_headroom, 8);
    const std::uint64_t stubs_offset = align_up(info_offset + sizeof(apk_container_info), 16);
    std::uint64_t stubs_size = 0;
    for (const auto &site : findings.sites) stubs_size += rewrite::stub_size(site);
    const std::uint64_t image_offset = page_up(stubs_offset + stubs_size);
    const auto container_address = [&](std::uint64_t address) { return image_offset + (address - min_vaddr); };

    auto &text = dylib.segments[0];
    text.vm_size = text.file_size = container_address(writable_start);
    text.sections[0].address = info_offset;

    auto &data = dylib.segments[1];
    data.address = data.file_offset = container_address(writable_start);
    data.file_size = writable_file_end - writable_start;
    data.vm_size = end - writable_start + page_size;  // one more page for the runtime slots
    const std::uint64_t runtime_slots = container_address(end);

    auto &linkedit = dylib.segments[2];
    linkedit.address = data.address + data.vm_size;
    linkedit.file_offset = data.file_offset + data.file_size;
    linkedit.file_size = symbol_table_size;
    linkedit.vm_size = page_size;

    std::vector<std::byte> bytes(linkedit.file_offset + linkedit.file_size, std::byte{0});
    for (const auto *segment : loads) {
        std::memcpy(bytes.data() + container_address(segment->address),
                    elf_bytes.data() + segment->offset, segment->file_size);
    }

    // Rewrite the sites; each stub goes into the stub area in the order of the sites.
    std::uint64_t stub = stubs_offset;
    for (const auto &site : findings.sites) {
        const std::uint64_t site_address = container_address(site.address);
        const auto size = rewrite::stub_size(site);
        const auto patch = rewrite::emit(site, site_address, stub, runtime_slots + offsetof(apk_runtime_slots, syscall_entry),
                                         std::span(bytes).subspan(static_cast<std::size_t>(stub), static_cast<std::size_t>(size)));
        std::memcpy(bytes.data() + site_address, &patch, sizeof patch);
        stub += size;
        switch (site.kind) {
            case rewrite::SiteKind::thread_pointer_read: ++report.thread_pointer_reads; break;
            case rewrite::SiteKind::thread_pointer_write: ++report.thread_pointer_writes; break;
            case rewrite::SiteKind::syscall: ++report.syscalls; break;
            case rewrite::SiteKind::x18: ++report.x18_islands; break;
        }
    }
    report.image_offset = image_offset;
    report.stub_bytes = stubs_size;

    apk_container_info info{};
    std::memcpy(info.magic, APK_CONTAINER_MAGIC, sizeof info.magic);
    info.version = APK_CONTAINER_VERSION;
    info.flags = report.is_static ? APK_CONTAINER_STATIC : 0;
    info.image_offset = image_offset;
    info.image_size = end - min_vaddr;
    info.min_vaddr = min_vaddr;
    info.entry = file.entry;
    info.phdr = program_header_address(file);
    info.phnum = static_cast<std::uint32_t>(file.segments.size());
    info.phentsize = 56;
    info.runtime_slots = runtime_slots;
    info.writable_start = writable_start;
    info.writable_end = end;
    info.thread_pointer_reads = report.thread_pointer_reads;
    info.thread_pointer_writes = report.thread_pointer_writes;
    info.syscalls = report.syscalls;
    std::memcpy(bytes.data() + info_offset, &info, sizeof info);

    dylib.uuid = content_uuid(bytes, options.install_name);
    macho::write_header(dylib, bytes);
    return Container{.bytes = std::move(bytes), .report = report};
}

CodeScan scan_code(std::span<const std::byte> elf_bytes) {
    const auto file = elf::read(elf_bytes);
    return scan_sections(file, elf_bytes, sections_for_scan(elf_bytes));
}

}  // namespace apk::container
