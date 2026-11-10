// The files of /proc and /sys that the runtime writes (proc.h, ADR-0033 decision 8), and the property area
// (properties.h). Each open writes what Linux would show at that moment into an unlinked temporary file of
// the host and gives the guest that file, so that reads, seeks, fstat and mmap work as on any file. The
// formats are Linux 6.6's, the kernel of Android 15's emulator: fs/proc/array.c (stat), fs/proc/base.c
// (cmdline), fs/proc/task_mmu.c (maps), drivers/base/cpu.c (possible, online), arch/arm64/kernel/cpuinfo.c
// (cpuinfo), fs/proc/fd.c and the anonymous inodes (the links of fd/).
#include "proc.h"

#include "descriptors.h"
#include "dl.h"
#include "hwcap.h"
#include "memfd.h"
#include "properties.h"
#include "syscalls.h"
#include "threads.h"
#include "vfs.h"

#include <TargetConditionals.h>
#include <fcntl.h>
#include <mach/mach.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace apkrt::proc {
namespace {

/// A file the guest mapped (map_file): its range, its guest path, the file offset at `begin`, and the device and
/// inode its container's stat gives, split in Linux's way.
struct FileMapping {
    std::uint64_t begin = 0, end = 0;
    std::string path;
    std::uint64_t offset = 0;
    std::uint32_t major = 0, minor = 0;
    std::uint64_t inode = 0;
    bool shared = false;  // MAP_SHARED
};

struct State {
    std::mutex mutex;
    std::uint64_t stack_low = 0, stack_high = 0, stack_start = 0;
    std::uint64_t arguments_begin = 0, arguments_end = 0, environment_begin = 0, environment_end = 0;
    std::uint64_t start_ticks = 0;  // CLOCK_BOOTTIME at the start, in clock ticks
    bool has_main_thread = false;
    pthread_t main_thread{};
    std::string program;
    std::string executable;  // /proc/self/exe's target
    std::vector<FileMapping> files;  // in address order, apart from one another
};

/// The names in [begin, end) go; a mapping that reaches past either end keeps its rest, with its offset.
void forget(State &current, std::uint64_t begin, std::uint64_t end) {
    std::vector<FileMapping> kept;
    for (auto &mapping : current.files) {
        if (mapping.end <= begin || mapping.begin >= end) {
            kept.push_back(std::move(mapping));
            continue;
        }
        if (mapping.begin < begin) {
            auto left = mapping;
            left.end = begin;
            kept.push_back(std::move(left));
        }
        if (mapping.end > end) {
            auto right = mapping;
            right.offset += end - mapping.begin;
            right.begin = end;
            kept.push_back(std::move(right));
        }
    }
    current.files = std::move(kept);
}

void insert(State &current, FileMapping mapping) {
    const auto at = std::lower_bound(current.files.begin(), current.files.end(), mapping.begin,
                                     [](const FileMapping &a, std::uint64_t begin) { return a.begin < begin; });
    current.files.insert(at, std::move(mapping));
}

State &state() {
    static auto *instance = new State;  // as long as the process
    return *instance;
}

/// USER_HZ, the clock ticks of /proc: 100 on every Linux.
constexpr std::uint64_t ticks_per_second = 100;

std::uint64_t page_size() {
    return static_cast<std::uint64_t>(::getpagesize());
}

std::uint64_t page_down(std::uint64_t address) {
    return address & ~(page_size() - 1);
}

std::uint64_t page_up(std::uint64_t address) {
    return page_down(address + page_size() - 1);
}

/// The range of the strings `list` points to, a list ending in null: from the first to past the
/// last string's NUL; empty for an empty list.
std::pair<std::uint64_t, std::uint64_t> strings_range(const char *const *list) {
    std::size_t count = 0;
    while (list[count] != nullptr) ++count;
    if (count == 0) return {0, 0};
    const char *last = list[count - 1];
    return {reinterpret_cast<std::uint64_t>(list[0]), reinterpret_cast<std::uint64_t>(last + std::strlen(last) + 1)};
}

// --- Memory: /proc/meminfo and sysinfo ---------------------------------------------------------------------------

/// The memory the guest is told of, in bytes: the device's, what is free, and what the process may still use. On the
/// iPhone that is iOS's budget for the app (os_proc_available_memory), on the Mac the free and reclaimable pages.
/// free <= available <= total.
struct Memory {
    std::uint64_t total = 0, free = 0, available = 0;
};

Memory memory() {
    Memory memory;
    std::size_t length = sizeof memory.total;
    if (::sysctlbyname("hw.memsize", &memory.total, &length, nullptr, 0) != 0) memory.total = 0;
    vm_statistics64_data_t pages{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (::host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&pages), &count) ==
        KERN_SUCCESS) {
        memory.free = std::uint64_t{pages.free_count} * vm_page_size;
        memory.available = memory.free + (std::uint64_t{pages.inactive_count} + pages.purgeable_count) * vm_page_size;
    }
#if TARGET_OS_IPHONE
    memory.available = ::os_proc_available_memory();
#endif
    memory.available = std::min(memory.available, memory.total);
    memory.free = std::min(memory.free, memory.available);
    return memory;
}

