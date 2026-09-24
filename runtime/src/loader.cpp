#include "loader.h"

#include "islands.h"
#include "syscall_frame.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <sys/param.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>

namespace apkrt::loader {
namespace {

// ELF (gABI, AArch64 ELF ABI) and Linux (include/uapi/linux/auxvec.h, arch/arm64 hwcap.h) constants.
constexpr std::uint32_t pt_dynamic = 2;
constexpr std::uint32_t pt_phdr = 6;
constexpr std::int64_t dt_null = 0;
constexpr std::int64_t dt_pltrelsz = 2;
constexpr std::int64_t dt_rela = 7;
constexpr std::int64_t dt_relasz = 8;
constexpr std::int64_t dt_jmprel = 23;
constexpr std::int64_t dt_relrsz = 35;
constexpr std::int64_t dt_relr = 36;
constexpr std::int64_t dt_android_relr = 0x6fff'e000;
constexpr std::int64_t dt_android_relrsz = 0x6fff'e001;
constexpr std::uint32_t r_aarch64_none = 0;
constexpr std::uint32_t r_aarch64_abs64 = 257;
constexpr std::uint32_t r_aarch64_copy = 1024;
constexpr std::uint32_t r_aarch64_glob_dat = 1025;
constexpr std::uint32_t r_aarch64_jump_slot = 1026;
constexpr std::uint32_t r_aarch64_relative = 1027;

constexpr std::uint64_t at_null = 0;
constexpr std::uint64_t at_phdr = 3;
constexpr std::uint64_t at_phent = 4;
constexpr std::uint64_t at_phnum = 5;
constexpr std::uint64_t at_pagesz = 6;
constexpr std::uint64_t at_base = 7;
constexpr std::uint64_t at_flags = 8;
constexpr std::uint64_t at_entry = 9;
constexpr std::uint64_t at_uid = 11;
constexpr std::uint64_t at_euid = 12;
constexpr std::uint64_t at_gid = 13;
constexpr std::uint64_t at_egid = 14;
constexpr std::uint64_t at_platform = 15;
constexpr std::uint64_t at_hwcap = 16;
constexpr std::uint64_t at_clktck = 17;
constexpr std::uint64_t at_secure = 23;
constexpr std::uint64_t at_random = 25;
constexpr std::uint64_t at_hwcap2 = 26;
constexpr std::uint64_t at_execfn = 31;

constexpr std::uint64_t guest_page_size = 16 * 1024;  // ADR-0007
constexpr std::uint64_t clock_ticks = 100;            // USER_HZ

struct ProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t vaddr;
    std::uint64_t paddr;
    std::uint64_t file_size;
    std::uint64_t memory_size;
    std::uint64_t alignment;
};
static_assert(sizeof(ProgramHeader) == 56);

struct DynamicEntry {
    std::int64_t tag;
    std::uint64_t value;
};

/// The word a relocation changes. Checked before anything is read or written: the code around the
/// writable part is signed, and outside the image lies whatever else the process has mapped.
std::uint64_t *relocation_target(std::uint64_t address, std::uint64_t bias, Range writable) {
    if (writable.end < writable.start + 8 || address < writable.start || address > writable.end - 8) {
        throw LoadError(std::format("relocation at {:#x} lies outside the writable part [{:#x}, {:#x})", address,
                                    writable.start, writable.end));
    }
    return reinterpret_cast<std::uint64_t *>(bias + address);
}

}  // namespace

void apply_relr(std::span<const std::uint64_t> table, std::uint64_t bias, Range writable) {
    // An even entry is an address, an odd one a bitmap of the 63 words after the last address.
    std::uint64_t where = 0;
    for (const auto entry : table) {
        if ((entry & 1) == 0) {
            *relocation_target(entry, bias, writable) += bias;
            where = entry + 8;
            continue;
        }
        for (unsigned bit = 1; bit < 64; ++bit) {
            if ((entry >> bit) & 1) *relocation_target(where + (bit - 1) * 8, bias, writable) += bias;
        }
        where += 63 * 8;
    }
}

