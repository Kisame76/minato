#include "apkconv/data_hints.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace apk::hints {
namespace {

using Bytes = std::span<const std::byte>;

/// Reads little-endian values from a byte range without ever leaving it; `ok` goes false at the first read past
/// the end.
class Reader {
public:
    explicit Reader(Bytes bytes) : bytes_(bytes) {}

    bool ok() const { return ok_; }
    std::size_t position() const { return at_; }
    void seek(std::size_t at) { at_ = at; if (at_ > bytes_.size()) ok_ = false; }
    bool done() const { return !ok_ || at_ >= bytes_.size(); }
    void fail() { ok_ = false; }

    std::uint64_t unsigned_of(std::size_t size) {
        if (!ok_ || size > bytes_.size() - std::min(at_, bytes_.size()) || at_ > bytes_.size()) {
            ok_ = false;
            return 0;
        }
        std::uint64_t value = 0;
        std::memcpy(&value, bytes_.data() + at_, size);
        at_ += size;
        return value;
    }
    std::int64_t signed_of(std::size_t size) {
        const auto value = unsigned_of(size);
        const auto shift = 64 - 8 * size;
        return static_cast<std::int64_t>(value << shift) >> shift;
    }
    std::uint64_t uleb() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64 && ok_; shift += 7) {
            const auto byte = unsigned_of(1);
            value |= (byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) break;
        }
        return value;
    }
    std::int64_t sleb() {
        std::uint64_t value = 0;  // sign-extended through unsigned: no signed shift or negation can overflow
        unsigned shift = 0;
        std::uint64_t byte = 0;
        while (ok_ && shift < 64) {
            byte = unsigned_of(1);
            value |= (byte & 0x7f) << shift;
            shift += 7;
            if ((byte & 0x80) == 0) break;
        }
        if (shift < 64 && (byte & 0x40) != 0) value |= ~std::uint64_t{0} << shift;
        return static_cast<std::int64_t>(value);
    }

private:
    Bytes bytes_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

constexpr std::uint8_t pe_omit = 0xff;

/// A DW_EH_PE-encoded value at the reader's position: its value, with the pc-relative part resolved against
/// `field_address`. Only the forms compilers write for AArch64 are read; any other fails the reader.
std::optional<std::uint64_t> encoded(Reader &reader, std::uint8_t encoding, std::uint64_t field_address) {
    if (encoding == pe_omit) return std::nullopt;
    std::int64_t value = 0;
    switch (encoding & 0x0f) {
        case 0x00: value = static_cast<std::int64_t>(reader.unsigned_of(8)); break;  // absptr
        case 0x01: value = static_cast<std::int64_t>(reader.uleb()); break;
        case 0x02: value = static_cast<std::int64_t>(reader.unsigned_of(2)); break;
        case 0x03: value = static_cast<std::int64_t>(reader.unsigned_of(4)); break;
        case 0x04: value = static_cast<std::int64_t>(reader.unsigned_of(8)); break;
        case 0x09: value = reader.sleb(); break;
        case 0x0a: value = reader.signed_of(2); break;
        case 0x0b: value = reader.signed_of(4); break;
        case 0x0c: value = reader.signed_of(8); break;
        default: reader.fail(); return std::nullopt;
    }
    if ((encoding & 0x70) == 0x10) value += static_cast<std::int64_t>(field_address);  // pcrel
    return static_cast<std::uint64_t>(value);
}