/// Android 15's /proc/meminfo (Linux 6.6's meminfo_proc_show on the emulator): its lines in its order, each name
/// with its colon padded to 16 characters and the value in kB right-aligned in 8. Cached is what may be used beyond
/// the free memory, so that ActivityManager's availMem (free plus cached) is MemAvailable; Buffers, swap and the
/// kernel's own counts are 0, and VmallocTotal is arm64's with 48-bit addresses.
std::string meminfo() {
    const auto host = memory();
    const std::uint64_t total = host.total / 1024, free = host.free / 1024, available = host.available / 1024;
    const std::pair<std::string_view, std::uint64_t> lines[] = {
        {"MemTotal", total}, {"MemFree", free}, {"MemAvailable", available}, {"Buffers", 0},
        {"Cached", available - free}, {"SwapCached", 0}, {"Active", 0}, {"Inactive", 0}, {"Active(anon)", 0},
        {"Inactive(anon)", 0}, {"Active(file)", 0}, {"Inactive(file)", 0}, {"Unevictable", 0}, {"Mlocked", 0},
        {"SwapTotal", 0}, {"SwapFree", 0}, {"Dirty", 0}, {"Writeback", 0}, {"AnonPages", 0}, {"Mapped", 0},
        {"Shmem", 0}, {"KReclaimable", 0}, {"Slab", 0}, {"SReclaimable", 0}, {"SUnreclaim", 0}, {"KernelStack", 0},
        {"ShadowCallStack", 0}, {"PageTables", 0}, {"SecPageTables", 0}, {"NFS_Unstable", 0}, {"Bounce", 0},
        {"WritebackTmp", 0}, {"CommitLimit", 0}, {"Committed_AS", 0}, {"VmallocTotal", 257687552},
        {"VmallocUsed", 0}, {"VmallocChunk", 0}, {"Percpu", 0}, {"AnonHugePages", 0}, {"ShmemHugePages", 0},
        {"ShmemPmdMapped", 0}, {"FileHugePages", 0}, {"FilePmdMapped", 0}, {"CmaTotal", 0}, {"CmaFree", 0}};
    std::string text;
    for (const auto &[name, kilobytes] : lines) {
        text += std::format("{:<16}{:>8} kB\n", std::string(name) + ":", kilobytes);
    }
    return text;
}

// --- /proc/cpuinfo -------------------------------------------------------------------------------------

/// What Linux's c_show writes for each online CPU (ADR-0047 c): the Features line names the bits the guest is told
/// (hwcap.h), and BogoMIPS comes from the counter's frequency as Linux takes loops_per_jiffy from the architected
/// timer, with Android's HZ of 250. The MIDR fields are those Android 15 shows on Apple's hypervisor: XNU does not
/// let EL0 read the device's own.
std::string cpuinfo() {
    int cpus = 1;
    std::size_t length = sizeof cpus;
    if (::sysctlbyname("hw.activecpu", &cpus, &length, nullptr, 0) != 0 || cpus < 1) cpus = 1;
    std::uint64_t frequency = 0;
    length = sizeof frequency;
    if (::sysctlbyname("hw.tbfrequency", &frequency, &length, nullptr, 0) != 0 || frequency == 0) {
        frequency = 24'000'000;  // the counter of every Apple CPU
    }
    const std::uint64_t jiffy = frequency / 250;
    const std::string features = hwcap::features(hwcap::guest());
    std::string text;
    for (int cpu = 0; cpu < cpus; ++cpu) {
        text += std::format("processor\t: {}\nBogoMIPS\t: {}.{:02}\nFeatures\t:{}{}\nCPU implementer\t: 0x61\n"
                            "CPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x000\nCPU revision\t: 0\n\n",
                            cpu, jiffy / 2000, jiffy / 20 % 100, features.empty() ? "" : " ", features);
    }
    return text;
}

// --- /proc/self/stat -----------------------------------------------------------------------------------

