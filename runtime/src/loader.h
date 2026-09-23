// The loader for container dylibs (ADR-0008, ADR-0019): finds a container dyld has loaded,
// applies its relocations and builds the Linux start stack of its program. Libraries and symbols
// are the linker's part (linker.h, ADR-0021).
#pragma once

#include "APKRuntime/container_format.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace apkrt::loader {

class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// ELF addresses [start, end).
struct Range {
    std::uint64_t start;
    std::uint64_t end;
};

/// Elf64_Rela.
struct Rela {
    std::uint64_t offset;
    std::uint64_t info;  // symbol << 32 | type
    std::int64_t addend;
};

/// Applies a RELR table: every address it lists gets `bias` added. Relocation targets must lie in
/// `writable`, because the code around them is signed (ADR-0010). Checked before each write.
void apply_relr(std::span<const std::uint64_t> table, std::uint64_t bias, Range writable);

/// AT_HWCAP of guest programs, which ifunc resolvers see as well: only what every ARMv8 CPU has and
/// XNU runs at EL0, FP and Advanced SIMD (HWCAP_FP, HWCAP_ASIMD). In particular no CPUID (bionic
/// would read MIDR_EL1, which traps on XNU), no SVE (the A17 Pro has none) and no SME (the M4 has
/// it, the A17 Pro does not). More features later, each one checked (M6).
inline constexpr std::uint64_t guest_hwcap = 0b11;

/// The host address of the symbol with the given number, for a relocation that names one.
using SymbolResolver = std::function<std::uint64_t(std::uint32_t symbol)>;

/// For the relocation types apply_rela does not know: returns whether it handled the entry. The
/// target has been checked to lie in the writable part already.
using OtherRelocation = std::function<bool(const Rela &entry, std::uint64_t *target)>;

/// Applies RELA entries: R_AARCH64_RELATIVE and _NONE, and with a resolver R_AARCH64_ABS64,
/// _GLOB_DAT and _JUMP_SLOT (S + A). Without one, a relocation with a symbol throws. So do
/// R_AARCH64_COPY, which Android's linker refuses as well, and types neither `other` nor this
/// function handles.
void apply_rela(std::span<const Rela> table, std::uint64_t bias, Range writable,
                const SymbolResolver &resolve = {}, const OtherRelocation &other = {});

/// What goes onto the start stack.
struct StartInfo {
    std::vector<std::string> arguments;    // argv; the first is also AT_EXECFN
    std::vector<std::string> environment;  // envp
    std::span<const std::byte> program_headers;  // the program's own, as mapped
    std::uint32_t program_header_count;
    std::uint64_t bias;   // host address of ELF address 0
    std::uint64_t entry;  // host address of e_entry
    /// A static program gets a copy of its program headers with absolute addresses: the static
    /// libc of the NDK assumes a load bias of 0 and reads PT_NOTE, PT_TLS and PT_GNU_RELRO at their
    /// p_vaddr, which works for us only if p_vaddr already is the address in memory.
    bool static_program;
};

/// Lays out the start stack in `stack` the way Linux does for an executable: strings, 16 random
/// bytes and the program headers at the top, then auxv, envp, argv and argc from the returned stack
/// pointer (16-byte aligned) upwards. Throws LoadError when it does not fit.
std::uint64_t build_start_stack(const StartInfo &start, std::span<std::byte> stack);

/// A container dyld has loaded, with its runtime slots filled in.
struct Image {
    const void *header;               // the Mach-O header
    const apk_container_info *info;   // __TEXT,__apk_info
    std::uint64_t bias;               // host address = bias + ELF address
};

/// dlopen()s the signed container at `path` (never unloaded, ADR-0003), checks its metadata and
/// points its syscall slot at the syscall entry.
Image load(const std::string &path);

/// Applies the relocations the program's PT_DYNAMIC lists.
void relocate(const Image &image);

}  // namespace apkrt::loader
