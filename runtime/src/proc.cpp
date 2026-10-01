// The files of /proc and /sys that the runtime writes (proc.h, ADR-0033 decision 8). Each open writes
// what Linux would show at that moment into an unlinked temporary file of the host and gives the guest
// that file, so that reads, seeks and fstat work as on any file. The formats are Linux 6.6's, the kernel
// of Android 15's emulator: fs/proc/array.c (stat), fs/proc/task_mmu.c (maps), drivers/base/cpu.c
// (possible, online), fs/proc/fd.c and the anonymous inodes (the links of fd/).
#include "proc.h"

#include "descriptors.h"
#include "dl.h"
#include "syscalls.h"
#include "threads.h"
#include "vfs.h"

#include <fcntl.h>
#include <mach/mach.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace apkrt::proc {
namespace {

struct State {
    std::mutex mutex;
    std::uint64_t stack_low = 0, stack_high = 0, stack_start = 0;
    std::uint64_t arguments_begin = 0, arguments_end = 0, environment_begin = 0, environment_end = 0;
    std::uint64_t start_ticks = 0;  // CLOCK_BOOTTIME at the start, in clock ticks
    bool has_main_thread = false;
    pthread_t main_thread{};
    std::string program;
};

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

// --- /sys/devices/system/cpu -------------------------------------------------------------------------

/// A CPU mask's list, as Linux writes it: "0-5" for six CPUs, "0" for one.
std::string cpu_list(const char *sysctl_name) {
    int cpus = 1;
    std::size_t length = sizeof cpus;
    if (::sysctlbyname(sysctl_name, &cpus, &length, nullptr, 0) != 0 || cpus < 1) cpus = 1;
    return cpus == 1 ? std::string("0\n") : std::format("0-{}\n", cpus - 1);
}

// --- /proc/self/stat -----------------------------------------------------------------------------------

/// Elf64_Phdr.
struct ProgramHeader {
    std::uint32_t type, flags;
    std::uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
constexpr std::uint32_t pt_load = 1;
constexpr std::uint32_t pf_x = 1;

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

// --- /proc/self/maps -----------------------------------------------------------------------------------

/// A range of the host's address space with its protection, as the host's VM map has it.
struct Region {
    std::uint64_t begin, end;
    int protection;
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
        if (address != 0) regions.push_back({address, address + size, info.protection});
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
};

std::vector<Label> guest_labels() {
    std::vector<Label> labels;
    {
        auto &current = state();
        const std::lock_guard lock(current.mutex);
        if (current.stack_high > current.stack_low) {
            labels.push_back({.begin = current.stack_low, .end = current.stack_high, .name = "[stack]"});
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
        for (std::size_t index = 0; index < object.program_header_count; ++index) {
            const auto &header = headers[index];
            if (header.type != pt_load) continue;
            const std::uint64_t start = object.bias + header.vaddr;
            const std::uint64_t begin = page_down(start);
            const std::uint64_t file_end = header.filesz != 0 ? page_up(start + header.filesz) : begin;
            const std::uint64_t memory_end = page_up(start + header.memsz);
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
    text += std::format("{:08x}-{:08x} {}{}{}p {:08x} {:02x}:{:02x} {} ", begin, end,
                        (protection & VM_PROT_READ) ? 'r' : '-', (protection & VM_PROT_WRITE) ? 'w' : '-',
                        (protection & VM_PROT_EXECUTE) ? 'x' : '-', label != nullptr && label->file ? offset : 0,
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

/// /proc/self/maps: the host's mappings, each cut where the guest knows a name for a part of it.
/// Whatever only the host maps stays anonymous, so that no host path reaches the guest. Neighbours with
/// the same protection and name, a file's at following offsets, are one line, as Linux merges them.
std::string maps() {
    const auto labels = guest_labels();
    struct Piece {
        std::uint64_t begin, end;
        int protection;
        const Label *label;
        std::uint64_t offset;
    };
    std::vector<Piece> pieces;
    const auto add = [&](const Piece &piece) {
        if (!pieces.empty()) {
            auto &last = pieces.back();
            const bool same_name = last.label == piece.label || (last.label != nullptr && piece.label != nullptr &&
                                                                 last.label->name == piece.label->name);
            const bool following = last.label == nullptr || !last.label->file ||
                                   last.offset + (last.end - last.begin) == piece.offset;
            if (last.end == piece.begin && last.protection == piece.protection && same_name && following) {
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
                add({at, end, region.protection, &label, label.offset + (at - label.begin)});
                at = end;
            } else {
                const auto end =
                    next_label < labels.size() ? std::min(region.end, labels[next_label].begin) : region.end;
                add({at, end, region.protection, nullptr, 0});
                at = end;
            }
        }
    }
    std::string text;
    for (const auto &piece : pieces) {
        append_line(text, piece.begin, piece.end, piece.protection, piece.label, piece.offset);
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
    if (!description->written_path.empty()) return description->written_path;
    using Kind = descriptors::Description::Kind;
    if (description->kind() == Kind::eventfd) return "anon_inode:[eventfd]";
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

std::optional<long> open(std::string_view path, int linux_flags) {
    std::optional<std::string> contents;
    std::string name(path);
    if (path == "/sys/devices/system/cpu/possible") {
        contents = cpu_list("hw.ncpu");
    } else if (path == "/sys/devices/system/cpu/online") {
        contents = cpu_list("hw.activecpu");  // as sched_getaffinity
    } else if (const auto entry = own_entry(path)) {
        if (*entry == "stat") contents = stat_line();
        if (*entry == "maps") contents = maps();
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

std::optional<long> read_link(std::string_view path, char *buffer, std::size_t size) {
    const auto entry = own_entry(path);
    if (!entry || !entry->starts_with("fd/")) return std::nullopt;
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

}  // namespace apkrt::proc