/// Elf64_Phdr.
struct ProgramHeader {
    std::uint32_t type, flags;
    std::uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
constexpr std::uint32_t pt_load = 1;
constexpr std::uint32_t pf_x = 1;
constexpr std::uint64_t small_page_size = 4096;  // the pages of a library laid out for 4 KiB (ADR-0007)

/// What Linux's ELF loader records of the program's segments (load_elf_binary): its code, its data
/// and where its heap would start.
struct ProgramRanges {
    std::uint64_t start_code = 0, end_code = 0, start_data = 0, end_data = 0, start_brk = 0;
};

ProgramRanges program_ranges() {
    ProgramRanges ranges;
    const auto objects = dl::mapped_objects();
    if (objects.empty() || objects.front().program_headers == nullptr) return ranges;
    const auto &program = objects.front();
    const auto *headers = static_cast<const ProgramHeader *>(program.program_headers);
    std::uint64_t start_code = ~std::uint64_t{0}, brk = 0;
    for (std::size_t index = 0; index < program.program_header_count; ++index) {
        const auto &header = headers[index];
        if (header.type != pt_load) continue;
        const bool code = (header.flags & pf_x) != 0;
        if (code) start_code = std::min(start_code, header.vaddr);
        ranges.start_data = std::max(ranges.start_data, header.vaddr);
        if (code) ranges.end_code = std::max(ranges.end_code, header.vaddr + header.filesz);
        ranges.end_data = std::max(ranges.end_data, header.vaddr + header.filesz);
        brk = std::max(brk, header.vaddr + header.memsz);
    }
    if (start_code == ~std::uint64_t{0}) start_code = 0;
    ranges.start_code = program.bias + start_code;
    ranges.end_code += program.bias;
    ranges.start_data += program.bias;
    ranges.end_data += program.bias;
    ranges.start_brk = page_up(program.bias + brk);
    return ranges;
}

std::uint64_t ticks_of(const timeval &time) {
    return static_cast<std::uint64_t>(time.tv_sec) * ticks_per_second +
           static_cast<std::uint64_t>(time.tv_usec) / (1'000'000 / ticks_per_second);
}

/// The name of the main thread, the comm of /proc: 15 characters at most (TASK_COMM_LEN).
std::string main_thread_name(State &current) {
    std::string name = current.program;
    if (current.has_main_thread) {
        char own[64] = {};
        // ESRCH once the thread is gone: then the program's name.
        if (::pthread_getname_np(current.main_thread, own, sizeof own) == 0 && own[0] != '\0') name = own;
    }
    return name.substr(0, 15);
}

/// The 52 fields of do_task_stat, for the process as a whole.
std::string stat_line() {
    auto &current = state();
    std::unique_lock lock(current.mutex);
    const std::string comm = main_thread_name(current);
    const auto stack_start = current.stack_start, start_ticks = current.start_ticks;
    const auto arguments_begin = current.arguments_begin, arguments_end = current.arguments_end;
    const auto environment_begin = current.environment_begin, environment_end = current.environment_end;
    lock.unlock();

    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    mach_task_basic_info_data_t task{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    ::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task), &count);
    long threads = 0;
    apkrt::threads::for_each([&](apkrt::threads::GuestThread &) { ++threads; });
    const auto program = program_ranges();

    return std::format(
        "{} ({}) R {} {} {} 0 -1 4194304 {} 0 {} 0 {} {} 0 0 20 0 {} 0 {} {} {} 18446744073709551615 {} {} {} 0 0 "
        "0 0 0 0 0 0 0 17 0 0 0 0 0 0 {} {} {} {} {} {} {} 0\n",
        ::getpid(), comm, ::getppid(), ::getpgrp(), ::getsid(0), usage.ru_minflt, usage.ru_majflt,
        ticks_of(usage.ru_utime), ticks_of(usage.ru_stime), std::max(threads, 1L), start_ticks, task.virtual_size,
        task.resident_size / page_size(), program.start_code, program.end_code, stack_start, program.start_data,
        program.end_data, program.start_brk, arguments_begin, arguments_end, environment_begin, environment_end);
}

// --- /proc/self/cmdline --------------------------------------------------------------------------------

/// What Linux 6.6's get_mm_cmdline reads from the argument block as it is now: [arg_start, arg_end); or, when
/// the block's last byte is no NUL anymore (setproctitle), the string at arg_start with its NUL, read within a
/// page (get_mm_proctitle) and up to the environment's end where the environment follows the arguments.
std::string cmdline() {
    auto &current = state();
    std::unique_lock lock(current.mutex);
    const auto begin = current.arguments_begin, end = current.arguments_end, stack_high = current.stack_high;
    auto environment_begin = current.environment_begin, environment_end = current.environment_end;
    lock.unlock();
    if (begin >= end) return {};
    if (environment_begin != end || environment_end < environment_begin) environment_begin = environment_end = end;
    const auto *const text = reinterpret_cast<const char *>(begin);
    if (text[end - begin - 1] == '\0') return std::string(text, end - begin);
    // A page from arg_start, as far as the stack's memory goes.
    const std::size_t readable = std::min(page_size(), stack_high > begin ? stack_high - begin : 0);
    std::size_t length = ::strnlen(text, readable);
    if (length < readable) ++length;  // with its NUL
    return std::string(text, std::min<std::uint64_t>(length, environment_end - begin));
}

// --- /proc/self/maps -----------------------------------------------------------------------------------

/// A range of the host's address space with its protection, and whether another task shares it (as after a fork),
/// as the host's VM map has it.
struct Region {
    std::uint64_t begin, end;
    int protection;
    bool shared;
};

