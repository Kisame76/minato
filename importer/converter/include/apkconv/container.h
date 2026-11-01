// The converter: one ELF file of an APK in, one unsigned container dylib out (ADR-0008, ADR-0010,
// ADR-0019). The layout is documented in APKRuntime/container_format.h, which the loader shares.
#pragma once

#include "apkconv/elf.h"
#include "apkconv/macho.h"
#include "apkconv/rewrite.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace apk::container {

struct Options {
    /// LC_ID_DYLIB, e.g. "@rpath/libgame.so.dylib".
    std::string install_name;
    macho::Platform platform = macho::Platform::macos;
    std::uint32_t platform_version = 26u << 16;
};

/// What the converter did with one file. ADR-0010: every change to the code is counted.
struct Report {
    bool is_static = false;          // no PT_INTERP and no DT_NEEDED
    std::uint64_t code_bytes = 0;    // bytes of the executable sections, all of them scanned
    std::uint32_t thread_pointer_reads = 0;
    std::uint32_t thread_pointer_writes = 0;
    std::uint32_t syscalls = 0;
    std::uint32_t x18_writes = 0;    // instructions that only write x18
    std::uint32_t x18_saves = 0;     // x18 stored in a dump of all registers
    std::uint32_t x18_islands = 0;   // instructions run with a scratch register for x18 (ADR-0020)
    std::uint64_t relocations = 0;   // RELR addresses plus RELA entries, all in writable memory
    std::uint64_t image_offset = 0;  // where the ELF image starts in the container
    std::uint64_t image_shift = 0;   // how far a 4 KiB layout moved it past a 16 KiB page (ADR-0007)
    std::uint64_t stub_bytes = 0;    // islands and veneers
    std::uint64_t island_table_bytes = 0;  // one entry per x18 island (ADR-0020 X4)
    std::string moved_sections_note;  // allocated sections a 4 KiB-multiple shift moved off a >4 KiB alignment
                                      // (ADR-0007); empty when none. A note, not a refusal: see page_layout.
};

struct Container {
    std::vector<std::byte> bytes;  // unsigned; codesign.h signs it (ADR-0022)
    Report report;
};

/// The file is a valid ELF file, but it cannot run on iOS as it is. The message says why, for
/// the import log and the "Incompatible" status.
class Incompatible : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A run of the image (ADR-0008 amendment): ELF addresses whose pages have one protection, one Mach-O
/// segment of the container. Non-writable pages are code (r-x), read-only data included.
struct Run {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool writable = false;
};

/// Where an ELF image goes in 16 KiB pages (ADR-0007 decision 2, as amended 2026-10-07, and ADR-0008
/// as amended 2026-10-11). Its LOAD segments are aligned to 4 KiB or more, and no 4 KiB page holds
/// both a writable and a non-writable one. The image splits into runs of pages with one protection
/// in address order, and the whole image moves by `shift` so that every boundary between runs starts
/// a 16 KiB page, as AOSP's 16 KiB compat mode moves its load bias: ADRP counts 4 KiB pages, so no
/// instruction or table in the image changes. A gap between segments goes to the run before, up to
/// the page of the segment after it.
struct PageLayout {
    std::uint64_t min_vaddr = 0;       // the first LOAD segment's 16 KiB page
    std::uint64_t writable_start = 0;  // the first writable run's start, else `end`
    std::uint64_t end = 0;             // past the last LOAD segment, on a 16 KiB page of the container
    std::uint64_t shift = 0;           // 0, 4, 8 or 12 KiB
    std::vector<Run> runs;             // from min_vaddr to end, in address order
    std::string moved_sections;        // allocated sections the shift moved off their >4 KiB alignment, for the note
};

/// The section headers for page_layout's and page_verdict's alignment decision: the file's, or none when it has
/// none or they cannot be read (a broken table counts as none; the layout then trusts the segment p_align).
std::vector<elf::Section> layout_sections(std::span<const std::byte> elf);

/// The layout of `file`. `sections` are its section headers (empty when the file has none, or they cannot be read).
/// A whole-image shift by a 4 KiB multiple keeps the loader's run-based 16 KiB page boundaries and every relative
/// offset, and bionic on a 4 KiB device maps with a 4 KiB load bias, so an allocated section's sh_addralign above
/// 4 KiB does not block the shift (ADR-0007): the sections a non-zero shift moves off such an alignment are reported
/// in PageLayout::moved_sections, not refused. The TLS block still blocks (our per-thread TLS must honour its
/// p_align), and without section headers the largest segment p_align stands. Throws Incompatible when no shift fits.
PageLayout page_layout(const elf::File &file, std::span<const elf::Section> sections);

/// The per-site facts an x18 refusal carries, for the ADR-0020 (b)/(c) split (counts only; the sites are
/// proprietary, so a protected library reaches us only through this refusal string on the iPhone). The first three
/// (how x18 reaches each refused use) sum to the number of uses; the last is how many sit in a function with a
/// spare register for a whole-function rename.
struct X18SiteCounts {
    std::size_t defined_in_loop = 0;     // x18 written in its exclusive construct before any read (a retry recomputes it)
    std::size_t live_in = 0;             // x18 live into the construct (zeroing corrupts it; no retry helps)
    std::size_t no_exclusive = 0;        // the use is in no exclusive construct
    std::size_t with_free_register = 0;  // the enclosing function has a register free for a whole-function rename
};

