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
                      section.address, scan.findings, data);
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
/// that starts at `base`: the sized symbol that contains it (the tightest), else the run from the instruction after
/// the previous RET to the next RET (inclusive). Option (c) would rename x18 across this whole run.
std::pair<std::uint64_t, std::uint64_t> enclosing_function(std::span<const elf::Symbol> symbols,
                                                           std::span<const std::byte> code, std::uint64_t base,
                                                           std::uint64_t address) {
    const elf::Symbol *best = nullptr;
    for (const auto &symbol : symbols) {
        if (symbol.size != 0 && address >= symbol.value && address - symbol.value < symbol.size &&
            (best == nullptr || symbol.value > best->value)) {
            best = &symbol;
        }
    }
    const std::uint64_t limit = base + code.size();
    if (best != nullptr) return {std::max(base, best->value), std::min(limit, best->value + best->size)};

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

bool has_free_register(std::span<const std::byte> code) {
    std::uint32_t used = 0;
    for (std::size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
        std::uint32_t word;
        std::memcpy(&word, code.data() + offset, sizeof word);
        const auto use = a64::register_use(word);
        used |= use.reads | use.writes;
    }
    for (unsigned candidate = 0; candidate < 29; ++candidate) {
        if (candidate != 18 && (used & (1u << candidate)) == 0) return true;
    }
    return false;
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
        if (has_free_register(code.subspan(static_cast<std::size_t>(begin - section->address),
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

void refuse_unsupported_x18(std::span<const std::byte> elf_bytes, const rewrite::Findings &findings) {
    if (findings.x18_unsupported.empty()) return;
    throw Incompatible(x18_refusal(findings.x18_unsupported, elf::read_symbols(elf_bytes), layout_sections(elf_bytes),
                                   x18_site_counts(elf_bytes, findings)));
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
    // XNU sets x18 to zero at every context switch (F4). Every use of x18 runs in an island with a
    // scratch register instead (ADR-0020); a form no island handles makes the file incompatible.
    refuse_unsupported_x18(elf_bytes, findings);
    report.x18_writes = static_cast<std::uint32_t>(findings.x18_writes.size());
    report.x18_saves = static_cast<std::uint32_t>(findings.x18_saves.size());

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
    // Both x18 and x18_nosave sites become an island (ADR-0020 C7), so both are counted here -- this must match
    // the add_island calls in the site loop below, or the table is mis-sized and the runs overwrite it.
    const auto island_count =
        static_cast<std::size_t>(std::ranges::count_if(findings.sites, [](const rewrite::Site &site) {
            return site.kind == rewrite::SiteKind::x18 || site.kind == rewrite::SiteKind::x18_nosave;
        })) +
        findings.exclusive_regions.size() + findings.jump_tables.size();
    const std::uint64_t islands_offset = align_up(info_offset + sizeof(apk_container_info), 8);
    const std::uint64_t runs_offset = align_up(islands_offset + island_count * sizeof(apk_island), 8);
    const std::uint64_t stubs_offset = align_up(runs_offset + runs.size() * sizeof(apk_run), 16);
    std::uint64_t stubs_size = 0;
    for (const auto &site : findings.sites) stubs_size += rewrite::stub_size(site);
    for (const auto &region : findings.exclusive_regions) stubs_size += rewrite::exclusive_region_stub_size(region);
    for (const auto &table : findings.jump_tables) stubs_size += rewrite::jump_table_stub_size(table);
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
        const std::uint64_t site_address = container_address(site.address);
        const auto size = rewrite::stub_size(site);
        const auto patch = rewrite::emit(site, site_address, stub, runtime_slots + offsetof(apk_runtime_slots, syscall_entry),
                                         std::span(file_at(stub, size), static_cast<std::size_t>(size)));
        std::memcpy(file_at(site_address, sizeof patch), &patch, sizeof patch);
        switch (site.kind) {
            case rewrite::SiteKind::thread_pointer_read: ++report.thread_pointer_reads; break;
            case rewrite::SiteKind::thread_pointer_write: ++report.thread_pointer_writes; break;
            case rewrite::SiteKind::syscall: ++report.syscalls; break;
            case rewrite::SiteKind::x18:
            case rewrite::SiteKind::x18_nosave:
                ++report.x18_islands;
                add_island(rewrite::island_layout(site), stub, site_address);
                break;
        }
        stub += size;
    }
    // Relocate each load-/store-exclusive window with x18 as one island (ADR-0020 X2c).
    for (const auto &region : findings.exclusive_regions) {
        const std::uint64_t region_address = container_address(region.address);
        const auto size = rewrite::exclusive_region_stub_size(region);
        const auto patch = rewrite::emit_exclusive_region(region, region_address, stub,
                                                          std::span(file_at(stub, size), static_cast<std::size_t>(size)));
        std::memcpy(file_at(region_address, sizeof patch), &patch, sizeof patch);
        add_island(rewrite::exclusive_region_layout(region), stub, region_address);
        stub += size;
        ++report.x18_islands;
    }
    // Each br x18 that ends a jump table: an island with its landing pads (ADR-0020 X2d).
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
    // Internal check (ADR-0020 C7): the pre-count above sized the island table and the runs table was laid
    // out right after it, so if the add_island calls do not match island_count the runs overwrite the table
    // silently (the bug a missed kind caused). Fail the conversion rather than write a corrupt container.
    if (islands.size() != island_count) {
        throw Incompatible(std::format("internal: wrote {} islands but reserved space for {}", islands.size(),
                                       island_count));
    }
    std::memcpy(bytes.data() + islands_offset, islands.data(), islands.size() * sizeof(apk_island));
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
    info.flags = report.is_static ? APK_CONTAINER_STATIC : 0;
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
    attempt([&] { refuse_unsupported_x18(elf_bytes, scan->findings); });
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