std::vector<Region> host_regions() {
    std::vector<Region> regions;
    vm_address_t address = 0;
    while (true) {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (::vm_region_64(::mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS) {
            break;
        }
        if (object != MACH_PORT_NULL) ::mach_port_deallocate(::mach_task_self(), object);
        // The host program's page zero is no mapping on Linux, where nothing lies at address 0.
        if (address != 0) regions.push_back({address, address + size, info.protection, info.shared != 0});
        if (address + size <= address) break;
        address += size;
    }
    return regions;
}

/// A range the guest knows a name for: the main stack, a segment of one of its ELF objects (with the
/// file offset at `begin`), or the zero-filled rest of a segment, which Android's linker names .bss.
struct Label {
    std::uint64_t begin, end;
    std::string name;
    bool file = false;
    std::uint64_t offset = 0;
    std::uint32_t major = 0, minor = 0;
    std::uint64_t inode = 0;
    bool shared = false;  // a MAP_SHARED file's
};

std::vector<Label> guest_labels() {
    std::vector<Label> labels;
    {
        auto &current = state();
        const std::lock_guard lock(current.mutex);
        if (current.stack_high > current.stack_low) {
            labels.push_back({.begin = current.stack_low, .end = current.stack_high, .name = "[stack]"});
        }
        for (const auto &mapping : current.files) {
            labels.push_back({.begin = mapping.begin, .end = mapping.end, .name = mapping.path, .file = true,
                              .offset = mapping.offset, .major = mapping.major, .minor = mapping.minor,
                              .inode = mapping.inode, .shared = mapping.shared});
        }
    }
    for (const auto &object : dl::mapped_objects()) {
        // The device and inode of the container, the file the object's pages come from.
        struct stat status{};
        ::stat(object.container.c_str(), &status);
        const auto device = static_cast<std::uint32_t>(status.st_dev);
        Label file{.begin = 0, .end = 0, .name = object.path, .file = true, .offset = 0, .major = (device >> 24) & 0xff,
                   .minor = device & 0xff'ffff, .inode = status.st_ino};
        if (object.program_headers == nullptr) {
            file.begin = object.image_begin;
            file.end = object.image_end;
            labels.push_back(file);
            continue;
        }
        const auto *headers = static_cast<const ProgramHeader *>(object.program_headers);
        // A library laid out for 4 KiB pages keeps the lines a 4 KiB device shows: the converter moved its
        // image as a whole (ADR-0007), so its first line starts at the bias, where crash reporters expect
        // file offset 0, and no line starts in the page before it.
        std::uint64_t granule = page_size();
        for (std::size_t index = 0; index < object.program_header_count; ++index) {
            if (headers[index].type == pt_load && headers[index].align < granule) granule = small_page_size;
        }
        const auto granule_down = [&](std::uint64_t address) { return address & ~(granule - 1); };
        const auto granule_up = [&](std::uint64_t address) { return granule_down(address + granule - 1); };
        for (std::size_t index = 0; index < object.program_header_count; ++index) {
            const auto &header = headers[index];
            if (header.type != pt_load) continue;
            const std::uint64_t start = object.bias + header.vaddr;
            const std::uint64_t begin = granule_down(start);
            const std::uint64_t file_end = header.filesz != 0 ? granule_up(start + header.filesz) : begin;
            const std::uint64_t memory_end = granule_up(start + header.memsz);
            if (file_end > begin) {
                file.begin = begin;
                file.end = file_end;
                file.offset = header.offset >= start - begin ? header.offset - (start - begin) : 0;
                labels.push_back(file);
            }
            if (memory_end > file_end) labels.push_back({.begin = file_end, .end = memory_end, .name = "[anon:.bss]"});
        }
    }
    // In address order and without overlaps: segments that share a page leave it to the first.
    std::sort(labels.begin(), labels.end(), [](const Label &a, const Label &b) { return a.begin < b.begin; });
    std::vector<Label> ordered;
    for (auto &label : labels) {
        if (!ordered.empty() && label.begin < ordered.back().end) {
            const auto cut = std::min(ordered.back().end, label.end);
            if (label.file) label.offset += cut - label.begin;
            label.begin = cut;
        }
        if (label.begin < label.end) ordered.push_back(std::move(label));
    }
    return ordered;
}

/// One line of show_map_vma: the header, and the name after spaces up to column 73 (seq_pad).
void append_line(std::string &text, std::uint64_t begin, std::uint64_t end, int protection, const Label *label,
                 std::uint64_t offset) {
    const std::size_t line_start = text.size();
    text += std::format("{:08x}-{:08x} {}{}{}{} {:08x} {:02x}:{:02x} {} ", begin, end,
                        (protection & VM_PROT_READ) ? 'r' : '-', (protection & VM_PROT_WRITE) ? 'w' : '-',
                        (protection & VM_PROT_EXECUTE) ? 'x' : '-', label != nullptr && label->shared ? 's' : 'p',
                        label != nullptr && label->file ? offset : 0,
                        label != nullptr ? label->major : 0, label != nullptr ? label->minor : 0,
                        label != nullptr ? label->inode : 0);
    if (label != nullptr) {
        constexpr std::size_t width = 25 + sizeof(void *) * 6 - 1;
        if (text.size() - line_start < width) text.append(width - (text.size() - line_start), ' ');
        text += ' ';
        text += label->name;
    }
    text += '\n';
}

/// A line of /proc/self/maps: part of one host mapping, with one name or none.
struct Piece {
    std::uint64_t begin, end;
    int protection;
    bool shared;
    const Label *label;
    std::uint64_t offset;
};

/// The host's mappings, each cut where the guest knows a name for a part of it (`labels`, into which the pieces
/// point). Whatever only the host maps stays anonymous, so that no host path reaches the guest. Neighbours with the
/// same protection and name, a file's at following offsets, are one piece, as Linux merges them.
std::vector<Piece> pieces_of(const std::vector<Label> &labels) {
    std::vector<Piece> pieces;
    const auto add = [&](const Piece &piece) {
        if (!pieces.empty()) {
            auto &last = pieces.back();
            const bool same_name = last.label == piece.label ||
                                   (last.label != nullptr && piece.label != nullptr &&
                                    last.label->name == piece.label->name && last.label->shared == piece.label->shared);
            const bool following = last.label == nullptr || !last.label->file ||
                                   last.offset + (last.end - last.begin) == piece.offset;
            if (last.end == piece.begin && last.protection == piece.protection && last.shared == piece.shared &&
                same_name && following) {
                last.end = piece.end;
                return;
            }
        }
        pieces.push_back(piece);
    };
    std::size_t next_label = 0;
    for (const auto &region : host_regions()) {
        for (std::uint64_t at = region.begin; at < region.end;) {
            while (next_label < labels.size() && labels[next_label].end <= at) ++next_label;
            if (next_label < labels.size() && labels[next_label].begin <= at) {
                const auto &label = labels[next_label];
                const auto end = std::min(region.end, label.end);
                add({at, end, region.protection, region.shared, &label, label.offset + (at - label.begin)});
                at = end;
            } else {
                const auto end =
                    next_label < labels.size() ? std::min(region.end, labels[next_label].begin) : region.end;
                add({at, end, region.protection, region.shared, nullptr, 0});
                at = end;
            }
        }
    }
    return pieces;
}

/// /proc/self/maps: a line for each piece.
std::string maps() {
    const auto labels = guest_labels();
    std::string text;
    for (const auto &piece : pieces_of(labels)) {
        append_line(text, piece.begin, piece.end, piece.protection, piece.label, piece.offset);
    }
    return text;
}

/// show_smap's sizes of a piece (Linux 6.6), in the form Android 15's emulator writes them [verified, 2026-10-07]:
/// each name, then its value in kB right-aligned to column 24. The host's pages say what is resident, dirty,
/// referenced and anonymous (mincore): a page dirty in the file cache counts, as Linux's PageDirty does, so a
/// freshly written file reads dirty until it is written back. A region the host shares with another task holds
/// shared pages, as Linux counts a page mapped by more than one process; Pss stays Rss, since the number of sharers
/// is unknown. Nothing is swapped or huge, so the rest is 0.
void append_sizes(std::string &text, const Piece &piece) {
    const std::uint64_t page = page_size(), length = piece.end - piece.begin;
    std::uint64_t resident = 0, dirty = 0, referenced = 0, anonymous = 0;
    std::vector<char> pages(static_cast<std::size_t>(length / page));
    if (!pages.empty() && ::mincore(reinterpret_cast<void *>(piece.begin), length, pages.data()) == 0) {
        for (const char flags : pages) {
            if ((flags & MINCORE_INCORE) == 0) continue;
            ++resident;
            if (flags & (MINCORE_MODIFIED | MINCORE_MODIFIED_OTHER)) ++dirty;
            if (flags & (MINCORE_REFERENCED | MINCORE_REFERENCED_OTHER)) ++referenced;
            if (flags & MINCORE_ANONYMOUS) ++anonymous;
        }
    }
    const auto field = [&](std::string_view name, std::uint64_t kilobytes) {
        text += std::format("{:<16}{:>8} kB\n", std::string(name) + ":", kilobytes);
    };
    const auto kb = [page](std::uint64_t pages_count) { return pages_count * page / 1024; };
    field("Size", length / 1024);
    field("KernelPageSize", page / 1024);
    field("MMUPageSize", page / 1024);
    field("Rss", kb(resident));
    field("Pss", kb(resident));
    field("Pss_Dirty", kb(dirty));
    field("Shared_Clean", piece.shared ? kb(resident - dirty) : 0);
    field("Shared_Dirty", piece.shared ? kb(dirty) : 0);
    field("Private_Clean", piece.shared ? 0 : kb(resident - dirty));
    field("Private_Dirty", piece.shared ? 0 : kb(dirty));
    field("Referenced", kb(referenced));
    field("Anonymous", kb(anonymous));
    for (const std::string_view name : {"KSM", "LazyFree", "AnonHugePages", "ShmemPmdMapped", "FilePmdMapped",
                                        "Shared_Hugetlb", "Private_Hugetlb", "Swap", "SwapPss", "Locked"}) {
        field(name, 0);
    }
    text += std::format("{:<16}{:>8}\n", "THPeligible:", 0);
    // show_smap_vma_flags: the mnemonics in the order of their bits, each with a space after it. A private mapping
    // that is writable is accounted (VM_ACCOUNT, "ac"); a MAP_SHARED file's says "sh" and "ms" instead, as for a
    // file open for writing.
    const bool writable = (piece.protection & VM_PROT_WRITE) != 0;
    const bool shared = piece.label != nullptr && piece.label->shared;
    text += "VmFlags: ";
    if (piece.protection & VM_PROT_READ) text += "rd ";
    if (writable) text += "wr ";
    if (piece.protection & VM_PROT_EXECUTE) text += "ex ";
    if (shared) text += "sh ";
    text += shared ? "mr mw me ms " : "mr mw me ";
    if (writable && !shared) text += "ac ";
    text += '\n';
}

/// /proc/self/smaps: each line of maps, then its sizes.
std::string smaps() {
    const auto labels = guest_labels();
    std::string text;
    for (const auto &piece : pieces_of(labels)) {
        append_line(text, piece.begin, piece.end, piece.protection, piece.label, piece.offset);
        append_sizes(text, piece);
    }
    return text;
}

// --- Paths ---------------------------------------------------------------------------------------------

/// The part of `path` below this process's directory, /proc/self/ or /proc/<pid>/: "stat", "fd/3";
/// nullopt for every other path.
std::optional<std::string_view> own_entry(std::string_view path) {
    constexpr std::string_view proc = "/proc/";
    if (!path.starts_with(proc)) return std::nullopt;
    path.remove_prefix(proc.size());
    const auto slash = path.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const auto process = path.substr(0, slash);
    if (process != "self" && process != std::to_string(::getpid())) return std::nullopt;
    return path.substr(slash + 1);
}

/// A host descriptor of an unlinked temporary file holding `contents`, at its start; -1 with errno.
int file_with(const std::string &contents) {
    char directory[PATH_MAX] = {};
    const auto length = ::confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof directory);
    std::string pattern = length > 0 && length <= sizeof directory ? directory : "/tmp/";
    if (!pattern.ends_with('/')) pattern += '/';
    pattern += "apkrt-proc-XXXXXX";
    const int host = ::mkostemp(pattern.data(), O_CLOEXEC);
    if (host < 0) return -1;
    ::unlink(pattern.c_str());
    for (std::size_t written = 0; written < contents.size();) {
        const auto count = ::write(host, contents.data() + written, contents.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            const int error = errno;
            ::close(host);
            errno = error;
            return -1;
        }
        written += static_cast<std::size_t>(count);
    }
    ::lseek(host, 0, SEEK_SET);
    return host;
}

/// What a link of /proc/self/fd shows for `fd`: the guest's path of its file, or Linux's name for what
/// has none. Nullopt when `fd` is not open, or its file lies outside every mount.
std::optional<std::string> descriptor_target(int fd) {
    const auto description = descriptors::get(fd);
    if (!description) return std::nullopt;
    if (const auto *file = memfd::as_file(*description)) return memfd::link_text(*file);
    if (!description->written_path.empty()) return description->written_path;
    using Kind = descriptors::Description::Kind;
    if (description->kind() == Kind::eventfd) return "anon_inode:[eventfd]";
    if (description->kind() == Kind::timerfd) return "anon_inode:[timerfd]";
    if (description->kind() == Kind::epoll) return "anon_inode:[eventpoll]";
    struct stat status{};
    if (::fstat(description->host(), &status) != 0) return std::nullopt;
    if (description->kind() == Kind::channel || S_ISSOCK(status.st_mode)) {
        return std::format("socket:[{}]", status.st_ino);
    }
    if (S_ISFIFO(status.st_mode)) return std::format("pipe:[{}]", status.st_ino);
    char host_path[MAXPATHLEN];
    if (::fcntl(description->host(), F_GETPATH, host_path) != 0) return std::nullopt;
    return vfs::guest_path(host_path);
}

}  // namespace