void apply_rela(std::span<const Rela> table, std::uint64_t bias, Range writable, const SymbolResolver &resolve,
                const OtherRelocation &other) {
    for (const auto &entry : table) {
        const auto type = static_cast<std::uint32_t>(entry.info);
        const auto symbol = static_cast<std::uint32_t>(entry.info >> 32);
        const auto addend = static_cast<std::uint64_t>(entry.addend);
        if (type == r_aarch64_none) continue;
        auto *const target = relocation_target(entry.offset, bias, writable);
        switch (type) {
            case r_aarch64_relative:
                *target = bias + addend;
                break;
            case r_aarch64_abs64:
            case r_aarch64_glob_dat:
            case r_aarch64_jump_slot:
                if (!resolve) {
                    throw LoadError(std::format("relocation type {} at {:#x} names a symbol and needs the linker",
                                                type, entry.offset));
                }
                *target = (symbol == 0 ? 0 : resolve(symbol)) + addend;
                break;
            case r_aarch64_copy:
                throw LoadError(std::format("R_AARCH64_COPY at {:#x}: copy relocations are not supported, as on Android",
                                            entry.offset));
            default:
                if (other && other(entry, target)) break;
                throw LoadError(std::format("relocation type {} at {:#x} is not supported yet", type, entry.offset));
        }
    }
}

std::uint64_t build_start_stack(const StartInfo &start, std::span<std::byte> stack) {
    const auto bottom = reinterpret_cast<std::uint64_t>(stack.data());
    std::uint64_t top = bottom + stack.size();

    // Takes `size` bytes below everything placed so far.
    const auto reserve = [&](std::uint64_t size, std::uint64_t alignment) {
        if (size > top - bottom || ((top - size) & ~(alignment - 1)) < bottom) {
            throw LoadError(std::format("the start stack does not fit into {} bytes", stack.size()));
        }
        top = (top - size) & ~(alignment - 1);
        return top;
    };
    const auto place_string = [&](const std::string &text) {
        const auto at = reserve(text.size() + 1, 1);
        std::memcpy(reinterpret_cast<void *>(at), text.c_str(), text.size() + 1);
        return at;
    };

    const auto platform = place_string("aarch64");
    const auto execfn = place_string(start.arguments.empty() ? std::string() : start.arguments.front());
    std::vector<std::uint64_t> environment;
    for (const auto &entry : start.environment) environment.push_back(place_string(entry));
    std::vector<std::uint64_t> arguments;
    for (const auto &argument : start.arguments) arguments.push_back(place_string(argument));

    const auto random = reserve(16, 16);
    arc4random_buf(reinterpret_cast<void *>(random), 16);

    std::uint64_t program_headers = 0;
    const std::uint64_t headers_size = std::uint64_t{start.program_header_count} * sizeof(ProgramHeader);
    if (start.program_headers.size() < headers_size) throw LoadError("the program headers are cut short");
    if (start.static_program && start.program_header_count > 0) {
        program_headers = reserve(headers_size, 8);
        auto *copy = reinterpret_cast<ProgramHeader *>(program_headers);
        std::memcpy(copy, start.program_headers.data(), headers_size);
        for (std::uint32_t index = 0; index < start.program_header_count; ++index) {
            if (copy[index].type == pt_phdr) {
                copy[index].vaddr = copy[index].paddr = program_headers;
            } else {
                copy[index].vaddr += start.bias;
                copy[index].paddr += start.bias;
            }
        }
    } else if (start.program_header_count > 0) {
        program_headers = reinterpret_cast<std::uint64_t>(start.program_headers.data());
    }

    const std::pair<std::uint64_t, std::uint64_t> auxv[] = {
        {at_phdr, program_headers},
        {at_phent, sizeof(ProgramHeader)},
        {at_phnum, start.program_header_count},
        {at_pagesz, guest_page_size},
        {at_base, 0},  // no interpreter
        {at_flags, 0},
        {at_entry, start.entry},
        {at_uid, getuid()},
        {at_euid, geteuid()},
        {at_gid, getgid()},
        {at_egid, getegid()},
        {at_platform, platform},
        {at_hwcap, guest_hwcap},
        {at_hwcap2, 0},
        {at_clktck, clock_ticks},
        {at_secure, 0},
        {at_random, random},
        {at_execfn, execfn},
        {at_null, 0},
    };

    const std::uint64_t words = 1 + (arguments.size() + 1) + (environment.size() + 1) + 2 * std::size(auxv);
    const auto stack_pointer = reserve(8 * words, 16);
    auto *out = reinterpret_cast<std::uint64_t *>(stack_pointer);
    *out++ = arguments.size();
    for (const auto argument : arguments) *out++ = argument;
    *out++ = 0;
    for (const auto entry : environment) *out++ = entry;
    *out++ = 0;
    for (const auto &[type, value] : auxv) {
        *out++ = type;
        *out++ = value;
    }
    return stack_pointer;
}

