// Words of an executable section that are data (a literal pool, a jump table), found only from proof, so that the
// scanner neither takes them for instructions (an svc #imm, a CASP or an x18 use that is a constant) nor refuses the
// library for them.
//
//   P1  Evidence comes only from words in a trusted run. Trusted roots (function symbols with a size, DT_INIT and
//       DT_FINI, the init and fini array entries) start a descent that decodes straight on and follows direct
//       branches; it is trusted up to its first barrier: a call, a system call, a trap, a register branch, a word
//       that is no instruction a compiler writes. Only the words before the barrier load. Everything else reaches
//       only: the rest of the run behind a barrier, the callee of a call, the branch targets behind a barrier, roots
//       without proof (an FDE start, a label, a function symbol without a size, a $x mark, the entry point), a
//       pointer in data, a relocation addend, an address the code takes that no load used (never a symbol table).
//       A word that anything reaches is code; a load there is a contradiction and leaves the word code. A call to an
//       import that never returns (abort, __cxa_throw, ...) ends a run.
//   P2  The extent of the data is what a load is proven to read: the bytes of a literal load or of an access whose
//       address adr/adrp+add formed, and the n entries of a table read by an indexed load that a bound proves. The
//       bound has exactly this form: `cmp wI, #n` (or xI), then, with no other flag-setting word between, `b.hi` (n + 1
//       entries) or `b.hs` (n entries), and I is the load's index register, not written between the compare and the
//       load; the entries are 1, 2 or 4 bytes (scaled by their size, or bytes), read from a table at an address the
//       code formed. A jump table is that, plus an add of the entry to a base and a `br`: 4-byte signed entries added
//       to the table's own address (ldrsw, add), or 1- or 2-byte entries added to a code label shifted left by 2
//       (clang's compressed table); every entry must be an aligned address inside a section, and each becomes a
//       strong root. A dispatch that is no such table gives no proof, and a function with a `br` that nothing resolves
//       (inside its FDE range, or between the two roots around it) has no data classified anywhere in it.
//
// Nothing is data because it lies after data, between entries, or looks like a constant. A word that is data by this
// but is reached as code, or lies on a page whose adrp the scan could not follow to its use, stays code. Everything
// not proven stays code: scanned as before, rewritten if the rewriter can, and refused with the counts if it decodes as
// a site that cannot be rewritten. A mistake of the analysis costs a refusal, never a missed rewrite; the known
// exceptions are named in data_ranges.cpp.
#pragma once

#include "apkconv/elf.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace apk::data {

struct Range {
    std::uint64_t begin = 0;  // of the first data word
    std::uint64_t end = 0;    // past the last one
};

/// How far the descent proved a word to be code.
enum class Reach : char { none = 0, weak = 1, strong = 2 };

/// A run of words with the same reach, inside one section.
struct ReachRun {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    char level = 0;  // a Reach
};

/// What was classified, in words of 4 bytes, and what was not.
struct DataRanges {
    std::vector<Range> ranges;      // sorted, disjoint, word aligned, each inside one executable section
    std::size_t literal_words = 0;  // the bytes of a load from strongly reached code: ldr literal, or an access at an
                                    // address that adr/adrp+add formed, with the size of the access
    std::size_t table_words = 0;    // a table with a proven bound
    std::size_t dropped_reached = 0;     // proven data, but the word is reached as code (strongly or weakly): code
    std::size_t dropped_unresolved = 0;  // proven data, but an adrp whose use was not seen reaches into its page
    std::size_t dropped_dispatch = 0;    // proven data, but inside a function with an unresolved register branch
    std::size_t unbounded_tables = 0;    // an indexed load from a base the code formed, with no proven bound: no data
    std::size_t reached_words = 0;       // the words reached, strongly or weakly
    std::vector<ReachRun> reach;                // every word's reach, in runs, ascending
    std::vector<std::uint64_t> unproven_bases;  // bases that a strongly reached access used with no proven extent
    std::vector<Range> functions;               // sized function symbols and unwind entries, by start then end

    /// How far the descent proved the word at `address` to be code.
    Reach reach_of(std::uint64_t address) const;
    /// Whether the word at `address` is unreached and lies, in an unbroken run of unreached words, behind a base that
    /// a strongly reached access used without a proven extent.
    bool behind_unproven_base(std::uint64_t address) const;
    /// Whether both addresses lie in a known function (a sized symbol or an unwind entry) and not in the same one.
    /// A literal pool lies in the function that loads it; a word in another function is no literal of this one.
    bool in_different_functions(std::uint64_t a, std::uint64_t b) const;
};

/// One executable section's bytes at its address.
struct Code {
    std::span<const std::byte> bytes;
    std::uint64_t address = 0;
};

/// Where the descent starts, and what it is told. Addresses that are unaligned or outside the code are ignored.
struct Roots {
    std::vector<std::uint64_t> strong;
    std::vector<std::uint64_t> weak;
    std::vector<std::uint64_t> no_return;  // targets of calls that never return (the PLT entries of abort, ...)
    std::vector<Range> functions;          // the FDE ranges, for the span an unresolved `br` poisons
    std::vector<std::uint64_t> probable;   // roots without proof of code, see data_ranges.cpp
};

/// The core, on plain inputs.
DataRanges find_ranges(std::span<const Code> code, const Roots &roots);

/// Relative or indirect relative relocations of a packed table: `count` of them at offset, offset + delta, ..., all
/// with the same addend (a group that reads nothing per relocation is one run; any other relocation is a run of one).
struct PackedRun {
    std::uint64_t offset = 0;
    std::uint64_t delta = 0;
    std::uint64_t count = 1;
    std::uint64_t addend = 0;
};

/// The relative and indirect relative relocations of a packed SHT_ANDROID_RELA table (APS2), with their offsets.
/// `complete` (when given) says whether the table was read to the count it promises.
std::vector<PackedRun> packed_relocations(std::span<const std::byte> content, bool *complete = nullptr);

/// The addends of those relocations, one per run.
std::vector<std::uint64_t> packed_relocation_addends(std::span<const std::byte> content);

/// The ranges of an ELF library. Trusted roots: function symbols with a size, DT_INIT and DT_FINI, and the entries of
/// .init_array, .fini_array and .preinit_array (in place, and the addends of the relocations that fill them). Roots
/// that only reach: the FDEs of .eh_frame, other symbols that are not $d marks or objects, the entry point. Weak roots:
/// every 8-byte word of any other non-executable allocated
/// section that points into code, and the addends of the other code-pointer relocations of SHT_RELA and of the packed
/// SHT_ANDROID_RELA (APS2) tables. The calls that never return are found through the PLT: the imports named in
/// data_ranges.cpp's table. Never throws: what cannot be read adds no root, and the reached code, and so the data,
/// comes out smaller or none.
DataRanges find(const elf::File &file, std::span<const std::byte> elf_bytes, std::span<const elf::Section> sections,
                std::span<const elf::Symbol> symbols);

}  // namespace apk::data