void set_main_stack(std::uint64_t low, std::uint64_t high, std::uint64_t start) {
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    current.stack_low = low;
    current.stack_high = high;
    current.stack_start = start;
    current.start_ticks = clock_gettime_nsec_np(CLOCK_MONOTONIC) / (1'000'000'000 / ticks_per_second);  // F10
    // argc, argv and envp at the start stack pointer, as the program finds them: where their strings lie.
    current.arguments_begin = current.arguments_end = current.environment_begin = current.environment_end = 0;
    if (start == 0) return;
    const auto *words = reinterpret_cast<const std::uint64_t *>(start);
    const auto *arguments = reinterpret_cast<const char *const *>(words + 1);
    std::tie(current.arguments_begin, current.arguments_end) = strings_range(arguments);
    std::tie(current.environment_begin, current.environment_end) = strings_range(arguments + words[0] + 1);
}

void set_main_thread(pthread_t thread, std::string_view program) {
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    current.has_main_thread = true;
    current.main_thread = thread;
    current.program = program;
}

void set_executable(std::string_view path) {
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    current.executable = path.starts_with('/')
                             ? std::string(path)
                             : (std::filesystem::path(vfs::working_directory()) / path).lexically_normal().string();
}

void map_file(std::uint64_t begin, std::uint64_t end, int fd, std::uint64_t offset, bool shared) {
    const auto description = descriptors::get(fd);
    const auto path = descriptor_target(fd);
    if (!description || !path) return;  // a file outside every mount stays anonymous, as the host's own do
    struct stat status{};
    ::fstat(description->host(), &status);
    const auto device = static_cast<std::uint32_t>(status.st_dev);
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    forget(current, begin, end);
    insert(current, {.begin = begin, .end = end, .path = *path, .offset = offset, .major = (device >> 24) & 0xff,
                     .minor = device & 0xff'ffff, .inode = status.st_ino, .shared = shared});
}

void unmap(std::uint64_t begin, std::uint64_t end) {
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    forget(current, begin, end);
}

bool range_is_file_backed(std::uint64_t begin, std::uint64_t end) {
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    for (const auto &mapping : current.files) {
        if (mapping.begin < end && mapping.end > begin) return true;
    }
    return false;
}

std::optional<MappedFile> file_mapping_at(std::uint64_t address) {
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    for (const auto &mapping : current.files) {
        if (mapping.begin <= address && address < mapping.end) {
            return MappedFile{.begin = mapping.begin, .end = mapping.end, .offset = mapping.offset,
                              .path = mapping.path, .major = mapping.major, .minor = mapping.minor,
                              .inode = mapping.inode};
        }
    }
    return std::nullopt;
}

void remap(std::uint64_t from, std::uint64_t old_length, std::uint64_t to, std::uint64_t new_length) {
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    const std::uint64_t kept = std::min(old_length, new_length);
    std::vector<FileMapping> moved;
    for (const auto &mapping : current.files) {
        const auto begin = std::max(mapping.begin, from), end = std::min(mapping.end, from + kept);
        if (begin >= end) continue;
        auto part = mapping;
        part.offset += begin - mapping.begin;
        part.begin = to + (begin - from);
        part.end = to + (end - from);
        moved.push_back(std::move(part));
    }
    forget(current, from, from + old_length);
    forget(current, to, to + new_length);
    for (auto &part : moved) insert(current, std::move(part));
}

std::optional<long> open(std::string_view path, int linux_flags) {
    std::optional<std::string> contents;
    std::string name(path);
    if (path == properties::path) {
        contents = properties::area();  // root's, read-only (sys_files.cpp)

    } else if (path == "/proc/cpuinfo") {
        contents = cpuinfo();
    } else if (path == "/proc/meminfo") {
        contents = meminfo();
    } else if (path == "/linkerconfig/apex.libraries.config.txt") {
        contents = dl::apex_libraries_config();
    } else if (const auto entry = own_entry(path)) {
        if (*entry == "stat") contents = stat_line();
        if (*entry == "maps") contents = maps();
        if (*entry == "smaps") contents = smaps();
        if (*entry == "cmdline") contents = cmdline();
        name = std::format("/proc/{}/{}", ::getpid(), *entry);  // /proc/self is a link to it
    }
    if (!contents) return std::nullopt;
    constexpr int access_mode = 03, nonblock = 04000, directory = 040000, cloexec = 02000000;
    if ((linux_flags & access_mode) != 0) return -sys::linux_errno::eacces;  // 0444, also for root
    if (linux_flags & directory) return -sys::linux_errno::enotdir;
    const int host = file_with(*contents);
    if (host < 0) return -sys::to_linux_errno(errno);
    auto description = std::make_shared<descriptors::Description>(host, false, false, (linux_flags & nonblock) != 0);
    description->written_path = std::move(name);
    return descriptors::add(std::move(description), (linux_flags & cloexec) != 0);
}

/// What the links exe, cwd and root of /proc/self show: the program's file, the working directory and the root, as
/// Linux's proc_exe_link, proc_cwd_link and proc_root_link give them; nullopt for another entry.
std::optional<std::string> process_link(std::string_view entry) {
    if (entry == "cwd") return vfs::working_directory();
    if (entry == "root") return std::string("/");
    if (entry != "exe") return std::nullopt;
    auto &current = state();
    const std::lock_guard lock(current.mutex);
    if (current.executable.empty()) return std::nullopt;
    return current.executable;
}

std::optional<long> read_link(std::string_view path, char *buffer, std::size_t size) {
    const auto entry = own_entry(path);
    if (!entry) return std::nullopt;
    if (const auto target = process_link(*entry)) {
        const auto length = std::min(size, target->size());
        std::memcpy(buffer, target->data(), length);
        return static_cast<long>(length);
    }
    if (!entry->starts_with("fd/")) return std::nullopt;
    const auto digits = entry->substr(3);
    int fd = -1;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), fd);
    if (error != std::errc{} || end != digits.data() + digits.size() || fd < 0) return -sys::linux_errno::enoent;
    const auto target = descriptor_target(fd);
    if (!target) return -sys::linux_errno::enoent;
    const auto length = std::min(size, target->size());
    std::memcpy(buffer, target->data(), length);
    return static_cast<long>(length);
}

std::optional<int> descriptor_entry(std::string_view path) {
    const auto entry = own_entry(path);
    if (!entry || !entry->starts_with("fd/")) return std::nullopt;
    const auto digits = entry->substr(3);
    int fd = -1;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), fd);
    if (error != std::errc{} || end != digits.data() + digits.size() || fd < 0) return std::nullopt;
    return fd;
}

