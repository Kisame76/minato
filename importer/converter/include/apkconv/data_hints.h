// What a library tells, without symbols, about whether the words of an executable section that the scanner refuses
// are data (a literal pool, a jump table) and not code: counts only, for an x18 refusal on the iPhone, where the
// library never runs through apk-convert and each import costs the user a sitting. Nothing here decides anything.
#pragma once

#include "apkconv/elf.h"
#include "apkconv/rewrite.h"

#include <cstddef>
#include <cstdint>
#include <span>
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
/// in executable segments, 3 of the 3 sites in executable sections".
std::string describe(const DataHints &hints);

}  // namespace apk::hints