/// The reason for refusing a file whose x18 `uses` no island handles (at least one), for Incompatible: one use with
/// its address, or every reason with its count and first addresses, so that one refusal names all a library needs,
/// then the per-site `counts` (ADR-0020 (b)/(c)). Each address is named with the symbol it sits in (`symbols` from
/// read_symbols) and its section (`sections`), so an import of a protected library shows which component; a stripped
/// library names only the section, or nothing.
std::string x18_refusal(std::span<const rewrite::Unsupported> uses, std::span<const elf::Symbol> symbols,
                        std::span<const elf::Section> sections, const X18SiteCounts &counts);

/// The [begin, end) of the sized function symbol (`read_symbols`) that contains `address` — the tightest by start —
/// clamped to the section [base, limit); nullopt when no sized symbol contains it. Option (c) renames x18 across
/// exactly this run and never guesses a boundary the way the free-register count may (must-have #2): a RET-to-RET
/// guess that ran long could rename x18 in the next function and clobber it.
std::optional<std::pair<std::uint64_t, std::uint64_t>> sized_function_bounds(std::span<const elf::Symbol> symbols,
                                                                             std::uint64_t base, std::uint64_t limit,
                                                                             std::uint64_t address);

/// What option (c) needs to rename x18 across the function that holds an x18 site: the function's [begin, end)
/// (virtual addresses) and the caller-saved register (rewrite::free_caller_saved_register) guest x18 moves into.
struct WholeFunctionRename {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    unsigned reg = 0;
};

/// Decides option (c) for the x18 site at `address` in `elf`: the rename, or nullopt when (c) cannot take it. Refuses
/// unless a sized symbol bounds the WHOLE function -- sized_function_bounds, with no nested or overlapping sized
/// symbol, and no word that falls through into `begin` or out past `end` (must-have 2) -- a caller-saved register is
/// free across it (must-have 1), and no direct branch or computed address from outside enters its interior
/// (rewrite::has_external_entry -- must-have 3). (c) is not yet wired into convert(); this is the gate the emission
/// will call per refused site.
std::optional<WholeFunctionRename> c_rename_register(std::span<const std::byte> elf, std::uint64_t address);

/// What option (c) does to a file's refused x18 `uses` (step 4b): the distinct sized functions it renames and the
/// refused sites those cover. The refused sites NOT in `covered_sites` are the ones (c) cannot take, which convert()
/// still refuses. The uses are grouped by their sized function, one c_rename_register verdict each.
struct CRenamePlan {
    std::vector<WholeFunctionRename> functions;  // ascending by begin, each a function (c) renames
    std::vector<std::uint64_t> covered_sites;    // the refused x18 addresses inside those functions
};

/// The option (c) plan for `elf`'s refused x18 `uses` (see CRenamePlan). `elf` is the file scan_code saw.
CRenamePlan c_rename_plan(std::span<const std::byte> elf, std::span<const rewrite::Unsupported> uses);

/// Of the refused x18 `uses`, how many sit in a function option (c) would take (c_rename_plan's covered_sites count).
/// These are the sites (c) converts once wired into convert(); until then the converter still refuses them.
std::size_t x18_sites_c_would_take(std::span<const std::byte> elf, std::span<const rewrite::Unsupported> uses);

/// Of the refused x18 `uses` (from scan_code's x18_unsupported), how many sit in a function that has a register a
/// whole-function rename may use (rewrite::free_caller_saved_register), for the ADR-0020 option (c) split (counts
/// only; the sites are proprietary). A function's bounds come from the sized symbol (`read_symbols`) that contains
/// the use, else the run between the returns around it within its executable section. `elf` is the file scan_code saw.
/// This counts a function as fixable on its free register alone: the stricter gate c_rename_register may still refuse
/// it (nested symbol, fall-through edge, outside entry), and a function where x18 is live across a BL/BLR is fixable
/// only because the rename saves and restores the register around the call (the emission's around-call sync).
std::size_t x18_sites_with_a_free_function_register(std::span<const std::byte> elf,
                                                    std::span<const rewrite::Unsupported> uses);

/// The per-site facts for `findings.x18_unsupported`: the (b)/(c) split from the scan (defined-in-loop, live-in,
/// the rest in no exclusive construct) and the free-register count over `elf`. Assembled for x18_refusal.
X18SiteCounts x18_site_counts(std::span<const std::byte> elf, const rewrite::Findings &findings);

/// Converts `elf`. Throws Incompatible, or elf::FormatError when the file is broken.
Container convert(std::span<const std::byte> elf, const Options &options);

/// Every reason convert() would refuse `elf` for, each in convert()'s own words and in its order (the machine and the
/// file type, the layout, the section headers, the relocations, a Darwin syscall, an x18 no island handles); empty when
/// convert() takes the file. It makes the same tests as convert(), not copies of them, and writes nothing. A file that
/// cannot be read, or is not AArch64 code of a shared object, has that one reason, as nothing else about it matters. A
/// reason is one line: control characters in a name from the file are spaces.
std::vector<std::string> refusals(std::span<const std::byte> elf);

/// The code of a file as the rewriter sees it: what it finds in the executable sections, the only
/// bytes known to be instructions (R1).
struct CodeScan {
    rewrite::Findings findings;
    std::uint64_t code_bytes = 0;
};

/// Scans the executable sections of `elf` without converting it. Throws Incompatible when there are
/// no section headers or a section is not where its segment says, or elf::FormatError.
CodeScan scan_code(std::span<const std::byte> elf);

}  // namespace apk::container
