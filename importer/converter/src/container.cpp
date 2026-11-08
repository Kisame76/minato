#include "apkconv/container.h"

#include "APKRuntime/container_format.h"
#include "apkconv/a64.h"
#include "apkconv/elf.h"
#include "apkconv/rewrite.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
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
            refuse(Kind::outside, address);
            return;
        }
        if ((segment->flags & elf::segment_writable) == 0) {
            refuse(Kind::text, address);
            return;
        }
        ++count;
    }

    /// Throws for the first relocation that cannot be applied, naming how many more there are: the walk went on to
    /// count them, so that one refusal says how much a library still needs.
    void finish() const {
        if (refused_ == 0) return;
        const auto reason = first_kind_ == Kind::outside
            ? std::format("a relocation at {:#x} lies outside the image", first_address_)
            : std::format("text relocation at {:#x}: signed code cannot be changed at run time", first_address_);
        throw Incompatible(refused_ > 1 ? std::format("{} (and {} more)", reason, refused_ - 1) : reason);
    }

    std::uint64_t count = 0;

private:
    enum class Kind { outside, text };

    /// Only the first is worded, at the end: a table of millions of refused relocations costs a count, not a message
    /// each.
    void refuse(Kind kind, std::uint64_t address) {
        if (refused_++ == 0) {
            first_kind_ = kind;
            first_address_ = address;
        }
    }

    const elf::File &file_;
    Kind first_kind_ = Kind::text;
    std::uint64_t first_address_ = 0;
    std::uint64_t refused_ = 0;
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
    // The first bad relocation is named before a later table is looked at, as when each was thrown at once.
    check.finish();
    if (const auto rela = dynamic_value(dynamic, dt_rela)) {
        if (dynamic_value(dynamic, dt_relaent).value_or(rela_entry_size) != rela_entry_size) {
            throw Incompatible("RELA entries are not 24 bytes long");
        }
        check_rela(file_bytes_at(file, bytes, *rela, dynamic_value(dynamic, dt_relasz).value_or(0), "RELA table"),
                   check);
    }
    check.finish();
    if (const auto jmprel = dynamic_value(dynamic, dt_jmprel)) {
        check_rela(file_bytes_at(file, bytes, *jmprel, dynamic_value(dynamic, dt_pltrelsz).value_or(0), "PLT relocations"),
                   check);
    }
    check.finish();
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

/// Where the tables of jump tables may lie (ADR-0020 X2d): in a section that is loaded but neither
/// written nor run, backed by the file part of a LOAD segment that is not writable either. Code
/// could change a table anywhere else, or the rewriter could, taking its words for instructions.
rewrite::ReadOnlyData read_only_data(const elf::File &file, std::span<const std::byte> elf_bytes,
                                     const std::vector<elf::Section> &sections) {
    // Whether [start, start + size) lies within `limit` bytes, without overflowing (as elf.cpp's fits).
    const auto within = [](std::uint64_t start, std::uint64_t size, std::uint64_t limit) {
        return start <= limit && size <= limit - start;
    };
    return [&file, elf_bytes, &sections, within](std::uint64_t address, std::size_t size) -> std::span<const std::byte> {
        constexpr auto kinds = elf::section_allocated | elf::section_writable | elf::section_executable;
        const bool in_section = std::ranges::any_of(sections, [&](const elf::Section &section) {
            return (section.flags & kinds) == elf::section_allocated && section.type != elf::section_type_nobits &&
                   address >= section.address && within(address - section.address, size, section.size);
        });
        const auto *segment = elf::load_containing(file, address, size);  // bounds address in the segment
        if (!in_section || segment == nullptr || (segment->flags & elf::segment_writable) != 0 ||
            !within(address - segment->address, size, segment->file_size)) {
            return {};
        }
        const auto offset = segment->offset + (address - segment->address);
        if (!within(offset, size, elf_bytes.size())) return {};
        return elf_bytes.subspan(static_cast<std::size_t>(offset), size);
    };
}

CodeScan scan_sections(const elf::File &file, std::span<const std::byte> elf_bytes,
                       const std::vector<elf::Section> &sections) {
    CodeScan scan;
    const auto data = read_only_data(file, elf_bytes, sections);
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
        // Its bytes come from the segment's file part, where the rewriter's sites must lie too.
        if (section.address - segment->address + section.size > segment->file_size) {
            throw Incompatible(std::format("the executable section {} reaches beyond the file part of its segment at "
                                           "{:#x}",
                                           section.name, segment->address));
        }
        rewrite::scan(elf_bytes.subspan(static_cast<std::size_t>(section.offset), static_cast<std::size_t>(section.size)),
                      section.address, scan.findings, data, true);  // ADR-0020 X7 (c2)
        scan.code_bytes += section.size;
    }
    return scan;
}

/// A deterministic LC_UUID: SHA-256 of the contents and the install name, as an RFC 4122 name-based UUID.
std::array<std::uint8_t, 16> content_uuid(std::span<const std::byte> image, const std::string &install_name) {
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    // CC_LONG has 32 bits: in parts, so that nothing of a container of 4 GiB or more is left out.
    for (std::size_t at = 0; at < image.size(); at += std::size_t{1} << 30) {
        const auto part = std::min(image.size() - at, std::size_t{1} << 30);
        CC_SHA256_Update(&context, image.data() + at, static_cast<CC_LONG>(part));
    }
    CC_SHA256_Update(&context, install_name.data(), static_cast<CC_LONG>(install_name.size()));
    std::uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    std::array<std::uint8_t, 16> uuid;
    std::memcpy(uuid.data(), digest, uuid.size());
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0f) | 0x50);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3f) | 0x80);
    return uuid;
}

/// The LOAD segments and GNU_RELRO, as a layout refusal names them, so that an app's import shows the layout it
/// met: "(segments: LOAD r-x 0x0 size 0x220 align 0x1000, ..., GNU_RELRO 0x12c0 size 0x40)".
std::string segments_text(const elf::File &file) {
    std::string text = "(segments:";
    const char *separator = " ";
    for (const auto &segment : file.segments) {
        const auto flags = segment.flags;
        if (segment.type == elf::SegmentType::load) {
            text += std::format("{}LOAD {}{}{} {:#x} size {:#x} align {:#x}", separator,
                                (flags & elf::segment_readable) ? 'r' : '-',
                                (flags & elf::segment_writable) ? 'w' : '-',
                                (flags & elf::segment_executable) ? 'x' : '-', segment.address, segment.memory_size,
                                segment.alignment);
        } else if (segment.type == elf::SegmentType::gnu_relro) {
            text += std::format("{}GNU_RELRO {:#x} size {:#x}", separator, segment.address, segment.memory_size);
        } else {
            continue;
        }
        separator = ", ";
    }
    return text + ")";
}

/// The allocated sections whose >4 KiB alignment a whole-image move by `shift` would not keep, named for the layout
/// note so an app's import shows which sections the move put off their 16 KiB boundary: ".text (addr 0x200, size
/// 0x20, align 0x4000)". Empty when the shift keeps every section's alignment (a zero shift included).
std::string section_blockers(std::span<const elf::Section> sections, std::uint64_t shift) {
    std::string text;
    const char *separator = "";
    for (const auto &section : sections) {
        if ((section.flags & elf::section_allocated) == 0 || section.addralign == 0 || shift % section.addralign == 0) {
            continue;
        }
        text += std::format("{}{} (addr {:#x}, size {:#x}, align {:#x})", separator,
                            section.name.empty() ? std::string("<unnamed>") : section.name, section.address,
                            section.size, section.addralign);
        separator = ", ";
    }
    return text;
}

/// Names the code a refusal points into, for the import report: " in <symbol>+0x<off> (<section>)", or "" when no
/// symbol covers the address (a stripped library -- the refusal gives the address alone, as before). The symbol is
/// the one with the greatest address at or before `address` in the same section (a sized symbol must also contain
/// it), from read_symbols; "" keeps the symbol-less refusals unchanged, while a protected library's import shows
/// which component the site is in.
std::string describe_site(std::span<const elf::Symbol> symbols, std::span<const elf::Section> sections,
                          std::uint64_t address) {
    const elf::Section *section = nullptr;
    for (const auto &candidate : sections) {
        if ((candidate.flags & elf::section_allocated) == 0 || candidate.size == 0) continue;
        if (address >= candidate.address && address < candidate.address + candidate.size) {
            section = &candidate;
            break;
        }
    }
    const elf::Symbol *best = nullptr;
    for (const auto &symbol : symbols) {
        if (symbol.value > address || (symbol.size != 0 && address >= symbol.value + symbol.size)) continue;
        if (section != nullptr &&
            (symbol.value < section->address || symbol.value >= section->address + section->size)) {
            continue;
        }
        if (best == nullptr || symbol.value > best->value) best = &symbol;
    }
    if (best == nullptr) return {};  // a stripped library names no symbol: the address alone, as before
    // Cap a mangled or hostile symbol name so it cannot grow the reason and the import report without bound.
    constexpr std::size_t name_cap = 160;
    const auto name = best->name.size() > name_cap ? best->name.substr(0, name_cap - 3) + "..." : best->name;
    const auto in_section = section != nullptr && !section->name.empty() ? std::format(" ({})", section->name)
                                                                         : std::string();
    return std::format(" in {}+{:#x}{}", name, address - best->value, in_section);
}

}  // namespace