Image load(const std::string &path) {
    char resolved[MAXPATHLEN];
    if (::realpath(path.c_str(), resolved) == nullptr) {
        throw LoadError(std::format("{}: {}", path, std::strerror(errno)));
    }
    // Loaded code is never unloaded (ADR-0003), so the handle is not kept.
    if (dlopen(resolved, RTLD_NOW | RTLD_LOCAL) == nullptr) throw LoadError(dlerror());

    const mach_header_64 *header = nullptr;
    for (std::uint32_t index = 0; index < _dyld_image_count() && header == nullptr; ++index) {
        if (std::strcmp(_dyld_get_image_name(index), resolved) == 0) {
            header = reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(index));
        }
    }
    if (header == nullptr) throw LoadError(std::format("{}: dyld loaded it but does not list it", resolved));

    unsigned long size = 0;
    const auto *info = reinterpret_cast<const apk_container_info *>(
        getsectiondata(header, APK_CONTAINER_SEGMENT, APK_CONTAINER_SECTION, &size));
    if (info == nullptr || size < sizeof(apk_container_info) ||
        std::memcmp(info->magic, APK_CONTAINER_MAGIC, sizeof info->magic) != 0) {
        throw LoadError(std::format("{}: not a container dylib (no {},{})", resolved, APK_CONTAINER_SEGMENT,
                                    APK_CONTAINER_SECTION));
    }
    if (info->version != APK_CONTAINER_VERSION) {
        throw LoadError(std::format("{}: container version {}, this runtime reads version {}", resolved,
                                    info->version, APK_CONTAINER_VERSION));
    }

    const auto base = reinterpret_cast<std::uint64_t>(header);
    auto *slots = reinterpret_cast<apk_runtime_slots *>(base + info->runtime_slots);
    slots->syscall_entry = reinterpret_cast<std::uint64_t>(&apkrt_syscall_entry);
    // Where a signal handler finds the x18 islands of the container (ADR-0020 X4).
    std::string_view name(resolved);
    name.remove_prefix(name.rfind('/') + 1);
    if (name.ends_with(".dylib")) name.remove_suffix(std::string_view(".dylib").size());
    islands::add(header, *info, name);
    return Image{.header = header, .info = info, .bias = base + info->image_offset - info->min_vaddr};
}

void relocate(const Image &image) {
    const auto &info = *image.info;
    if (info.phdr == 0) throw LoadError("the program headers are not in any LOAD segment");
    const auto *headers = reinterpret_cast<const ProgramHeader *>(image.bias + info.phdr);
    const ProgramHeader *dynamic = nullptr;
    for (std::uint32_t index = 0; index < info.phnum; ++index) {
        if (headers[index].type == pt_dynamic) dynamic = &headers[index];
    }
    if (dynamic == nullptr) return;

    std::uint64_t relr = 0, relr_size = 0, rela = 0, rela_size = 0, jmprel = 0, jmprel_size = 0;
    const auto *entries = reinterpret_cast<const DynamicEntry *>(image.bias + dynamic->vaddr);
    for (std::uint64_t index = 0; index < dynamic->memory_size / sizeof(DynamicEntry); ++index) {
        const auto &entry = entries[index];
        if (entry.tag == dt_null) break;
        switch (entry.tag) {
            case dt_relr:
            case dt_android_relr: relr = entry.value; break;
            case dt_relrsz:
            case dt_android_relrsz: relr_size = entry.value; break;
            case dt_rela: rela = entry.value; break;
            case dt_relasz: rela_size = entry.value; break;
            case dt_jmprel: jmprel = entry.value; break;
            case dt_pltrelsz: jmprel_size = entry.value; break;
            default: break;
        }
    }

    const Range writable{info.writable_start, info.writable_end};
    if (rela != 0) {
        apply_rela({reinterpret_cast<const Rela *>(image.bias + rela), rela_size / sizeof(Rela)}, image.bias, writable);
    }
    if (jmprel != 0) {
        apply_rela({reinterpret_cast<const Rela *>(image.bias + jmprel), jmprel_size / sizeof(Rela)}, image.bias,
                   writable);
    }
    if (relr != 0) {
        apply_relr({reinterpret_cast<const std::uint64_t *>(image.bias + relr), relr_size / 8}, image.bias, writable);
    }
}

}  // namespace apkrt::loader
