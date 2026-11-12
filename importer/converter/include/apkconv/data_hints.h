// What a library tells, without symbols, about whether the words of an executable section that the scanner refuses
// are data (a literal pool, a jump table) and not code: counts only, for an x18 refusal on the iPhone, where the
// library never runs through apk-convert and each import costs the user a sitting. Nothing here decides anything.
#pragma once

#include "apkconv/elf.h"
#include "apkconv/rewrite.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <string>

namespace apk::hints {

struct DataHints {
    bool eh_frame_hdr = false;   // a .eh_frame_hdr section or a PT_GNU_EH_FRAME segment
    bool eh_frame = false;       // a .eh_frame section
    std::size_t fdes = 0;        // the frame description entries read from it
    std::size_t sites = 0;       // the refused sites asked about
    std::size_t sites_outside_fdes = 0;       // sites that no FDE's [pc_begin, pc_begin + pc_range) covers
    std::size_t sites_loaded_as_data = 0;     // sites inside what a PC-relative load of code in the library reads
    std::size_t executable_sections = 0;      // sections with SHF_EXECINSTR
    std::uint64_t other_bytes_in_executable_segments = 0;  // bytes of non-executable sections inside an RX segment
    std::size_t sites_in_executable_sections = 0;          // sites that lie in a section with SHF_EXECINSTR

    /// What one refused site looks like, without a name of anything: where it lies and what stands in front of it.
    struct Word {
        std::uint64_t address = 0;
        std::string section;       // the section that holds it, empty when none does
        bool executable = false;   // that section has SHF_EXECINSTR
        bool in_fde = false;       // an FDE covers it
        bool loaded = false;       // a PC-relative load of code in the library reads it
        char before = 's';         // the word in front: r return, b branch, c call, s first word of its section,
                                   // x any other
        bool has_taken = false;    // some code of the library forms an address at or below the word, in its section
        std::uint64_t taken = 0;   // the greatest such address (adr, or adrp followed by add): where a blob would begin
        bool taken_fde_start = false;      // that address is the start of an FDE
        bool taken_branch_target = false;  // a direct branch or call of the library lands on it
        std::string around;        // the classes of the two words before and the two after, "xr|bc": r return,
                                   // b branch, c call, u reserved or unallocated (data-like), x any other, - none
    };
    std::vector<Word> words;       // the first kWords refused sites, by address
    static constexpr std::size_t kWords = 8;
};

/// The hints for the refused `uses` of the library `elf_bytes` (`file` from elf::read, `sections` from read_sections).
/// The FDEs come from .eh_frame (CIE augmentation "zR", DW_EH_PE absptr, udata, sdata and pcrel forms), the loads
/// from a scan of the executable sections for `ldr` (literal), `ldp`/`ldr` (unsigned offset) through a register
/// that `adr`, `adrp` or `add` set within the last 16 words of straight-line code. Never throws: what cannot be
/// read is not counted.
DataHints data_hints(const elf::File &file, std::span<const std::byte> elf_bytes,
                     std::span<const elf::Section> sections, std::span<const rewrite::Unsupported> uses);

/// The text appended to an x18 refusal: "; unwind: .eh_frame_hdr yes, .eh_frame yes, 1 FDEs, 2 of the 3 sites outside
/// every FDE; PC-relative loads: 1 of the 3 sites loaded from code; sections: 1 executable, 92 bytes of other sections
/// in executable segments, 3 of the 3 sites in executable sections; refused words: 0x230 (.text, outside every FDE,
/// loaded from code, after ret) [nearest taken address 0x1f0, 64 below, not an FDE start, a branch target; around
/// xr|bc], 0x238 (...)" (the first eight, and "and N more"); a word with no taken address at or below it in its
/// section says "no taken address below it in its section".
std::string describe(const DataHints &hints);

}  // namespace apk::hints
