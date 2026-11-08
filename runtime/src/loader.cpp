#include "loader.h"

#include "crash_report.h"
#include "hwcap.h"
#include "identity.h"
#include "islands.h"
#include "monitor_probe.h"
#include "syscall_frame.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <sys/param.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
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
std::uint64_t *relocation_target(std::uint64_t address, std::uint64_t bias, const Writable &writable) {
    if (!writable.contains(address, 8)) {
        std::string ranges;
        for (const auto &range : writable.ranges) ranges += std::format(" [{:#x}, {:#x})", range.start, range.end);
        throw LoadError(std::format("relocation at {:#x} lies outside the writable part{}", address, ranges));
    }
    return reinterpret_cast<std::uint64_t *>(bias + address);
}

/// More runs than any image of 4 GiB in 16 KiB pages could have (the converter's limit).
constexpr std::uint64_t most_runs = (std::uint64_t{4} << 30) / (16 * 1024);

}  // namespace

void require_monitor_kept(std::uint32_t flags, std::string_view name) {
    if ((flags & APK_CONTAINER_MONITOR_INSIDE) == 0 || monitor::kept()) return;
    throw LoadError(std::format("{}: this library's x18 islands lie inside the guest's LL/SC loops, which needs a core that "
                                "keeps the exclusive monitor across a memory access to another granule; this device's core "
                                "clears it, so the loops would never end (ADR-0020 X7)", name));
}

bool Writable::contains(std::uint64_t address, std::uint64_t size) const {
    return std::ranges::any_of(ranges, [&](const Range &range) {
        return range.end >= range.start && range.end - range.start >= size && address >= range.start &&
               address <= range.end - size;
    });
}

Writable writable_part(const Image &image) {
    const auto &info = *image.info;
    if (info.version < 3) return Range{info.writable_start, info.writable_end};
    // The table lies in __TEXT between __apk_info and the image, which the code signature covers.
    if (info.run_count > most_runs || info.runs < sizeof(apk_container_info) ||
        info.runs + std::uint64_t{info.run_count} * sizeof(apk_run) > info.image_offset) {
        throw LoadError(std::format("the table of {} runs at {:#x} does not lie before the image at {:#x}",
                                    info.run_count, info.runs, info.image_offset));
    }
    const auto *runs = reinterpret_cast<const apk_run *>(reinterpret_cast<std::uint64_t>(image.header) + info.runs);
    // The runs cover the image from min_vaddr to its end in order, each after the one before, none empty.
    std::uint64_t expected = info.min_vaddr;
    for (std::uint32_t index = 0; index < info.run_count; ++index) {
        if (runs[index].start != expected || runs[index].end <= runs[index].start) expected = ~std::uint64_t{0};
        else expected = runs[index].end;
    }
    if (info.run_count == 0 || info.image_size > ~std::uint64_t{0} - info.min_vaddr ||
        expected != info.min_vaddr + info.image_size) {
        throw LoadError(std::format("the runs do not cover the image [{:#x}, {:#x}) in order", info.min_vaddr,
                                    info.min_vaddr + info.image_size));
    }
    std::vector<Range> parts;
    for (std::uint32_t index = 0; index < info.run_count; ++index) {
        if (runs[index].flags & APK_RUN_WRITABLE) parts.push_back({runs[index].start, runs[index].end});
    }
    return Writable(std::move(parts));
}