/// The [begin, end) of every FDE of .eh_frame at `address` (its section address). Reads CIEs for the pointer encoding
/// of an augmentation "zR..." and gives up on one it cannot read, counting what it has read.
std::vector<std::pair<std::uint64_t, std::uint64_t>> read_fdes(Bytes section, std::uint64_t address) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> fdes;
    std::map<std::size_t, std::uint8_t> cie_encodings;  // CIE offset -> its R encoding (absptr when none)
    Reader reader(section);
    while (!reader.done()) {
        const std::size_t start = reader.position();
        std::uint64_t length = reader.unsigned_of(4);
        if (!reader.ok() || length == 0) break;
        if (length == 0xffff'ffff) length = reader.unsigned_of(8);
        const std::size_t body = reader.position();
        if (!reader.ok() || length > section.size() - body) break;
        const std::size_t end = body + static_cast<std::size_t>(length);
        const std::uint32_t id = static_cast<std::uint32_t>(reader.unsigned_of(4));
        if (id == 0) {  // a CIE
            const auto version = reader.unsigned_of(1);
            std::string augmentation;
            for (auto byte = reader.unsigned_of(1); reader.ok() && byte != 0; byte = reader.unsigned_of(1)) {
                augmentation.push_back(static_cast<char>(byte));
            }
            std::uint8_t encoding = 0x00;
            if (version == 1 || version == 3 || version == 4) {
                reader.uleb();  // code alignment
                reader.sleb();  // data alignment
                if (version == 1) reader.unsigned_of(1); else reader.uleb();  // return address register
                if (!augmentation.empty() && augmentation[0] == 'z') {
                    const auto augmentation_length = reader.uleb();
                    const std::size_t augmentation_end =
                        reader.position() + static_cast<std::size_t>(augmentation_length);
                    for (std::size_t index = 1; index < augmentation.size() && reader.ok(); ++index) {
                        if (augmentation[index] == 'R') {
                            encoding = static_cast<std::uint8_t>(reader.unsigned_of(1));
                        } else if (augmentation[index] == 'L') {
                            reader.unsigned_of(1);
                        } else if (augmentation[index] == 'P') {
                            const auto personality = static_cast<std::uint8_t>(reader.unsigned_of(1));
                            encoded(reader, personality, 0);
                        }
                    }
                    reader.seek(augmentation_end);
                }
            }
            if (reader.ok()) cie_encodings[start] = encoding;
        } else {  // an FDE: id is the distance from this field back to its CIE
            const std::size_t id_field = body;
            if (id <= id_field) {
                const auto cie = cie_encodings.find(id_field - id);
                if (cie != cie_encodings.end()) {
                    const auto begin = encoded(reader, cie->second, address + reader.position());
                    const auto range = encoded(reader, cie->second & 0x0f, 0);
                    if (reader.ok() && begin && range) fdes.emplace_back(*begin, *begin + *range);
                }
            }
        }
        reader.seek(end);
    }
    return fdes;
}

std::int64_t sign_extend(std::uint64_t value, unsigned bits) {
    const auto shift = 64 - bits;
    return static_cast<std::int64_t>(value << shift) >> shift;
}

/// The sorted addresses of the refused sites, and which of them some load read.
struct Sites {
    std::vector<std::uint64_t> address;
    std::vector<char> loaded;

    /// A load of [begin, end) bytes reads every site word [a, a + 4) that it overlaps.
    void load(std::uint64_t begin, std::uint64_t end) {
        auto at = std::ranges::lower_bound(address, begin >= 3 ? begin - 3 : 0);
        for (; at != address.end() && *at < end; ++at) {
            loaded[static_cast<std::size_t>(at - address.begin())] = 1;
        }
    }
};