std::optional<StatEntry> stat_entry(std::string_view path, bool follow) {
    using Type = StatEntry::Type;
    constexpr std::string_view proc = "/proc";
    if (path != proc && !path.starts_with("/proc/")) return std::nullopt;  // the host answers non-/proc paths
    const StatEntry regular{Type::regular, 0444, 1};
    const StatEntry link_rwx{Type::symlink, 0777, 1};  // exe, cwd, root, and /proc/self by lstat
    const StatEntry link_fd{Type::symlink, 0700, 1};   // /proc/<pid>/fd/<n>
    const auto directory = [](std::uint32_t nlink) { return StatEntry{Type::directory, 0555, nlink}; };
    // /proc: a directory. Its link count on Linux is the process count (per system, 376 on the AVD); the runtime
    // shows one process, so a small structural count, which no caller reads and the fixture does not assert.
    if (path == proc) return directory(3);
    if (path == "/proc/cpuinfo" || path == "/proc/meminfo") return regular;
    const auto entry = own_entry(path);  // the part under /proc/self/ or /proc/<pid>/, else nullopt
    if (!entry) {
        // /proc/self and /proc/<pid> themselves. /proc/self is a symlink to the pid unless followed; the process
        // directory counts two plus its subdirectories (task, fd, fdinfo, map_files, ns, net, attr) on Linux 6.6.
        if (path == "/proc/self") return follow ? directory(9) : link_rwx;
        if (path == std::format("/proc/{}", ::getpid())) return directory(9);
        return StatEntry{};  // another /proc path the runtime does not serve
    }
    if (*entry == "stat" || *entry == "maps" || *entry == "smaps" || *entry == "cmdline") return regular;
    if (*entry == "task") {
        long threads = 0;
        apkrt::threads::for_each([&](apkrt::threads::GuestThread &) { ++threads; });
        return directory(static_cast<std::uint32_t>(2 + std::max(threads, 1L)));  // two plus the guest's threads
    }
    if (*entry == "exe" || *entry == "cwd" || *entry == "root") return follow ? std::nullopt : std::optional(link_rwx);
    if (entry->starts_with("fd/")) {
        if (follow) return std::nullopt;  // the host resolves a descriptor link's target; lstat names it here
        const auto fd = descriptor_entry(path);
        return fd && descriptors::get(*fd) ? std::optional(link_fd) : std::optional(StatEntry{});
    }
    return StatEntry{};  // an entry under the process directory the runtime does not serve (status, environ, ...)
}