std::vector<elf::Section> layout_sections(std::span<const std::byte> elf_bytes) {
    try {
        return elf::read_sections(elf_bytes);
    } catch (const elf::FormatError &) {
        return {};
    }
}

PageLayout page_layout(const elf::File &file, std::span<const elf::Section> sections) {
    constexpr std::uint64_t small_page = 4 * 1024;  // Android's pages before 16 KiB
    if (const auto *segment = elf::first_misaligned_load(file, small_page)) {
        throw Incompatible(std::format(
            "LOAD segment at {:#x} does not fit 4 KiB pages: aligned to {:#x}, at offset {:#x} (ADR-0007) {}",
            segment->address, segment->alignment, segment->offset, segments_text(file)));
    }
    std::vector<const elf::Segment *> loads;
    for (const auto &segment : file.segments) {
        if (segment.type == elf::SegmentType::load && segment.memory_size > 0) loads.push_back(&segment);
    }
    if (loads.empty()) throw Incompatible("the file has no LOAD segments");
    std::ranges::sort(loads, {}, &elf::Segment::address);

    // Every end, rounded up to a 16 KiB page, stays in the address space, and the image within 4 GiB, so that no sum
    // below wraps and no container takes more than that (a hostile file's sizes).
    constexpr auto highest = ~std::uint64_t{0} - page_size;
    std::uint64_t image_end = 0;
    for (const auto *segment : loads) {
        if ((segment->flags & (elf::segment_writable | elf::segment_executable)) ==
            (elf::segment_writable | elf::segment_executable)) {
            throw Incompatible(std::format(
                "the LOAD segment at {:#x} is writable and executable, which Android's linker refuses for apps that "
                "target Android 8 (API 26) or later and iOS cannot run {}",
                segment->address, segments_text(file)));
        }
    }
    for (const auto &segment : file.segments) {
        if (segment.type != elf::SegmentType::load && segment.type != elf::SegmentType::gnu_relro) continue;
        if (segment.address > highest || segment.memory_size > highest - segment.address) {
            throw Incompatible(std::format("the segment at {:#x} size {:#x} ends beyond the address space {}",
                                           segment.address, segment.memory_size, segments_text(file)));
        }
        if (segment.type == elf::SegmentType::load) {
            image_end = std::max(image_end, segment.address + segment.memory_size);
        }
    }
    constexpr std::uint64_t largest_image = std::uint64_t{4} << 30;
    if (image_end - page_down(loads.front()->address) > largest_image) {
        throw Incompatible(std::format("the image spans more than 4 GiB, from {:#x} to {:#x} {}",
                                       loads.front()->address, image_end, segments_text(file)));
    }

    // The runs on pages of `granule`: each LOAD segment's pages join the run before when they have its protection;
    // a gap between segments goes to the run before, up to the page of the segment after it. `crossing` is the first
    // segment whose pages reach into the run before of the other protection.
    struct Attempt {
        std::vector<Run> runs;
        const elf::Segment *crossing = nullptr;
    };
    const auto runs_on = [&](std::uint64_t granule) {
        Attempt attempt;
        for (const auto *segment : loads) {
            const bool writable = (segment->flags & elf::segment_writable) != 0;
            const auto start = segment->address & ~(granule - 1);
            const auto end = align_up(segment->address + segment->memory_size, granule);
            if (attempt.runs.empty()) {
                attempt.runs.push_back({.start = start, .end = end, .writable = writable});
                continue;
            }
            auto &before = attempt.runs.back();
            if (before.writable == writable) {
                before.end = std::max(before.end, end);
                continue;
            }
            if (start < before.end) {
                attempt.crossing = segment;
                break;
            }
            before.end = start;
            attempt.runs.push_back({.start = start, .end = end, .writable = writable});
        }
        return attempt;
    };

    PageLayout layout;
    layout.min_vaddr = page_down(loads.front()->address);
    // The alignment a whole-image shift must keep. A shift by a 4 KiB multiple keeps the loader's run-based 16 KiB
    // page boundaries and every relative offset in the image, and bionic on a 4 KiB device maps with a 4 KiB load
    // bias, so an allocated section's sh_addralign above 4 KiB is honoured in place by no Android phone either: it
    // does not block the shift (ADR-0007). The sections such a shift moves off their alignment go to
    // `layout.moved_sections` below as a note, not a refusal. The TLS block stays a hard need: our per-thread TLS
    // must honour its p_align, which a shift off a 16 KiB boundary would break. Without section headers we cannot
    // tell a real need from a post-processed p_align, so the largest segment p_align stands. (`small_page` floors it,
    // as the shift is always a multiple of 4 KiB.)
    std::uint64_t alignment = small_page;
    if (sections.empty()) {
        for (const auto *segment : loads) alignment = std::max(alignment, segment->alignment);
    } else {
        for (const auto &segment : file.segments) {
            if (segment.type == elf::SegmentType::tls) alignment = std::max(alignment, segment.alignment);
        }
    }
    if (auto pages = runs_on(page_size); pages.crossing == nullptr) {
        // As it is: built for 16 KiB pages, or for 4 KiB pages with code and data apart.
        layout.runs = std::move(pages.runs);
    } else {
        auto small_pages = runs_on(small_page);
        if (small_pages.crossing != nullptr) {
            throw Incompatible(std::format(
                "the segment at {:#x} and the one before it share a 4 KiB page, one writable and one not "
                "(ADR-0007) {}",
                small_pages.crossing->address, segments_text(file)));
        }
        layout.runs = std::move(small_pages.runs);
        if (layout.runs.front().start != layout.min_vaddr) {
            // Below a first writable run: the 16 KiB page's rest goes to the code before it, with the header.
            if (layout.runs.front().writable) {
                layout.runs.insert(layout.runs.begin(),
                                   Run{.start = layout.min_vaddr, .end = layout.runs.front().start, .writable = false});
            } else {
                layout.runs.front().start = layout.min_vaddr;
            }
        }
        // One shift for the whole image must put every boundary between runs on a 16 KiB page, and the start of a
        // first run that is writable, which ends the header's __TEXT.
        std::optional<std::uint64_t> offset;
        std::uint64_t first_boundary = 0;
        for (std::size_t index = layout.runs.front().writable ? 0 : 1; index < layout.runs.size(); ++index) {
            const auto into_page = (layout.runs[index].start - layout.min_vaddr) % page_size;
            if (offset && *offset != into_page) {
                throw Incompatible(std::format(
                    "no shift puts every boundary between code and data on a 16 KiB page: {:#x} and {:#x} lie "
                    "{:#x} and {:#x} into theirs (ADR-0007, ADR-0008) {}",
                    first_boundary, layout.runs[index].start, *offset, into_page, segments_text(file)));
            }
            if (!offset) first_boundary = layout.runs[index].start;
            offset = into_page;
        }
        layout.shift = offset ? (page_size - *offset) % page_size : 0;
        if (layout.shift % alignment != 0) {
            if (sections.empty()) {
                throw Incompatible(std::format(
                    "a LOAD segment aligned to {:#x} cannot move by {:#x}, which its 4 KiB layout needs, and the "
                    "section headers that would show whether that alignment is real are missing (ADR-0007) {}",
                    alignment, layout.shift, segments_text(file)));
            }
            throw Incompatible(std::format(
                "a move by {:#x} for its 4 KiB layout would break the alignment of the TLS block (align {:#x}); our "
                "per-thread TLS must honour it (ADR-0007) {}",
                layout.shift, alignment, segments_text(file)));
        }
    }
    // The allocated sections the shift moved off a >4 KiB alignment: a note (above), not a refusal.
    layout.moved_sections = section_blockers(sections, layout.shift);
    layout.runs.front().start = layout.min_vaddr;
    // The image ends on a 16 KiB page of the container.
    auto &last = layout.runs.back();
    last.end += (page_size - (layout.shift + last.end - layout.min_vaddr) % page_size) % page_size;
    layout.end = last.end;
    const auto first_writable = std::ranges::find(layout.runs, true, &Run::writable);
    layout.writable_start = first_writable == layout.runs.end() ? layout.end : first_writable->start;

    // GNU_RELRO, which the guest may make read-only after its relocations, lies in one run of writable pages.
    for (const auto &segment : file.segments) {
        if (segment.type != elf::SegmentType::gnu_relro || segment.memory_size == 0) continue;
        const bool inside = std::ranges::any_of(layout.runs, [&](const Run &run) {
            return run.writable && segment.address >= run.start && segment.address + segment.memory_size <= run.end;
        });
        if (!inside) {
            throw Incompatible(std::format(
                "GNU_RELRO at {:#x} size {:#x} is not inside one run of writable pages (ADR-0008) {}",
                segment.address, segment.memory_size, segments_text(file)));
        }
    }
    return layout;
}