/// Loads that code makes from addresses it forms itself: `ldr` (literal), and `ldr`/`ldp` with an unsigned or signed
/// offset through a register that adr, adrp or add (immediate) set within the last 16 words, marked on `sites` as they
/// are found. Straight-line only: any branch or return forgets every register. Register 31 is never tracked (it is
/// xzr for adr and adrp, sp as a base). Every register write it knows of is forgotten: the destination Rt, the second
/// register of a load/store pair or exclusive pair, the status register Rs of a store-exclusive and a CAS, the base of
/// a pre/post-indexed access and of a post-indexed vector structure load. A word that writes some other register than
/// those is not followed; its form is not one that compilers use between an adr and the load that reads its target.
void scan_loads(Bytes code, std::uint64_t address, Sites &sites) {
    std::array<std::optional<std::uint64_t>, 32> known;
    std::array<int, 32> age{};
    for (std::size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
        std::uint32_t word;
        std::memcpy(&word, code.data() + offset, 4);
        const std::uint64_t pc = address + offset;
        for (std::size_t reg = 0; reg < 32; ++reg) {
            if (known[reg] && ++age[reg] > 16) known[reg].reset();
        }
        const unsigned rd = word & 31;
        const unsigned rn = (word >> 5) & 31;
        // Branches and returns end the straight line.
        if ((word & 0x7c00'0000) == 0x1400'0000 || (word & 0xfe00'0000) == 0xd600'0000 ||
            (word & 0x7e00'0000) == 0x3400'0000 || (word & 0xff00'0010) == 0x5400'0000 ||
            (word & 0x7e00'0000) == 0x3600'0000) {
            known.fill(std::nullopt);
            continue;
        }
        if ((word & 0x3b00'0000) == 0x1800'0000) {  // ldr (literal): ldr w/x/s/d/q, ldrsw
            const auto opc = word >> 30;
            const bool vector = (word & 0x0400'0000) != 0;
            if (!(opc == 3 && !vector)) {  // not prfm
                const std::uint64_t size = vector ? (opc == 0 ? 4 : opc == 1 ? 8 : 16) : (opc == 1 ? 8 : 4);
                const auto target = pc + static_cast<std::uint64_t>(sign_extend((word >> 5) & 0x7ffff, 19) * 4);
                sites.load(target, target + size);
            }
            known[rd].reset();
            continue;
        }
        if ((word & 0x9f00'0000) == 0x1000'0000 || (word & 0x9f00'0000) == 0x9000'0000) {  // adr, adrp
            const auto imm = (((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3);
            const bool page = (word & 0x8000'0000) != 0;
            known[rd].reset();
            if (rd != 31) {
                const auto offset_bytes = static_cast<std::uint64_t>(sign_extend(imm, 21)) * (page ? 4096 : 1);
                known[rd] = (page ? pc & ~std::uint64_t{0xfff} : pc) + offset_bytes;
                age[rd] = 0;
            }
            continue;
        }
        if ((word & 0xff80'0000) == 0x9100'0000 && known[rn]) {  // add xd, xn, #imm{, lsl #12}
            const auto imm = static_cast<std::uint64_t>((word >> 10) & 0xfff) << (((word >> 22) & 1) ? 12 : 0);
            const auto value = *known[rn] + imm;
            known[rd].reset();
            if (rd != 31) {
                known[rd] = value;
                age[rd] = 0;
            }
            continue;
        }
        if ((word & 0x3b40'0000) == 0x3940'0000 && known[rn]) {  // ldr (unsigned offset): the loads, not the stores
            const auto size_field = word >> 30;
            const bool vector = (word & 0x0400'0000) != 0;
            const auto opc = (word >> 22) & 3;
            const unsigned shift = vector && opc >= 2 ? 4 : size_field;
            const auto target = *known[rn] + (static_cast<std::uint64_t>((word >> 10) & 0xfff) << shift);
            sites.load(target, target + (std::uint64_t{1} << shift));
        } else if ((word & 0x3fc0'0000) == 0x2940'0000 && known[rn]) {  // ldp (signed offset), general registers
            const auto opc = word >> 30;
            const unsigned shift = opc == 2 ? 3 : 2;
            const auto target = *known[rn] + static_cast<std::uint64_t>(sign_extend((word >> 15) & 0x7f, 7) << shift);
            sites.load(target, target + (std::uint64_t{2} << shift));
        }
        // What this word writes besides Rt: forget it, after the load above used the base.
        known[rd].reset();
        const unsigned rt2 = (word >> 10) & 31;
        const unsigned rs = (word >> 16) & 31;
        if ((word & 0x3a00'0000) == 0x2800'0000) {  // load/store pair, every form
            known[rt2].reset();
            const auto index = (word >> 23) & 3;
            if (index == 1 || index == 3) known[rn].reset();  // post-index, pre-index
        } else if ((word & 0x3b20'0000) == 0x3800'0000) {  // load/store, immediate forms (bit 21 clear)
            const auto index = (word >> 10) & 3;
            if (index == 1 || index == 3) known[rn].reset();  // post-index, pre-index
        } else if ((word & 0x3f00'0000) == 0x0800'0000) {  // exclusive, ordered, CAS and CASP
            known[rs].reset();
            known[rt2].reset();
        } else if ((word & 0xbf80'0000) == 0x0c80'0000 || (word & 0xbf80'0000) == 0x0d80'0000) {  // ld1..ld4, post
            known[rn].reset();
        }
    }
}

/// The class of the word in front of a refused site, for DataHints::Word::before.
char kind_of_instruction(std::uint32_t word) {
    if ((word & 0xffff'fc1f) == 0xd65f'0000) return 'r';                                       // ret
    if ((word & 0xfc00'0000) == 0x1400'0000) return 'b';                                       // b
    if ((word & 0xfc00'0000) == 0x9400'0000 || (word & 0xffff'fc1f) == 0xd63f'0000) return 'c';  // bl, blr
    return 'x';
}

}  // namespace

DataHints data_hints(const elf::File &file, std::span<const std::byte> elf_bytes,
                     std::span<const elf::Section> sections, std::span<const rewrite::Unsupported> uses) {
    DataHints hints;
    hints.sites = uses.size();
    const auto inside = [](const elf::Section &section, std::uint64_t address) {
        return address >= section.address && address - section.address < section.size;
    };
    const auto bytes_of = [&](const elf::Section &section) -> Bytes {
        if (section.type == elf::section_type_nobits || section.offset > elf_bytes.size() ||
            section.size > elf_bytes.size() - section.offset) {
            return {};
        }
        return elf_bytes.subspan(static_cast<std::size_t>(section.offset), static_cast<std::size_t>(section.size));
    };

    hints.eh_frame_hdr = std::ranges::any_of(sections, [](const elf::Section &section) {
                             return section.name == ".eh_frame_hdr";
                         }) ||
                         std::ranges::any_of(file.segments, [](const elf::Segment &segment) {
                             return segment.type == elf::SegmentType::gnu_eh_frame;
                         });
    std::vector<std::pair<std::uint64_t, std::uint64_t>> fdes;
    for (const auto &section : sections) {
        if (section.name != ".eh_frame") continue;
        hints.eh_frame = true;
        fdes = read_fdes(bytes_of(section), section.address);
    }
    hints.fdes = fdes.size();
    Sites sites;
    for (const auto &use : uses) sites.address.push_back(use.address);
    std::ranges::sort(sites.address);
    sites.loaded.assign(sites.address.size(), 0);
    for (const auto &section : sections) {
        if ((section.flags & elf::section_executable) == 0 || (section.flags & elf::section_allocated) == 0) continue;
        ++hints.executable_sections;
        scan_loads(bytes_of(section), section.address, sites);
    }
    for (const auto &segment : file.segments) {
        if (segment.type != elf::SegmentType::load || (segment.flags & elf::segment_executable) == 0) continue;
        for (const auto &section : sections) {
            if ((section.flags & elf::section_allocated) != 0 && (section.flags & elf::section_executable) == 0 &&
                section.type != elf::section_type_nobits && section.address >= segment.address &&
                section.address - segment.address < segment.memory_size) {
                hints.other_bytes_in_executable_segments += section.size;
            }
        }
    }
    std::vector<std::uint64_t> addresses;
    for (const auto &use : uses) addresses.push_back(use.address);
    std::ranges::sort(addresses);
    for (const auto address : addresses) {
        if (hints.words.size() >= DataHints::kWords) break;
        DataHints::Word word;
        word.address = address;
        const auto holder = std::ranges::find_if(sections, [&](const elf::Section &section) {
            return (section.flags & elf::section_allocated) != 0 && inside(section, address);
        });
        if (holder != sections.end()) {
            word.section = holder->name;
            word.executable = (holder->flags & elf::section_executable) != 0;
            const auto code = bytes_of(*holder);
            if (address - holder->address >= 4 && address - holder->address + 4 <= code.size()) {
                std::uint32_t before;
                std::memcpy(&before, code.data() + (address - holder->address - 4), 4);
                word.before = kind_of_instruction(before);
            }
        }
        word.in_fde = std::ranges::any_of(
            fdes, [&](const auto &fde) { return address >= fde.first && address < fde.second; });
        const auto at = std::ranges::lower_bound(sites.address, address);
        word.loaded = sites.loaded[static_cast<std::size_t>(at - sites.address.begin())] != 0;
        hints.words.push_back(word);
    }
    for (const auto &use : uses) {
        const auto covered = [&](const auto &fde) { return use.address >= fde.first && use.address < fde.second; };
        if (std::ranges::none_of(fdes, covered)) {
            ++hints.sites_outside_fdes;
        }
        const auto at = std::ranges::lower_bound(sites.address, use.address);
        if (sites.loaded[static_cast<std::size_t>(at - sites.address.begin())] != 0) {
            ++hints.sites_loaded_as_data;
        }
        if (std::ranges::any_of(sections, [&](const elf::Section &section) {
                return (section.flags & elf::section_executable) != 0 && inside(section, use.address);
            })) {
            ++hints.sites_in_executable_sections;
        }
    }
    return hints;
}

std::string describe(const DataHints &hints) {
    std::string words;
    for (const auto &word : hints.words) {
        const char *before = word.before == 'r'   ? "after a return"
                             : word.before == 'b' ? "after a branch"
                             : word.before == 'c' ? "after a call"
                             : word.before == 's' ? "first word of its section"
                                                  : "after another instruction";
        words += std::format("{}{:#x} ({}{}, {}, {}, {})", words.empty() ? "; refused words: " : ", ", word.address,
                             word.section.empty() ? "in no section" : word.section,
                             word.section.empty() || word.executable ? "" : " (not executable)",
                             word.in_fde ? "inside an FDE" : "outside every FDE",
                             word.loaded ? "loaded from code" : "not loaded", before);
    }
    if (hints.sites > hints.words.size()) words += std::format(" and {} more", hints.sites - hints.words.size());
    return std::format(
        "; unwind: .eh_frame_hdr {}, .eh_frame {}, {} FDEs, {} of the {} sites outside every FDE; PC-relative loads: "
        "{} of the {} sites loaded from code; sections: {} executable, {} bytes of other sections in executable "
        "segments, {} of the {} sites in executable sections{}",
        hints.eh_frame_hdr ? "yes" : "no", hints.eh_frame ? "yes" : "no", hints.fdes, hints.sites_outside_fdes,
        hints.sites, hints.sites_loaded_as_data, hints.sites, hints.executable_sections,
        hints.other_bytes_in_executable_segments, hints.sites_in_executable_sections, hints.sites, words);
}

}  // namespace apk::hints