long sysinfo(std::uint64_t address) {
    // Linux's struct sysinfo on 64 bits, as do_sysinfo fills it: memory in bytes (mem_unit 1), as /proc/meminfo has
    // it, the loads in 1/65536 (SI_LOAD_SHIFT), the time since boot (CLOCK_BOOTTIME: Darwin's CLOCK_MONOTONIC counts
    // the time asleep too), and the threads, here the process's.
    struct LinuxSysinfo {
        std::int64_t uptime;
        std::uint64_t loads[3];
        std::uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
        std::uint16_t procs, pad;
        std::uint64_t totalhigh, freehigh;
        std::uint32_t mem_unit;
        char rest[4];
    };
    static_assert(sizeof(LinuxSysinfo) == 112 && offsetof(LinuxSysinfo, totalhigh) == 88 &&
                  offsetof(LinuxSysinfo, mem_unit) == 104);
    if (address == 0) return -sys::linux_errno::efault;
    LinuxSysinfo info{};
    const auto host = memory();
    info.totalram = host.total;
    info.freeram = host.free;
    info.mem_unit = 1;
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    info.uptime = now.tv_sec;
    double loads[3] = {};
    if (::getloadavg(loads, 3) == 3) {
        for (std::size_t index = 0; index < 3; ++index) {
            info.loads[index] = static_cast<std::uint64_t>(loads[index] * 65536);
        }
    }
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t thread_count = 0;
    if (::task_threads(mach_task_self(), &threads, &thread_count) == KERN_SUCCESS) {
        for (mach_msg_type_number_t index = 0; index < thread_count; ++index) {
            ::mach_port_deallocate(mach_task_self(), threads[index]);
        }
        ::vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), thread_count * sizeof *threads);
    }
    info.procs = static_cast<std::uint16_t>(std::clamp<mach_msg_type_number_t>(thread_count, 1, 0xffff));
    std::memcpy(sys::as_pointer<void>(address), &info, sizeof info);
    return 0;
}

}  // namespace apkrt::proc