void apply_relr(std::span<const std::uint64_t> table, std::uint64_t bias, const Writable &writable) {
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

void apply_rela(std::span<const Rela> table, std::uint64_t bias, const Writable &writable,
                const SymbolResolver &resolve, const OtherRelocation &other) {
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

    // The strings as Linux's exec copies them (fs/exec.c), each below the one before: AT_EXECFN's, then the
    // environment's and the arguments' from the last to the first. So they lie in order from argv[0] on, and the
    // arguments form the one block that AndroidRuntime::setArgv0 clears and /proc/self/cmdline reads. The
    // platform's name goes below them, as create_elf_tables puts it.
    const auto execfn = place_string(start.arguments.empty() ? std::string() : start.arguments.front());
    std::vector<std::uint64_t> environment(start.environment.size());
    for (auto index = environment.size(); index-- > 0;) environment[index] = place_string(start.environment[index]);
    std::vector<std::uint64_t> arguments(start.arguments.size());
    for (auto index = arguments.size(); index-- > 0;) arguments[index] = place_string(start.arguments[index]);
    const auto platform = place_string("aarch64");

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
        {at_uid, app_uid},  // the app uid, also as the gid (ADR-0039 decision 3)
        {at_euid, app_uid},
        {at_gid, app_uid},
        {at_egid, app_uid},
        {at_platform, platform},
        {at_hwcap, hwcap::guest().hwcap},  // the device's CPU features (ADR-0047)
        {at_hwcap2, hwcap::guest().hwcap2},
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
    // A version 2 container, converted before the runs (ADR-0008 amendment), has a shorter __apk_info.
    constexpr auto version_2_size = offsetof(apk_container_info, runs);
    if (info == nullptr || size < version_2_size ||
        std::memcmp(info->magic, APK_CONTAINER_MAGIC, sizeof info->magic) != 0) {
        throw LoadError(std::format("{}: not a container dylib (no {},{})", resolved, APK_CONTAINER_SEGMENT,
                                    APK_CONTAINER_SECTION));
    }
    // Each version carries its own __apk_info size: version 2 ends before the runs, versions 3 to 5 before the (c)
    // R-ranges, version 6 and 7 before the entry hooks, version 8 the whole struct. A container must hold at least
    // what its version promises; a newer field
    // is read only for the versions that have it (writable_part reads the runs only for >= 3; islands::c_ranges the
    // ranges only for >= APK_CONTAINER_RANGES_VERSION).
    const std::size_t required = info->version < 3 ? version_2_size
                                 : info->version < APK_CONTAINER_RANGES_VERSION
                                       ? offsetof(apk_container_info, c_ranges)
                                 : info->version < APK_CONTAINER_ENTRY_HOOKS_VERSION
                                       ? offsetof(apk_container_info, entry_hooks)
                                       : sizeof(apk_container_info);
    if (info->version < APK_CONTAINER_OLDEST_VERSION || info->version > APK_CONTAINER_VERSION || size < required) {
        throw LoadError(std::format("{}: container version {}, this runtime reads versions {} to {}", resolved,
                                    info->version, APK_CONTAINER_OLDEST_VERSION, APK_CONTAINER_VERSION));
    }

    require_monitor_kept(info->flags, resolved);

    const auto base = reinterpret_cast<std::uint64_t>(header);
    auto *slots = reinterpret_cast<apk_runtime_slots *>(base + info->runtime_slots);
    slots->syscall_entry = reinterpret_cast<std::uint64_t>(&apkrt_syscall_entry);
    // Entry hooks (ADR-0072 decision 3 b): until an override applies (dl.cpp apply_entry_hooks), a hooked function runs
    // through its original stub, unchanged.
    if (info->version >= APK_CONTAINER_ENTRY_HOOKS_VERSION && info->entry_hook_count != 0) {
        if (info->entry_hook_count > APK_MAX_ENTRY_HOOKS) {
            throw LoadError(std::format("{}: {} entry hooks, at most {}", resolved, info->entry_hook_count,
                                        APK_MAX_ENTRY_HOOKS));
        }
        const auto *hooks = reinterpret_cast<const apk_entry_hook *>(base + info->entry_hooks);
        for (std::uint32_t index = 0; index < info->entry_hook_count; ++index) {
            slots->hook[index] = base + hooks[index].original;
        }
    }
    // Where a signal handler finds the x18 islands of the container (ADR-0020 X4).
    std::string_view name(resolved);
    name.remove_prefix(name.rfind('/') + 1);
    if (name.ends_with(".dylib")) name.remove_suffix(std::string_view(".dylib").size());
    islands::add(header, *info, name);
    // Record the image for the async-signal-safe crash/fault resolver (ADR-0032 decision 8): a signal handler names a
    // faulting address by its library and guest vaddr without a lock or allocation. The range and bias match
    // mapped_objects(); the soname is copied, so `name`'s view into the realpath buffer need not outlive this call.
    crash::register_module(base + info->image_offset, base + info->image_offset + info->image_size,
                           base + info->image_offset - info->min_vaddr, name);
    return Image{.header = header, .info = info, .bias = base + info->image_offset - info->min_vaddr};
}

std::uint64_t entry_hook_original(const Image &image, std::string_view name) {
    const auto *info = image.info;
    if (info == nullptr || info->version < APK_CONTAINER_ENTRY_HOOKS_VERSION) return 0;
    const auto base = reinterpret_cast<std::uint64_t>(image.header);
    const auto *hooks = reinterpret_cast<const apk_entry_hook *>(base + info->entry_hooks);
    for (std::uint32_t index = 0; index < info->entry_hook_count && index < APK_MAX_ENTRY_HOOKS; ++index) {
        if (std::string_view(hooks[index].name, ::strnlen(hooks[index].name, sizeof hooks[index].name)) == name) {
            return base + hooks[index].original;
        }
    }
    return 0;
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

    const auto writable = writable_part(image);
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