std::string x18_refusal(std::span<const rewrite::Unsupported> uses, std::span<const elf::Symbol> symbols,
                        std::span<const elf::Section> sections, const X18SiteCounts &counts) {
    // The (b)/(c) split, after the reason(s): the only way the counts reach an import on the iPhone, where the
    // library never runs through apk-convert (no user libraries on the Mac).
    const auto facts = std::format(
        "; of the {}: {} defined in their loop, {} live into it, {} in no window; {} in a function with a free "
        "register",
        uses.size(), counts.defined_in_loop, counts.live_in, counts.no_exclusive, counts.with_free_register);
    if (uses.size() == 1) {
        return std::format("x18 at {:#x}{} cannot run on iOS: {}{}", uses[0].address,
                           describe_site(symbols, sections, uses[0].address), uses[0].reason, facts);
    }
    // One class per reason, the most common first, then the one met first; the first three addresses the scan met,
    // each with the symbol it sits in.
    std::vector<std::pair<std::string_view, std::vector<std::uint64_t>>> classes;
    for (const auto &use : uses) {
        auto known = std::ranges::find(classes, std::string_view(use.reason), &decltype(classes)::value_type::first);
        if (known == classes.end()) known = classes.insert(classes.end(), {use.reason, {}});
        known->second.push_back(use.address);
    }
    std::ranges::stable_sort(classes, std::ranges::greater{}, [](const auto &c) { return c.second.size(); });
    constexpr std::size_t shown = 3;
    std::string text = std::format("x18 cannot run on iOS in {} places", uses.size());
    for (std::size_t i = 0; i < classes.size(); ++i) {
        const auto &[reason, addresses] = classes[i];
        text += std::format("{} {} where {} (", i == 0 ? ":" : ";", addresses.size(), reason);
        for (std::size_t k = 0; k < addresses.size() && k < shown; ++k) {
            const bool last = k + 1 == addresses.size();
            text += std::format("{}{:#x}{}", k == 0 ? "" : last ? " and " : ", ", addresses[k],
                                describe_site(symbols, sections, addresses[k]));
        }
        if (addresses.size() > shown) text += std::format(" and {} more", addresses.size() - shown);
        text += ')';
    }
    return text + facts;
}

namespace {

/// RET xN, any Rn (0xd65f0000 | Rn << 5): the function boundary when there is no symbol.
constexpr bool is_return(std::uint32_t word) { return (word & 0xffff'fc1f) == 0xd65f'0000; }

/// An instruction that cannot fall through to the next: an unconditional B, an indirect BR, RET or ERET. BL/BLR and
/// the conditional branches (B.cond, CBZ/CBNZ, TBZ/TBNZ) fall through -- they return, or may not be taken -- so they
/// are not among these. Used to decide whether the word before a function can fall into it, or its last word out.
constexpr bool is_hard_transfer(std::uint32_t word) {
    if ((word & 0xfc00'0000) == 0x1400'0000) return true;  // B (unconditional); BL is 0x94000000, excluded
    if ((word & 0xffff'fc1f) == 0xd61f'0000) return true;  // BR Xn; BLR is 0xd63f0000, excluded
    if (is_return(word)) return true;                      // RET Xn
    return word == 0xd69f'03e0 || word == 0xd69f'0bff || word == 0xd69f'0fff;  // ERET, ERETAA, ERETAB
}

/// The section that holds `address`, or nullptr.
const elf::Section *section_of(std::span<const elf::Section> sections, std::uint64_t address) {
    for (const auto &section : sections) {
        if (section.size != 0 && address >= section.address && address - section.address < section.size) {
            return &section;
        }
    }
    return nullptr;
}

/// The [begin, end) of the function that holds the use at `address` (virtual addresses), inside the section `code`
/// that starts at `base`: the sized symbol that contains it (sized_function_bounds), else the run from the
/// instruction after the previous RET to the next RET (inclusive). This guessed fallback is good enough to count
/// whether a function has a free register, but not to rename across (must-have #2): option (c) uses
/// sized_function_bounds directly, which refuses rather than guess.
std::pair<std::uint64_t, std::uint64_t> enclosing_function(std::span<const elf::Symbol> symbols,
                                                           std::span<const std::byte> code, std::uint64_t base,
                                                           std::uint64_t address) {
    const std::uint64_t limit = base + code.size();
    if (const auto bounds = sized_function_bounds(symbols, base, limit, address)) return *bounds;

    const auto word = [&](std::uint64_t at) {
        std::uint32_t value;
        std::memcpy(&value, code.data() + (at - base), sizeof value);
        return value;
    };
    std::uint64_t begin = base;
    for (std::uint64_t at = address; at > base; at -= 4) {  // the instruction after the previous RET
        if (is_return(word(at - 4))) {
            begin = at;
            break;
        }
    }
    std::uint64_t end = limit;
    for (std::uint64_t at = address; at + 4 <= limit; at += 4) {  // up to and including the next RET
        if (is_return(word(at))) {
            end = at + 4;
            break;
        }
    }
    return {begin, end};
}

}  // namespace

std::optional<std::pair<std::uint64_t, std::uint64_t>> sized_function_bounds(std::span<const elf::Symbol> symbols,
                                                                             std::uint64_t base, std::uint64_t limit,
                                                                             std::uint64_t address) {
    const elf::Symbol *best = nullptr;
    for (const auto &symbol : symbols) {
        if (symbol.size != 0 && address >= symbol.value && address - symbol.value < symbol.size &&
            (best == nullptr || symbol.value > best->value)) {
            best = &symbol;  // the tightest by start among the sized symbols that contain the address
        }
    }
    if (best == nullptr) return std::nullopt;
    return std::pair{std::max(base, best->value), std::min(limit, best->value + best->size)};
}

namespace {

/// Whether a direct BL in [begin, end) of the section `code` (its first byte at virtual address `base`) calls a
/// sized function in the same file that reads x30 as a value (ADR-0020 option c, lead review rule 3): that callee
/// runs with x30 pointing into our call island, so reading it as data is wrong. A BL whose target is not a sized
/// function of the file (external, or an unsized region) cannot be checked -- a named limit, not refused.
bool a_called_sized_function_reads_x30(const elf::File &file, std::span<const std::byte> elf_bytes,
                                       std::span<const elf::Symbol> symbols, std::span<const elf::Section> sections,
                                       std::span<const std::byte> code, std::uint64_t base, std::size_t begin,
                                       std::size_t end) {
    for (std::size_t offset = begin; offset + 4 <= end; offset += 4) {
        std::uint32_t word;
        std::memcpy(&word, code.data() + offset, sizeof word);
        if ((word & 0xfc00'0000) != 0x9400'0000) continue;  // only a direct BL
        std::int64_t imm = static_cast<std::int64_t>(word & 0x03ff'ffff);
        if ((imm & 0x0200'0000) != 0) imm -= 0x0400'0000;  // sign-extend the 26-bit displacement
        const std::uint64_t target = base + offset + static_cast<std::uint64_t>(imm * 4);
        const auto *callee = section_of(sections, target);
        if (callee == nullptr) continue;
        const auto bounds = sized_function_bounds(symbols, callee->address, callee->address + callee->size, target);
        if (!bounds) continue;  // not a sized function of the file: a named limit, not refused
        std::span<const std::byte> callee_code;
        try {
            callee_code = file_bytes_at(file, elf_bytes, callee->address, callee->size, "section");
        } catch (const Incompatible &) {
            continue;
        }
        const auto [callee_begin, callee_end] = *bounds;
        if (rewrite::reads_link_register_as_value(
                callee_code.subspan(static_cast<std::size_t>(callee_begin - callee->address),
                                    static_cast<std::size_t>(callee_end - callee_begin)))) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<WholeFunctionRename> c_rename_register(std::span<const std::byte> elf_bytes, std::uint64_t address) {
    const auto file = elf::read(elf_bytes);
    const auto symbols = elf::read_symbols(elf_bytes);
    const auto sections = layout_sections(elf_bytes);
    const auto *section = section_of(sections, address);
    if (section == nullptr) return std::nullopt;
    std::span<const std::byte> code;
    try {
        code = file_bytes_at(file, elf_bytes, section->address, section->size, "section");
    } catch (const Incompatible &) {
        return std::nullopt;  // a section not in the file part of a LOAD segment: nothing to read
    }
    const std::uint64_t base = section->address;
    const std::uint64_t limit = base + code.size();
    const auto bounds = sized_function_bounds(symbols, base, limit, address);
    if (!bounds) return std::nullopt;  // must-have 2: no sized symbol bounds the function; never guess for a rename
    const auto [begin, end] = *bounds;
    const auto begin_off = static_cast<std::size_t>(begin - base);
    const auto end_off = static_cast<std::size_t>(end - base);

    // Hole 1 (lead review): sized bounds are trustworthy only if they are the WHOLE function. Refuse a nested or
    // overlapping sized symbol -- renaming part of a real function would leave the rest on x18. Then refuse a word
    // that falls through into begin (begin would be mid-function, running renamed code with the register unset) or
    // out past end (the renamed register would escape into the next function). The exit sync will later cover the
    // fall-out edge; until the emission adds it, (c) refuses that case too.
    for (const auto &symbol : symbols) {
        if (symbol.size == 0) continue;
        const std::uint64_t other_begin = std::max(base, symbol.value);
        const std::uint64_t other_end = std::min(limit, symbol.value + symbol.size);
        if (other_begin >= other_end) continue;  // not in this section
        if (other_begin < end && begin < other_end && (other_begin != begin || other_end != end)) {
            return std::nullopt;  // overlaps the chosen bounds but is not exactly them
        }
    }
    const auto word_at_off = [&](std::size_t off) {
        std::uint32_t value;
        std::memcpy(&value, code.data() + off, sizeof value);
        return value;
    };
    if (begin != base && !is_hard_transfer(word_at_off(begin_off - 4))) return std::nullopt;  // falls into begin
    if (!is_hard_transfer(word_at_off(end_off - 4))) return std::nullopt;                      // falls past end
    // The entry island runs the first instruction, so it must be a plain one (not control flow, not pc-relative).
    if (!rewrite::c_entry_runs_first_instruction(word_at_off(begin_off))) return std::nullopt;

    const auto function = code.subspan(begin_off, end_off - begin_off);
    if (!rewrite::can_rename_x18(function)) return std::nullopt;  // an x18 use with no register field (CASP, ST64B)
    const auto reg = rewrite::free_caller_saved_register(function);
    if (!reg) return std::nullopt;  // must-have 1: no caller-saved register is free across the function
    if (rewrite::has_external_entry(code, base, begin_off, end_off)) return std::nullopt;  // must-have 3
    // A non-call branch inside the function back to begin would rerun the entry reload with a stale slot 11 (lead
    // review); a BL to begin is fine, as its call island stores first.
    if (rewrite::has_internal_branch_to_begin(code, begin_off, end_off)) return std::nullopt;
    // A call island leaves x30 pointing into the stub, so refuse a function that reads x30 as a value after a call,
    // or a bl .+4 PC-getter (lead review).
    if (rewrite::call_leaks_link_register(code.subspan(begin_off, end_off - begin_off))) return std::nullopt;
    // ...and refuse when a direct BL calls a sized in-file function that reads x30 as data (lead review rule 3).
    if (a_called_sized_function_reads_x30(file, elf_bytes, symbols, sections, code, base, begin_off, end_off)) {
        return std::nullopt;
    }
    return WholeFunctionRename{begin, end, *reg};
}

CRenamePlan c_rename_plan(std::span<const std::byte> elf_bytes, std::span<const rewrite::Unsupported> uses) {
    CRenamePlan plan;
    if (uses.empty()) return plan;
    const auto symbols = elf::read_symbols(elf_bytes);
    const auto sections = layout_sections(elf_bytes);
    std::map<std::uint64_t, std::optional<WholeFunctionRename>> verdict;  // sized function begin -> (c) rename or none
    for (const auto &use : uses) {
        const auto *section = section_of(sections, use.address);
        if (section == nullptr) continue;
        const auto bounds =
            sized_function_bounds(symbols, section->address, section->address + section->size, use.address);
        if (!bounds) continue;  // no sized function holds the site, so (c) cannot take it
        auto it = verdict.find(bounds->first);
        if (it == verdict.end()) {  // one c_rename_register verdict per function: group the sites by it
            it = verdict.emplace(bounds->first, c_rename_register(elf_bytes, use.address)).first;
        }
        if (it->second) plan.covered_sites.push_back(use.address);  // the function is taken, so this site is covered
    }
    for (const auto &[begin, rename] : verdict) {  // std::map is ordered, so the functions come out ascending by begin
        if (rename) plan.functions.push_back(*rename);
    }
    return plan;
}

std::size_t x18_sites_c_would_take(std::span<const std::byte> elf_bytes,
                                   std::span<const rewrite::Unsupported> uses) {
    return c_rename_plan(elf_bytes, uses).covered_sites.size();
}

std::size_t x18_sites_with_a_free_function_register(std::span<const std::byte> elf_bytes,
                                                    std::span<const rewrite::Unsupported> uses) {
    if (uses.empty()) return 0;
    const auto file = elf::read(elf_bytes);
    const auto symbols = elf::read_symbols(elf_bytes);
    const auto sections = layout_sections(elf_bytes);
    std::size_t count = 0;
    for (const auto &use : uses) {
        const auto *section = section_of(sections, use.address);
        if (section == nullptr) continue;  // not in a known section: no function to bound, leave it out
        std::span<const std::byte> code;
        try {
            code = file_bytes_at(file, elf_bytes, section->address, section->size, "section");
        } catch (const Incompatible &) {
            continue;  // a section not in the file part of a LOAD segment: nothing to read
        }
        const auto [begin, end] = enclosing_function(symbols, code, section->address, use.address);
        if (rewrite::free_caller_saved_register(code.subspan(static_cast<std::size_t>(begin - section->address),
                                                             static_cast<std::size_t>(end - begin)))) {
            ++count;
        }
    }
    return count;
}

X18SiteCounts x18_site_counts(std::span<const std::byte> elf_bytes, const rewrite::Findings &findings) {
    X18SiteCounts counts;
    counts.defined_in_loop = findings.x18_defined_in_loop;
    counts.live_in = findings.x18_live_in;
    counts.no_exclusive = findings.x18_unsupported.size() - counts.defined_in_loop - counts.live_in;
    counts.with_free_register = x18_sites_with_a_free_function_register(elf_bytes, findings.x18_unsupported);
    return counts;
}

/// The tests convert() and refusals() both make, so that the two cannot drift: the machine and the file type, a
/// Darwin syscall, and a use of x18 that no island handles. Each throws Incompatible.
void require_aarch64_shared(const elf::File &file) {
    if (file.machine != elf::machine_aarch64) {
        throw Incompatible(std::format("machine {}: only AArch64 (arm64-v8a) code runs", elf::machine_name(file.machine)));
    }
    if (file.type != elf::FileType::shared) {
        throw Incompatible("not position independent (no ET_DYN): nothing can be mapped below 4 GiB on iOS");
    }
}

void refuse_darwin_syscall(const elf::File &file, std::span<const std::byte> elf_bytes,
                           const rewrite::Findings &findings) {
    if (findings.other_syscalls.empty()) return;
    const auto address = findings.other_syscalls.front();
    const auto word = file_bytes_at(file, elf_bytes, address, 4, "instruction");
    std::uint32_t instruction;
    std::memcpy(&instruction, word.data(), sizeof instruction);
    const auto more = findings.other_syscalls.size() - 1;
    const auto site = describe_site(elf::read_symbols(elf_bytes), layout_sections(elf_bytes), address);
    throw Incompatible(std::format("svc #{:#x} at {:#x}{} is no Linux syscall and would run a Darwin one{}",
                                   (instruction >> 5) & 0xffff, address, site,
                                   more == 0 ? std::string() : std::format(" (and {} more)", more)));
}

void refuse_tpidrro_read(const elf::File &file, std::span<const std::byte> elf_bytes,
                         const rewrite::Findings &findings) {
    if (findings.tpidrro_reads.empty()) return;
    const auto address = findings.tpidrro_reads.front();
    const auto word = file_bytes_at(file, elf_bytes, address, 4, "instruction");
    std::uint32_t instruction;
    std::memcpy(&instruction, word.data(), sizeof instruction);
    const auto more = findings.tpidrro_reads.size() - 1;
    const auto site = describe_site(elf::read_symbols(elf_bytes), layout_sections(elf_bytes), address);
    throw Incompatible(std::format(
        "mrs x{}, TPIDRRO_EL0 at {:#x}{} reads the Darwin TSD base (zero on Android), which a guest could name as an "
        "exclusive target to clear an x18 island's monitor{}",
        instruction & 0x1f, address, site, more == 0 ? std::string() : std::format(" (and {} more)", more)));
}

// Option (c) and the islands must agree on every site of a taken function. A (c) function renames x18 across its
// whole body, so a single or no-save x18 site or an exclusive window inside it is NOT emitted (ADR-0020 A1): the
// rename already handles those x18 uses, so such a construct no longer drops the function. A thread-pointer or
// syscall site inside it is kept and handled (ADR-0020 A2): the site loop patches it to a branch into its veneer, and
// because that branch leaves the function it becomes a (c) branch_exit whose sync stores the register to slot 11
// before the veneer runs -- so a signal stopped in the veneer, which reads slot 11, sees the current x18 (the pre-
// count reserves that branch_exit island through c_function_preview below). A signal's READ there is exact; a handler
// that WRITES x18 between that store and the resume at site+4 (the island's pop/b tail and the veneer, not the
// dispatcher) reaches only slot 11, not the register -- the A2 lost-write limit. Three things still drop the function
// and keep it refused: a jump table (whether (c) renames a `br x18` to `br <register>` correctly is not yet verified,
// so we do not suppress its island -- a named limit for a later step); an exclusive window that straddles the
// function's begin or end (A1b): a window wholly inside is renamed in place, but one that only overlaps would leave
// its words outside [begin, end) on physical x18 while the inside words become the register, and no island can span
// both halves; and a thread-pointer read into a real register, or a thread-pointer write, AS the first instruction
// (A3): it becomes a veneer branch at offset 0, which c_sync_points reads as the entry rather than a branch_exit, so
// emit_c_entry would run it with no R->slot-11 store before the veneer -- a named limit (a svc at begin the gate
// already refuses as a branch, and a read into xzr is a harmless no-stub nop, so neither drops the function here).
void keep_c_functions_clear_of_islands(CRenamePlan &plan, const rewrite::Findings &findings) {
    const auto holds_an_island_c_cannot_absorb = [&](const WholeFunctionRename &function) {
        const auto inside = [&](std::uint64_t address) { return address >= function.begin && address < function.end; };
        for (const auto &table : findings.jump_tables) if (inside(table.address)) return true;
        for (const auto &region : findings.exclusive_regions) {
            const std::uint64_t start = region.address;
            const std::uint64_t stop = region.address + 4 * region.words.size();  // one past the store-exclusive
            const bool overlaps = start < function.end && stop > function.begin;
            const bool wholly_inside = start >= function.begin && stop <= function.end;
            if (overlaps && !wholly_inside) return true;  // straddles a boundary (scan cannot build this; defense)
        }
        // A3: a thread-pointer or syscall site AS the first instruction becomes a veneer BRANCH there, but
        // c_sync_points reads offset 0 as the entry, not a branch_exit -- emit_c_entry runs that branch as the entry's
        // "first instruction" with no R->slot-11 store before it, so a signal in the veneer would read slot 11 relying
        // on it still holding the entry x18, and re-basing the entry branch is unverified. Refuse the function (a named
        // limit). The gate already refuses a svc at begin (is_branch), so in practice only a mrs/msr reaches here; a
        // thread-pointer read into xzr is a no-stub nop, not a branch, so it does not drop the function.
        for (const auto &site : findings.sites) {
            if (site.address != function.begin) continue;
            const bool into_xzr = site.kind == rewrite::SiteKind::thread_pointer_read && site.reg == 31;
            if (!into_xzr && (site.kind == rewrite::SiteKind::thread_pointer_read ||
                              site.kind == rewrite::SiteKind::thread_pointer_write ||
                              site.kind == rewrite::SiteKind::syscall))
                return true;
        }
        return false;
    };
    std::vector<WholeFunctionRename> kept;
    for (const auto &function : plan.functions) {
        if (!holds_an_island_c_cannot_absorb(function)) kept.push_back(function);
    }
    std::vector<std::uint64_t> covered;
    for (const auto address : plan.covered_sites) {
        for (const auto &function : kept) {
            if (address >= function.begin && address < function.end) {
                covered.push_back(address);
                break;
            }
        }
    }
    plan.functions = std::move(kept);
    plan.covered_sites = std::move(covered);
}

void refuse_unsupported_x18(std::span<const std::byte> elf_bytes, const rewrite::Findings &findings,
                            std::span<const std::uint64_t> covered) {
    std::vector<rewrite::Unsupported> remaining;
    for (const auto &use : findings.x18_unsupported) {
        if (std::ranges::find(covered, use.address) == covered.end()) remaining.push_back(use);
    }
    if (remaining.empty()) return;
    throw Incompatible(x18_refusal(remaining, elf::read_symbols(elf_bytes), layout_sections(elf_bytes),
                                   x18_site_counts(elf_bytes, findings)));
}

/// A libffi entry the converter redirects (ADR-0072 decision 3 b).
struct EntryHookPlan {
    std::string name;
    std::uint64_t address = 0;  // ELF address of the function's first word
    std::uint32_t first = 0;    // the word
};

/// The three libffi closure functions of a library that defines all of them, when each one's first word can run in the
/// original stub and no other rewrite touches it; else none (all or nothing: the loader's override replaces the three
/// together). They are found by name in the symbol table, as the loader's override finds them by name.
static std::vector<EntryHookPlan> plan_entry_hooks(const elf::File &file, std::span<const std::byte> elf_bytes,
                                            const std::vector<elf::Section> &sections,
                                            const rewrite::Findings &findings, const CRenamePlan &c_plan) {
    constexpr std::string_view names[] = {"ffi_closure_alloc", "ffi_closure_free", "ffi_prep_closure_loc"};
    const auto symbols = elf::read_symbols(elf_bytes);
    std::vector<EntryHookPlan> plan;
    for (const auto name : names) {
        const auto found =
            std::ranges::find_if(symbols, [&](const elf::Symbol &symbol) { return symbol.name == name; });
        if (found == symbols.end() || found->value % 4 != 0) return {};
        const bool executable = std::ranges::any_of(sections, [&](const elf::Section &section) {
            return (section.flags & elf::section_executable) != 0 && found->value >= section.address &&
                   found->value + 4 <= section.address + section.size;
        });
        if (!executable || elf::load_containing(file, found->value, 4) == nullptr) return {};
        std::uint32_t first = 0;
        std::memcpy(&first, file_bytes_at(file, elf_bytes, found->value, 4, "libffi entry").data(), sizeof first);
        if (!rewrite::entry_hook_runs_first_instruction(first)) return {};
        // Another rewrite of the same word, or a renamed function around it, would fight over it.
        if (std::ranges::any_of(findings.sites,
                                [&](const rewrite::Site &site) { return site.address == found->value; })) {
            return {};
        }
        if (std::ranges::any_of(c_plan.functions, [&](const WholeFunctionRename &function) {
                return found->value >= function.begin && found->value < function.end;
            })) {
            return {};
        }
        plan.push_back({std::string(name), found->value, first});
    }
    return plan;
}

Container convert(std::span<const std::byte> elf_bytes, const Options &options) {
    const auto file = elf::read(elf_bytes);
    require_aarch64_shared(file);
    const auto layout = page_layout(file, layout_sections(elf_bytes));

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

    // Layout of the ELF image (page_layout): its runs of pages with one protection, each from a 16 KiB page of the
    // container on. A code run is in the file in whole pages, a data run only up to the page of its last file byte;
    // dyld fills the rest with zeros.
    const std::uint64_t min_vaddr = layout.min_vaddr;
    const std::uint64_t end = layout.end;
    const auto &runs = layout.runs;
    const auto file_end = [&](const Run &run) {
        if (!run.writable) return run.end;
        std::uint64_t last = run.start;
        for (const auto *segment : loads) {
            if (segment->address >= run.start && segment->address < run.end && segment->file_size > 0) {
                last = std::max(last, run.start + align_up(segment->address + segment->file_size - run.start,
                                                           page_size));
            }
        }
        return last;
    };

    // Scan the executable sections: only there are the bytes known to be instructions (R1).
    const auto scan = scan_sections(file, elf_bytes, sections);
    const auto &findings = scan.findings;
    report.code_bytes = scan.code_bytes;
    refuse_darwin_syscall(file, elf_bytes, findings);
    // A guest mrs TPIDRRO_EL0 would read the Darwin TSD base, where the x18 islands keep the guest's x18; it could
    // then name that granule as an exclusive target and clear an island's monitor (ADR-0020 step C, Q2).
    refuse_tpidrro_read(file, elf_bytes, findings);
    // XNU sets x18 to zero at every context switch (F4). Every use of x18 runs in an island with a scratch register
    // instead (ADR-0020); option (c) instead renames x18 to a free caller-saved register across a whole sized
    // function, so a function with a hard x18 use (nested, unclosed-register-branch, no island) and a free register
    // is taken out of the refusal here and renamed below. A form no island handles and no (c) function takes makes
    // the file incompatible.
    auto c_plan = c_rename_plan(elf_bytes, findings.x18_unsupported);
    keep_c_functions_clear_of_islands(c_plan, findings);
    refuse_unsupported_x18(elf_bytes, findings, c_plan.covered_sites);
    report.x18_writes = static_cast<std::uint32_t>(findings.x18_writes.size());
    report.x18_saves = static_cast<std::uint32_t>(findings.x18_saves.size());

    // ADR-0020 A1: a single/no-save x18 site or an exclusive window whose address a (c) function covers is NOT
    // emitted -- the whole-function rename already turns that x18 use into the free register, so an island there would
    // double-handle it (and its stub would read the stale slot 11 on a signal). These predicates name what option (c)
    // suppresses; the pre-count and the emission below both use them, so island_count and the stub bytes match the
    // add_island calls. Thread-pointer, syscall and jump-table sites never reach here:
    // keep_c_functions_clear_of_islands drops any function holding one, so no (c) range covers it.
    const auto in_c_function = [&](std::uint64_t address) {
        for (const auto &function : c_plan.functions) {
            if (address >= function.begin && address < function.end) return true;
        }
        return false;
    };
    const auto x18_site_suppressed = [&](const rewrite::Site &site) {
        return (site.kind == rewrite::SiteKind::x18 || site.kind == rewrite::SiteKind::x18_nosave) &&
               in_c_function(site.address);
    };
    // An exclusive window is renamed in place (no island) only when its WHOLE span lies inside a (c) function; a
    // window that merely starts inside must still be islanded, as its words past the function end keep physical x18.
    // keep_c_functions_clear_of_islands drops any (c) function a window straddles, so a surviving function holds only
    // wholly-inside or wholly-outside windows -- but the pre-count and emission both test the whole span, so the
    // island table matches the add_island calls whatever the plan holds (A1b).
    const auto exclusive_window_in_c_function = [&](const rewrite::ExclusiveRegion &region) {
        const std::uint64_t stop = region.address + 4 * region.words.size();  // one past the store-exclusive
        for (const auto &function : c_plan.functions) {
            if (region.address >= function.begin && stop <= function.end) return true;
        }
        return false;
    };

    // Layout of the container (APKRuntime/container_format.h): __TEXT with the header and the first run when it is
    // code, a segment for every other run, the runtime slots' page after the image (in the last run when that is
    // writable), then __LINKEDIT.
    constexpr auto code_protection = macho::protection_read | macho::protection_execute;
    constexpr auto data_protection = macho::protection_read | macho::protection_write;
    macho::Dylib dylib{
        .install_name = options.install_name,
        .platform = options.platform,
        .version = options.platform_version,
        .uuid = {},
        .segments = {macho::Segment{.name = "__TEXT",
                                    .protection = code_protection,
                                    .sections = {macho::Section{.name = APK_CONTAINER_SECTION,
                                                                .size = sizeof(apk_container_info)}}}},
    };
    unsigned code_segments = 0, data_segments = 0;
    const auto data_name = [&] {
        return data_segments++ == 0 ? std::string("__DATA") : std::format("__DATA_{}", data_segments - 1);
    };
    for (std::size_t index = runs.front().writable ? 0 : 1; index < runs.size(); ++index) {
        dylib.segments.push_back(runs[index].writable
                                     ? macho::Segment{.name = data_name(), .protection = data_protection}
                                     : macho::Segment{.name = std::format("__TEXT_{}", ++code_segments),
                                                      .protection = code_protection});
    }
    if (!runs.back().writable) {
        dylib.segments.push_back(macho::Segment{.name = data_name(), .protection = data_protection});
    }
    dylib.segments.push_back(macho::Segment{.name = "__LINKEDIT", .protection = macho::protection_read});

    const std::uint64_t info_offset = align_up(macho::header_size(dylib) + macho::signature_headroom, 8);
    // The island table follows __apk_info: one entry per x18 island, in the order of the stubs; then the runs.
    // Both x18 and x18_nosave sites become an island (ADR-0020 C7), so both are counted here -- but a single/no-save
    // x18 site or an exclusive window a (c) function covers is renamed, not islanded (A1), so it is excluded. (Jump
    // tables are never inside a (c) function: keep_c_functions_clear_of_islands drops such a function.) This must
    // match the add_island calls in the site loop below, or the table is mis-sized and the runs overwrite it.
    const auto island_count =
        static_cast<std::size_t>(std::ranges::count_if(findings.sites, [&](const rewrite::Site &site) {
            return !x18_site_suppressed(site) &&
                   (site.kind == rewrite::SiteKind::x18 || site.kind == rewrite::SiteKind::x18_nosave);
        })) +
        static_cast<std::size_t>(std::ranges::count_if(
            findings.exclusive_regions, [&](const auto &region) { return !exclusive_window_in_c_function(region); })) +
        findings.jump_tables.size();
    // emit_c_rename below runs on the body AFTER the site loop patches each tp/syscall site inside the function to a
    // branch into its veneer (ADR-0020 A2); c_sync_points then reads that branch as a branch_exit and lays a sync
    // island for it. The pre-count here reads the unpatched body, so build the same preview: replace each in-function
    // veneer site with the branch (or, for a thread-pointer read into xzr, the nop) it becomes, so c_rename_ranges and
    // c_rename_stub_bytes count that branch_exit island. The exact branch target does not change the count.
    const auto c_function_preview = [&](const WholeFunctionRename &function) {
        const auto original = file_bytes_at(file, elf_bytes, function.begin, function.end - function.begin, "function");
        std::vector<std::byte> body(original.begin(), original.end());
        for (const auto &site : findings.sites) {
            const bool veneer = site.kind == rewrite::SiteKind::thread_pointer_read ||
                                site.kind == rewrite::SiteKind::thread_pointer_write ||
                                site.kind == rewrite::SiteKind::syscall;
            if (!veneer || site.address < function.begin || site.address >= function.end) continue;
            const auto offset = static_cast<std::size_t>(site.address - function.begin);
            const bool into_xzr = site.kind == rewrite::SiteKind::thread_pointer_read && site.reg == 31;
            const std::uint32_t word =
                into_xzr ? 0xd503'201fu  // nop, no stub (emit() returns nop for a read into xzr)
                         : (0x1400'0000u | (((static_cast<std::uint32_t>(body.size() - offset)) / 4) & 0x03ff'ffffu));
            std::memcpy(body.data() + offset, &word, sizeof word);  // `b` to one past the end: a branch leaving fn
        }
        return body;
    };
    // Option (c)'s R-range table (apk_c_range) sits between the runs and the stubs; its sync islands follow the x18
    // islands in the stub area. The range count is address-independent (c_rename_ranges counts the body plus one or
    // two per sync point), so a dummy address serves for the pre-count.
    std::uint64_t c_range_count = 0;
    for (const auto &function : c_plan.functions) {
        c_range_count += rewrite::c_rename_ranges(c_function_preview(function), 0, function.reg, 0).size();
    }
    const std::uint64_t islands_offset = align_up(info_offset + sizeof(apk_container_info), 8);
    const std::uint64_t runs_offset = align_up(islands_offset + island_count * sizeof(apk_island), 8);
    const std::uint64_t c_ranges_offset = align_up(runs_offset + runs.size() * sizeof(apk_run), 8);
    // The entry hooks (ADR-0072 decision 3 b) and their stubs: planned from the symbols and the findings, none for a
    // library that does not define the three libffi names or whose entries cannot be redirected.
    const auto entry_hooks = plan_entry_hooks(file, elf_bytes, sections, findings, c_plan);
    const std::uint64_t entry_hooks_offset = align_up(c_ranges_offset + c_range_count * sizeof(apk_c_range), 8);
    const std::uint64_t stubs_offset = align_up(entry_hooks_offset + entry_hooks.size() * sizeof(apk_entry_hook), 16);
    std::uint64_t stubs_size = entry_hooks.size() * rewrite::entry_hook_stub_size();
    for (const auto &site : findings.sites) if (!x18_site_suppressed(site)) stubs_size += rewrite::stub_size(site);
    for (const auto &region : findings.exclusive_regions)
        if (!exclusive_window_in_c_function(region)) stubs_size += rewrite::exclusive_region_stub_size(region);
    for (const auto &table : findings.jump_tables) stubs_size += rewrite::jump_table_stub_size(table);
    for (const auto &function : c_plan.functions) {
        stubs_size += rewrite::c_rename_stub_bytes(c_function_preview(function));
    }
    const std::uint64_t image_offset = page_up(stubs_offset + stubs_size);
    const auto container_address = [&](std::uint64_t address) {
        return image_offset + layout.shift + (address - min_vaddr);
    };

    auto &text = dylib.segments[0];
    text.vm_size = text.file_size = container_address(runs.front().writable ? runs.front().start : runs.front().end);
    text.sections[0].address = info_offset;

    // The segments follow one another in the file; a data run's zero-filled pages take no file bytes.
    std::uint64_t file_position = text.file_size;
    std::size_t next = 1;
    for (std::size_t index = runs.front().writable ? 0 : 1; index < runs.size(); ++index, ++next) {
        const auto &run = runs[index];
        auto &segment = dylib.segments[next];
        segment.address = container_address(run.start);
        segment.file_offset = file_position;
        segment.vm_size = container_address(run.end) - segment.address;
        segment.file_size = container_address(file_end(run)) - segment.address;
        file_position += segment.file_size;
    }
    const std::uint64_t runtime_slots = container_address(end);
    if (runs.back().writable) {
        dylib.segments[next - 1].vm_size += page_size;  // the runtime slots' page
    } else {
        auto &slots = dylib.segments[next++];
        slots.address = runtime_slots;
        slots.file_offset = file_position;
        slots.vm_size = page_size;
    }

    const auto &before_linkedit = dylib.segments[next - 1];
    auto &linkedit = dylib.segments[next];
    linkedit.address = before_linkedit.address + before_linkedit.vm_size;
    linkedit.file_offset = file_position;
    linkedit.file_size = symbol_table_size;
    linkedit.vm_size = page_size;

    // No container takes much more than its file: code runs are in the file in whole pages, zero-filled ones too.
    const std::uint64_t container_size = linkedit.file_offset + linkedit.file_size;
    if (container_size > 2 * std::uint64_t{elf_bytes.size()} + (std::uint64_t{16} << 20)) {
        throw Incompatible(std::format("the container would take {:#x} bytes for a file of {:#x} {}", container_size,
                                       elf_bytes.size(), segments_text(file)));
    }
    std::vector<std::byte> bytes(container_size, std::byte{0});
    // Where `size` bytes at container address `address` lie in the file: within one segment's file part, or the
    // conversion stops before writing.
    const auto file_at = [&](std::uint64_t address, std::uint64_t size) -> std::byte * {
        for (std::size_t index = 0; index < next; ++index) {
            const auto &segment = dylib.segments[index];
            if (address >= segment.address && size <= segment.file_size &&
                address - segment.address <= segment.file_size - size) {
                return bytes.data() + segment.file_offset + (address - segment.address);
            }
        }
        throw Incompatible(std::format("{:#x} bytes at {:#x} of the container lie outside its file part", size,
                                       address));
    };
    for (const auto *segment : loads) {
        if (segment->file_size == 0) continue;
        std::memcpy(file_at(container_address(segment->address), segment->file_size),
                    elf_bytes.data() + segment->offset, segment->file_size);
    }

    // What a signal handler needs to know of each x18 island (ADR-0020 X4). Both offsets fit 32 bits:
    // emit() refuses a site more than 128 MiB from its stub, and the stubs lie before the image.
    std::vector<apk_island> islands;
    islands.reserve(island_count);
    const auto add_island = [&](const rewrite::IslandLayout &layout, std::uint64_t stub_at, std::uint64_t site_at) {
        islands.push_back({.stub = static_cast<std::uint32_t>(stub_at),
                           .site = static_cast<std::uint32_t>(site_at),
                           .words = static_cast<std::uint16_t>(layout.words),
                           .kind = layout.kind,
                           .value = layout.value,
                           .base = layout.base,
                           .commit = layout.commit,
                           .store = layout.store,
                           .resume = layout.resume});
    };

    // Rewrite the sites; each stub goes into the stub area in the order of the sites.
    std::uint64_t stub = stubs_offset;
    for (const auto &site : findings.sites) {
        if (x18_site_suppressed(site)) continue;  // A1: a (c) function renamed this x18 use; no island for it
        const std::uint64_t site_address = container_address(site.address);
        const auto size = rewrite::stub_size(site);
        const auto patch = rewrite::emit(site, site_address, stub, runtime_slots + offsetof(apk_runtime_slots, syscall_entry),
                                         std::span(file_at(stub, size), static_cast<std::size_t>(size)));
        std::memcpy(file_at(site_address, sizeof patch), &patch, sizeof patch);
        switch (site.kind) {
            case rewrite::SiteKind::thread_pointer_read: ++report.thread_pointer_reads; break;
            case rewrite::SiteKind::thread_pointer_write: ++report.thread_pointer_writes; break;
            case rewrite::SiteKind::syscall: ++report.syscalls; break;
            case rewrite::SiteKind::cache_type_read: ++report.cache_type_reads; break;
            case rewrite::SiteKind::x18:
            case rewrite::SiteKind::x18_nosave:
                ++report.x18_islands;
                if (findings.monitor_inside_sites.contains(site.address)) ++report.monitor_inside_islands;
                add_island(rewrite::island_layout(site), stub, site_address);
                break;
        }
        stub += size;
    }
    // Relocate each load-/store-exclusive window with x18 as one island (ADR-0020 X2c).
    for (const auto &region : findings.exclusive_regions) {
        // A1: a (c) function renamed the window's x18 to the register.
        if (exclusive_window_in_c_function(region)) continue;
        const std::uint64_t region_address = container_address(region.address);
        const auto size = rewrite::exclusive_region_stub_size(region);
        const auto patch = rewrite::emit_exclusive_region(region, region_address, stub,
                                                          std::span(file_at(stub, size), static_cast<std::size_t>(size)));
        std::memcpy(file_at(region_address, sizeof patch), &patch, sizeof patch);
        for (const auto &[at, word] : rewrite::exclusive_region_entry_patches(region, region_address, stub)) {
            std::memcpy(file_at(at, sizeof word), &word, sizeof word);  // a branch from outside enters through its pad
        }
        add_island(rewrite::exclusive_region_layout(region), stub, region_address);
        stub += size;
        ++report.x18_islands;
    }
    // Each br x18 that ends a jump table: an island with its landing pads (ADR-0020 X2d). A (c) function never holds
    // one (keep_c_functions_clear_of_islands drops it), so none is suppressed here.
    for (const auto &table : findings.jump_tables) {
        const std::uint64_t branch_address = container_address(table.address);
        const auto size = rewrite::jump_table_stub_size(table);
        const auto patch = rewrite::emit_jump_table(table, branch_address, stub,
                                                    std::span(file_at(stub, size), static_cast<std::size_t>(size)));
        std::memcpy(file_at(branch_address, sizeof patch), &patch, sizeof patch);
        add_island(rewrite::jump_table_layout(table), stub, branch_address);
        stub += size;
        ++report.x18_islands;
    }
    // Option (c): rename x18 to the free register across each taken function in the container image, and lay its sync
    // islands into the stub area after the x18 islands. The R-ranges say where the guest x18 then lives in the
    // register instead of TSD slot 11, for the signal path (ADR-0020 must-have 6).
    std::vector<apk_c_range> c_ranges;
    c_ranges.reserve(c_range_count);
    for (const auto &function : c_plan.functions) {
        const std::uint64_t function_address = container_address(function.begin);
        const auto size = static_cast<std::size_t>(function.end - function.begin);
        std::byte *body = file_at(function_address, size);
        const auto stub_bytes = rewrite::c_rename_stub_bytes(std::span(body, size));
        const auto ranges = rewrite::emit_c_rename(std::span(body, size), function_address, function.reg, stub,
                                                   std::span(file_at(stub, stub_bytes), stub_bytes));
        for (const auto &range : ranges) {
            c_ranges.push_back({.start = static_cast<std::uint32_t>(range.start),
                                .end = static_cast<std::uint32_t>(range.end),
                                .reg = range.reg,
                                .reserved = {}});
        }
        stub += stub_bytes;
    }
    // The entry hooks: the displaced first word and the branch back, the hook stub that branches through the runtime
    // slot of the hook, and the first word replaced by the branch to the hook stub.
    std::vector<apk_entry_hook> hook_table;
    for (const auto &hook : entry_hooks) {
        const std::uint64_t entry_address = container_address(hook.address);
        const auto size = rewrite::entry_hook_stub_size();
        const std::uint64_t slot = runtime_slots + offsetof(apk_runtime_slots, hook) + 8 * hook_table.size();
        const auto patch = rewrite::emit_entry_hook(hook.first, entry_address, stub, slot,
                                                    std::span(file_at(stub, size), size));
        std::memcpy(file_at(entry_address, sizeof patch), &patch, sizeof patch);
        apk_entry_hook record{};
        std::memcpy(record.name, hook.name.data(), std::min(hook.name.size(), sizeof record.name - 1));
        record.entry = static_cast<std::uint32_t>(entry_address);
        record.original = static_cast<std::uint32_t>(stub);
        hook_table.push_back(record);
        stub += size;
        ++report.entry_hooks;
    }
    if (hook_table.size() > APK_MAX_ENTRY_HOOKS) throw Incompatible("internal: too many entry hooks");
    std::memcpy(bytes.data() + entry_hooks_offset, hook_table.data(), hook_table.size() * sizeof(apk_entry_hook));
    // Internal check (ADR-0020 C7): the pre-count above sized the island table and the runs table was laid
    // out right after it, so if the add_island calls do not match island_count the runs overwrite the table
    // silently (the bug a missed kind caused). Fail the conversion rather than write a corrupt container.
    if (islands.size() != island_count) {
        throw Incompatible(std::format("internal: wrote {} islands but reserved space for {}", islands.size(),
                                       island_count));
    }
    std::memcpy(bytes.data() + islands_offset, islands.data(), islands.size() * sizeof(apk_island));
    if (c_ranges.size() != c_range_count) {
        throw Incompatible(std::format("internal: wrote {} (c) ranges but reserved space for {}", c_ranges.size(),
                                       c_range_count));
    }
    // The signal path binary-searches the table by `start` (ADR-0020 must-have 6, apk_c_range), so it must be sorted:
    // emit_c_rename returns each function's body range (in the image) and its sync-island stub ranges (in the stub
    // area) in emission order, which is not ascending by start across functions or between a function's body and
    // its stubs. Sort the whole table before writing it.
    std::sort(c_ranges.begin(), c_ranges.end(),
              [](const apk_c_range &a, const apk_c_range &b) { return a.start < b.start; });
    // The signal path's binary search also needs the ranges non-overlapping (a pc is in at most one): every (c)
    // body and sync-island stub occupies its own disjoint span by construction, so overlap means a converter bug.
    // Fail loudly here rather than let a signal silently read the stale slot 11 for a covered pc (ADR-0020 6).
    for (std::size_t i = 1; i < c_ranges.size(); ++i) {
        if (c_ranges[i].start < c_ranges[i - 1].end) {
            throw Incompatible(std::format("internal: (c) ranges overlap ([{:#x},{:#x}) and [{:#x},{:#x}))",
                                           c_ranges[i - 1].start, c_ranges[i - 1].end, c_ranges[i].start,
                                           c_ranges[i].end));
        }
    }
    std::memcpy(bytes.data() + c_ranges_offset, c_ranges.data(), c_ranges.size() * sizeof(apk_c_range));
    std::vector<apk_run> run_table;
    for (const auto &run : runs) {
        run_table.push_back({.start = run.start, .end = run.end, .flags = run.writable ? APK_RUN_WRITABLE : 0u,
                             .reserved = 0});
    }
    std::memcpy(bytes.data() + runs_offset, run_table.data(), run_table.size() * sizeof(apk_run));
    report.image_offset = image_offset + layout.shift;
    report.image_shift = layout.shift;
    report.moved_sections_note = layout.moved_sections;
    report.stub_bytes = stubs_size;
    report.island_table_bytes = islands.size() * sizeof(apk_island);

    apk_container_info info{};
    std::memcpy(info.magic, APK_CONTAINER_MAGIC, sizeof info.magic);
    info.version = APK_CONTAINER_VERSION;
    info.flags = (report.is_static ? APK_CONTAINER_STATIC : 0u) |
                 (report.monitor_inside_islands != 0 ? APK_CONTAINER_MONITOR_INSIDE : 0u);
    info.image_offset = image_offset + layout.shift;
    info.image_size = end - min_vaddr;
    info.min_vaddr = min_vaddr;
    info.entry = file.entry;
    info.phdr = program_header_address(file);
    info.phnum = static_cast<std::uint32_t>(file.segments.size());
    info.phentsize = 56;
    info.runtime_slots = runtime_slots;
    const auto last_writable = std::find_if(runs.rbegin(), runs.rend(), [](const Run &run) { return run.writable; });
    info.writable_start = layout.writable_start;
    info.writable_end = last_writable == runs.rend() ? end : last_writable->end;
    info.thread_pointer_reads = report.thread_pointer_reads;
    info.thread_pointer_writes = report.thread_pointer_writes;
    info.syscalls = report.syscalls;
    info.island_count = static_cast<std::uint32_t>(islands.size());
    info.islands = islands_offset;
    info.stubs = stubs_offset;
    info.stubs_size = stubs_size;
    info.runs = runs_offset;
    info.run_count = static_cast<std::uint32_t>(runs.size());
    info.c_ranges = c_ranges_offset;
    info.c_range_count = static_cast<std::uint32_t>(c_ranges.size());
    info.entry_hooks = entry_hooks_offset;
    info.entry_hook_count = static_cast<std::uint32_t>(hook_table.size());
    std::memcpy(bytes.data() + info_offset, &info, sizeof info);

    dylib.uuid = content_uuid(bytes, options.install_name);
    macho::write_header(dylib, bytes);
    return Container{.bytes = std::move(bytes), .report = report};
}

std::vector<std::string> refusals(std::span<const std::byte> elf_bytes) {
    std::vector<std::string> reasons;
    // Each step is a test convert() makes, and its reason is convert()'s own words; whatever else convert() would
    // find in the file is the file's too (as c_api.cpp takes it), but out of memory is not.
    const auto attempt = [&](const auto &step) {
        try {
            step();
            return true;
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::exception &failure) {
            // A name from the file may carry any byte; a reason is one line of printable text.
            std::string reason = failure.what();
            for (auto &letter : reason) {
                if (static_cast<unsigned char>(letter) < 0x20 || letter == 0x7f) letter = ' ';
            }
            reasons.emplace_back(std::move(reason));
            return false;
        }
    };
    std::optional<elf::File> file;
    if (!attempt([&] { file = elf::read(elf_bytes); })) return reasons;
    // Code of another machine, or a file that is no shared object, says nothing more worth naming.
    if (!attempt([&] { require_aarch64_shared(*file); })) return reasons;
    // The scan runs even when page_layout refused the file, which convert() never gets to: scan_sections reads only
    // what it checks itself (a 300k-mutation fuzz found nothing), so a step added after it must not assume
    // page_layout's invariants.
    attempt([&] { (void)page_layout(*file, layout_sections(elf_bytes)); });
    std::optional<std::vector<elf::Section>> sections;
    attempt([&] { sections = sections_for_scan(elf_bytes); });
    attempt([&] { (void)check_relocations(*file, elf_bytes, elf::read_dynamic(*file, elf_bytes)); });
    if (!sections) return reasons;
    std::optional<CodeScan> scan;
    if (!attempt([&] { scan = scan_sections(*file, elf_bytes, *sections); })) return reasons;
    attempt([&] { refuse_darwin_syscall(*file, elf_bytes, scan->findings); });
    attempt([&] { refuse_tpidrro_read(*file, elf_bytes, scan->findings); });
    attempt([&] {
        auto plan = c_rename_plan(elf_bytes, scan->findings.x18_unsupported);
        keep_c_functions_clear_of_islands(plan, scan->findings);
        refuse_unsupported_x18(elf_bytes, scan->findings, plan.covered_sites);
    });
    // What convert() refuses only after these, in building the container (its size, a place outside the file, the
    // rewriter's and the writer's limits), shows when the tests above found nothing.
    if (reasons.empty()) attempt([&] { (void)convert(elf_bytes, {.install_name = "@rpath/check.dylib"}); });
    return reasons;
}

CodeScan scan_code(std::span<const std::byte> elf_bytes) {
    const auto file = elf::read(elf_bytes);
    return scan_sections(file, elf_bytes, sections_for_scan(elf_bytes));
}

}  // namespace apk::container
