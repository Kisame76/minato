// Memory syscalls of the Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): mmap, mprotect and
// madvise with Linux flags on Darwin's virtual memory. Executable memory is never handed out.
#include "descriptors.h"
#include "memfd.h"
#include "proc.h"
#include "syscalls.h"
#include "threads.h"
#include "vfs.h"

#include <fcntl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace apkrt::sys {
namespace {

/// Linux's mmap_lock, taken as a writer every time (ADR-0021): mmap, munmap, mremap, mprotect, madvise's DONTNEED and
/// FREE and a thread's deferred unmap change the VM map one at a time, so that mremap's look at the map and its change
/// are one step for the others. Not recursive: none of them calls another. No guest signal handler runs on a thread
/// that holds it, as guest signals wait for the syscall's exit (signals.h).
std::mutex memory_lock;

/// The shared mappings of memfds' writable open files, which Linux counts in i_mmap_writable, each with its memfd:
/// F_SEAL_WRITE waits for none to be left (memfd.h). Apart from one another; under memory_lock.
struct SharedMapping {
    std::uint64_t begin, end;
    std::weak_ptr<memfd::Inode> inode;
};
std::vector<SharedMapping> shared_mappings;

/// The shared mappings' parts in [begin, end) go, as proc::unmap's names do.
void forget_shared(std::uint64_t begin, std::uint64_t end) {
    std::vector<SharedMapping> kept;
    for (auto &mapping : shared_mappings) {
        if (mapping.inode.expired()) continue;
        if (mapping.end <= begin || mapping.begin >= end) {
            kept.push_back(std::move(mapping));
            continue;
        }
        if (mapping.begin < begin) kept.push_back({mapping.begin, begin, mapping.inode});
        if (mapping.end > end) kept.push_back({end, mapping.end, mapping.inode});
    }
    shared_mappings = std::move(kept);
}

/// mremap moved [from, from + old_length) to `to` with `new_length`: the shared mappings go with their pages, as
/// proc::remap's names do.
void move_shared(std::uint64_t from, std::uint64_t old_length, std::uint64_t to, std::uint64_t new_length) {
    const std::uint64_t kept = std::min(old_length, new_length);
    std::vector<SharedMapping> moved;
    for (const auto &mapping : shared_mappings) {
        const auto begin = std::max(mapping.begin, from), end = std::min(mapping.end, from + kept);
        if (begin < end) moved.push_back({to + (begin - from), to + (end - from), mapping.inode});
    }
    forget_shared(from, from + old_length);
    forget_shared(to, to + new_length);
    shared_mappings.insert(shared_mappings.end(), moved.begin(), moved.end());
}

/// A file mapping the guest asked for at an offset aligned to 4 KiB but not to the host's 16 KiB page, as a guest that
/// hard-codes 4 KiB alignment maps an uncompressed APK/OBB entry (ADR-0007 change, 2026-11-07). Darwin's mmap refuses
/// such an offset with EINVAL, so engines fall back to reading the whole file into anonymous memory -- a huge blow-up.
/// The runtime maps the 16 KiB-aligned region [host_begin, host_end) at the aligned offset and hands the guest
/// host_begin + delta; this table keeps the real host range so munmap, mprotect, msync, madvise and /proc/self/maps act
/// on whole host pages. Apart from one another; under memory_lock. (Deliberately more permissive than a 16 KiB Android
/// device, which also refuses it, because the apps run on 4 KiB Android.)
struct OffsetMapping {
    std::uint64_t host_begin;  // page-aligned real start of the host mapping
    std::uint64_t host_end;    // page-aligned real end
    std::uint64_t delta;       // guest address = host_begin + delta (0 < delta < page)
};
std::vector<OffsetMapping> offset_mappings;

/// Drops the emulated mappings' parts in [begin, end), as proc::unmap's names go: a head keeps its delta,
/// a tail starts at the page boundary `end` (delta 0). Under memory_lock.
void forget_offset(std::uint64_t begin, std::uint64_t end) {
    std::vector<OffsetMapping> kept;
    for (const auto &mapping : offset_mappings) {
        if (mapping.host_end <= begin || mapping.host_begin >= end) {
            kept.push_back(mapping);
            continue;
        }
        if (mapping.host_begin < begin) kept.push_back({mapping.host_begin, begin, mapping.delta});
        if (mapping.host_end > end) kept.push_back({end, mapping.host_end, 0});
    }
    offset_mappings = std::move(kept);
}

/// The real host range a guest VM op should act on: if [address, address + length) begins inside an emulated file
/// mapping, whole host pages clamped to that mapping's host bounds (so the op covers the delta-front page and never
/// spills into a neighbour); otherwise the range unchanged. Under memory_lock.
struct HostRange {
    std::uint64_t address;
    std::uint64_t length;
};
HostRange to_host_range(std::uint64_t address, std::uint64_t length) {
    const std::uint64_t page = vm_page_size;
    for (const auto &mapping : offset_mappings) {
        if (address >= mapping.host_begin + mapping.delta && address < mapping.host_end) {
            const std::uint64_t begin = address & ~(page - 1);  // the delta is below the page, so begin >= host_begin
            std::uint64_t end;
            if (length == 0) {
                end = begin;
            } else if (length > ~std::uint64_t{0} - address) {
                end = mapping.host_end;  // the length overflows: clamp to the mapping
            } else {
                end = (address + length + page - 1) & ~(page - 1);
            }
            if (end > mapping.host_end) end = mapping.host_end;
            if (end < begin) end = begin;
            return {begin, end - begin};
        }
    }
    return {address, length};
}

/// Writes `line` to the session log once per process (the first caller wins), only when MINATO_LOADER_LOG is set, as
/// the loader's scudo line and session.cpp's trim-footprint line are: a real session (RuntimeSession.swift) sets it, so
/// the line reaches the session log, while the byte-compared fixtures (Mac ctest and the AVD) leave it unset and
/// stay unchanged. In normal context (under memory_lock), so a plain write() is enough; the line is in Minato's liblog
/// form, so the exit path treats it as the runtime's own.
void log_once(std::atomic_flag &written, const char *line, std::size_t length) {
    if (::getenv("MINATO_LOADER_LOG") == nullptr) return;
    if (!written.test_and_set(std::memory_order_relaxed)) static_cast<void>(::write(STDERR_FILENO, line, length));
}
std::atomic_flag offset_emulated_logged = ATOMIC_FLAG_INIT;
std::atomic_flag fixed_offset_refused_logged = ATOMIC_FLAG_INIT;

// Linux mmap flags (include/uapi/asm-generic/mman*.h) and protections.
namespace linux_map {
constexpr int private_ = 0x02;
constexpr int type_mask = 0x03;  // MAP_SHARED 1, MAP_PRIVATE 2, MAP_SHARED_VALIDATE 3
constexpr int fixed = 0x10;
constexpr int anonymous = 0x20;
constexpr int growsdown = 0x100;
constexpr int denywrite = 0x800;
constexpr int executable = 0x1000;
constexpr int noreserve = 0x4000;
constexpr int populate = 0x8000;
constexpr int nonblock = 0x10000;
constexpr int stack = 0x20000;
constexpr int fixed_noreplace = 0x10'0000;
// Hints only: they change nothing a program can observe, or Linux ignores them itself.
constexpr int hints = growsdown | denywrite | executable | populate | nonblock | stack;
}  // namespace linux_map

constexpr int linux_prot_exec = 0x4;
constexpr int linux_prot_known = 0x1 | 0x2 | linux_prot_exec;

/// Protections: the same bits on both systems. Executable memory never (ADR-0002): only signed code
/// runs, and the guest's own mappings are not signed. Linux answers EACCES when its policy (SELinux
/// execmem) refuses executable memory, so guests handle that answer.
std::optional<long> check_protection(int protection) {
    if (protection & ~linux_prot_known) return -linux_errno::einval;  // PROT_BTI, PROT_MTE: not offered
    if (protection & linux_prot_exec) return -linux_errno::eacces;
    return std::nullopt;
}

}  // namespace

long sys_mmap(std::uint64_t address, std::uint64_t length, int protection, int flags, int fd, std::uint64_t offset) {
    if (auto error = check_protection(protection)) return *error;
    const int type = flags & linux_map::type_mask;
    const int known = linux_map::type_mask | linux_map::fixed | linux_map::anonymous | linux_map::noreserve |
                      linux_map::fixed_noreplace | linux_map::hints;
    if (type == 0 || (flags & ~known) != 0) return -linux_errno::einval;

    int host_flags = (type == linux_map::private_) ? MAP_PRIVATE : MAP_SHARED;
    if (flags & linux_map::fixed) host_flags |= MAP_FIXED;
    if (flags & linux_map::anonymous) host_flags |= MAP_ANON;
    if (flags & linux_map::noreserve) host_flags |= MAP_NORESERVE;
    // A file mapping maps the host descriptor behind the guest's; the mapping keeps the file itself.
    descriptors::Handle description;
    if (!(flags & linux_map::anonymous)) {
        description = descriptors::get(fd);
        if (!description) return -linux_errno::ebadf;
        // Neither has an mmap on Linux: pipes, sockets and terminals, which can block, eventfds and epolls.
        if (description->kind() != descriptors::Description::Kind::file || description->can_block()) {
            return -linux_errno::enodev;
        }
    }
    const int host_fd = description ? description->host() : -1;
    // A shared mapping of a memfd's writable open file, which Linux 6.6 counts and checks: mmap_region refuses it after
    // F_SEAL_WRITE (mapping_map_writable), shmem_mmap a writable one after F_SEAL_FUTURE_WRITE, and keeps a read-only
    // one from becoming writable (VM_MAYWRITE, so that mprotect answers EACCES).
    memfd::File *const memfd_file = description ? memfd::as_file(*description) : nullptr;
    const bool counted = memfd_file != nullptr && type != linux_map::private_ && memfd_file->writable;

    const std::lock_guard lock(memory_lock);
    bool read_only_at_most = false;
    if (counted) {
        const std::lock_guard seals(memfd_file->inode->mutex);
        if (memfd_file->inode->writes_denied) return -linux_errno::eperm;
        if (memfd_file->inode->seals & memfd::seal_future_write) {
            if (protection & PROT_WRITE) return -linux_errno::eperm;
            read_only_at_most = true;
        }
    }
    // A file offset aligned to 4 KiB but not to the host's 16 KiB page: Darwin refuses it. A fixed address cannot be
    // shifted, so keep Linux's EINVAL there (logged once); otherwise map the 16 KiB-aligned region and hand back the
    // address plus the delta (ADR-0007 change; recorded in offset_mappings below).
    const std::uint64_t page = vm_page_size;
    std::uint64_t delta = 0;
    if (!(flags & linux_map::anonymous) && (offset & (page - 1)) != 0) {
        if (flags & (linux_map::fixed | linux_map::fixed_noreplace)) {
            static const char line[] = "E/apkrt: fixed file mapping at a 4K offset refused\n";
            log_once(fixed_offset_refused_logged, line, sizeof line - 1);
            return -linux_errno::einval;
        }
        delta = offset & (page - 1);
        if (length > ~std::uint64_t{0} - delta) return -linux_errno::enomem;
        offset -= delta;
        length += delta;
    }
    void *const wanted = reinterpret_cast<void *>(static_cast<std::uintptr_t>(address));
    void *const result = ::mmap(wanted, length, protection, host_flags, host_fd, static_cast<off_t>(offset));
    if (result == MAP_FAILED) return -to_linux_errno(errno);
    if (read_only_at_most) {
        ::vm_protect(::mach_task_self(), reinterpret_cast<vm_address_t>(result), static_cast<vm_size_t>(length), TRUE,
                     VM_PROT_READ);
    }
    // MAP_FIXED_NOREPLACE: at exactly this address or not at all. Darwin takes the address as a hint
    // and places the mapping elsewhere when something is there already.
    if ((flags & linux_map::fixed_noreplace) && !(flags & linux_map::fixed) && result != wanted) {
        ::munmap(result, length);
        return -linux_errno::eexist;
    }
    // /proc/self/maps: what lay there loses its name, and a file's mapping takes the file's (proc.h).
    const auto begin = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(result));
    const auto end = begin + ((length + vm_page_size - 1) & ~std::uint64_t{vm_page_size - 1});
    proc::unmap(begin, end);
    forget_shared(begin, end);
    forget_offset(begin, end);
    if (description) proc::map_file(begin, end, fd, offset, type != linux_map::private_);
    if (counted) shared_mappings.push_back({begin, end, memfd_file->inode});
    if (delta != 0) {
        offset_mappings.push_back({begin, end, delta});
        static const char line[] = "I/apkrt: file mapping at a 4K offset emulated\n";
        log_once(offset_emulated_logged, line, sizeof line - 1);
        return static_cast<long>(begin + delta);
    }
    return static_cast<long>(begin);
}

long sys_munmap(const apkrt_syscall_frame &frame, std::uint64_t address, std::uint64_t length) {
    const std::lock_guard lock(memory_lock);
    // bionic's detached threads unmap their own stack and exit right after (_exit_with_stack_teardown).
    // The syscall layer runs on that stack, from here up to the guest's stack pointer at the svc.
    const auto low = reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
    const auto high = reinterpret_cast<std::uint64_t>(&frame + 1) + 16;
    if (auto *thread = threads::current(); thread != nullptr && address < high && address + length > low) {
        thread->deferred_unmap_start = address;
        thread->deferred_unmap_length = length;
        return 0;
    }
    // An emulated 4 KiB-offset file mapping's guest address is not host-page-aligned: act on the real host range.
    const HostRange host = to_host_range(address, length);
    const long result = result_or_error(::munmap(as_pointer<void>(host.address), host.length));
    if (result == 0) {
        const auto end = host.address + ((host.length + vm_page_size - 1) & ~std::uint64_t{vm_page_size - 1});
        proc::unmap(host.address, end);
        forget_shared(host.address, end);
        forget_offset(host.address, end);
    }
    return result;
}

namespace memory {

void (*test_pause)() = nullptr;

void unmap_deferred(std::uint64_t start, std::uint64_t length) {
    const std::lock_guard lock(memory_lock);
    ::munmap(as_pointer<void>(start), length);
    proc::unmap(start, start + length);
    forget_shared(start, start + length);
}

long with_shared_mappings_of(const memfd::Inode &inode, const std::function<long(bool mapped)> &call) {
    const std::lock_guard lock(memory_lock);
    const bool mapped = std::any_of(shared_mappings.begin(), shared_mappings.end(),
                                    [&](const SharedMapping &mapping) { return mapping.inode.lock().get() == &inode; });
    return call(mapped);
}

}  // namespace memory

long sys_mprotect(std::uint64_t address, std::uint64_t length, int protection) {
    if (auto error = check_protection(protection)) return *error;
    const std::lock_guard lock(memory_lock);
    const HostRange host = to_host_range(address, length);  // an emulated 4 KiB-offset mapping: the real host range
    return result_or_error(::mprotect(as_pointer<void>(host.address), host.length, protection));
}

namespace {

// mremap flags (include/uapi/linux/mman.h). MREMAP_DONTUNMAP (4, Linux 5.7) is not offered: kernels before it
// refuse it as an unknown flag.
namespace linux_mremap {
constexpr int maymove = 1;
constexpr int fixed = 2;
}  // namespace linux_mremap

/// What Darwin's VM map holds in a range, as Linux's VMAs would show it.
struct Span {
    bool mapped = true;       // every page is mapped
    bool one = true;          // with one protection, kind and tag, which Linux would merge into one VMA
    bool executable = false;  // some page may execute
    bool file = false;        // some page maps a file (an external pager)
    vm_prot_t protection = VM_PROT_NONE;
};

/// The VM map's entries in [begin, end), down to the entries inside submaps (the shared region), each with its
/// own protection. Holes make the range unmapped, but every entry in it still counts for `executable`. The user tag
/// keeps the guest's mappings (0, sys_mmap's) apart from neighbouring host memory (malloc's and the like), which is
/// never part of the same mapping.
Span inspect(std::uint64_t begin, std::uint64_t end) {
    Span span;
    bool first = true;
    unsigned int tag = 0;
    std::uint64_t address = begin;
    while (address < end) {
        vm_address_t region = static_cast<vm_address_t>(address);
        vm_size_t size = 0;
        natural_t depth = 0;
        // The short form holds all this needs (protection, pager, tag), at a hundredth of the full form's cost.
        vm_region_submap_short_info_data_64_t info{};
        bool found = false;
        while (true) {
            vm_address_t query = std::max(region, static_cast<vm_address_t>(address));
            mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
            if (::vm_region_recurse_64(::mach_task_self(), &query, &size, &depth,
                                       reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS) {
                found = false;  // inside a submap too: what lies there is unknown, so it counts as unmapped
                break;
            }
            region = query;
            found = true;
            if (!info.is_submap || depth >= 8) break;
            ++depth;
        }
        if (!found || region >= end) {
            span.mapped = false;  // nothing more is mapped in the range
            break;
        }
        if (region > address) span.mapped = false;  // a hole before this entry
        const bool file = info.external_pager != 0;
        span.executable = span.executable || (info.protection & VM_PROT_EXECUTE) != 0;
        if (first) {
            span.protection = info.protection;
            span.file = file;
            tag = info.user_tag;
            first = false;
        } else if (info.protection != span.protection || file != span.file || info.user_tag != tag) {
            span.one = false;
            span.file = span.file || file;
        }
        if (region + size <= region) break;  // the end of the address space
        address = region + size;
    }
    return span;
}

/// `length` rounded up to the page, as Linux's PAGE_ALIGN; 0 when that overflows, as there.
std::uint64_t page_align(std::uint64_t length, std::uint64_t page) {
    return length > ~(page - 1) ? 0 : (length + page - 1) & ~(page - 1);
}

bool overlaps(std::uint64_t begin, std::uint64_t end, std::uint64_t other_begin, std::uint64_t other_end) {
    return begin < other_end && other_begin < end;
}

/// Moves the pages of [address, address + old_length) onto `target` with vm_remap without a copy, which replaces
/// what lies there in one step and carries the pages themselves: their contents and protection go along, nothing
/// reads them, and a failed remap changes nothing. Then the rest of [target, target + new_length) is zero pages with
/// `protection`, mapped here unless `rest_mapped`, and the old range goes.
long move_pages(std::uint64_t address, std::uint64_t old_length, std::uint64_t target, std::uint64_t new_length,
                vm_prot_t protection, bool rest_mapped) {
    vm_address_t remapped = static_cast<vm_address_t>(target);
    vm_prot_t current = VM_PROT_NONE, maximum = VM_PROT_NONE;
    // Carry the source's inheritance to the moved region (vm_remap defaults to VM_INHERIT_COPY): a MAP_SHARED mapping
    // stays VM_INHERIT_SHARE so dont_need still keeps it, a private one stays VM_INHERIT_COPY. vm_remap leaves the
    // moved region SM_TRUESHARED regardless, which is why dont_need classifies by inheritance, not share_mode.
    vm_inherit_t inheritance = VM_INHERIT_COPY;
    {
        vm_address_t probe = static_cast<vm_address_t>(address);
        vm_size_t probe_size = 0;
        natural_t depth = 0;
        vm_region_submap_short_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
        if (::vm_region_recurse_64(::mach_task_self(), &probe, &probe_size, &depth,
                                   reinterpret_cast<vm_region_recurse_info_t>(&info), &count) == KERN_SUCCESS &&
            probe <= address) {
            inheritance = info.inheritance;
        }
    }
    if (::vm_remap(::mach_task_self(), &remapped, static_cast<vm_size_t>(old_length), 0,
                   VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, ::mach_task_self(), static_cast<vm_address_t>(address), FALSE,
                   &current, &maximum, inheritance) != KERN_SUCCESS) {
        return -linux_errno::enomem;
    }
    if (new_length > old_length) {
        const std::uint64_t rest = target + old_length, rest_length = new_length - old_length;
        if (!rest_mapped && ::mmap(as_pointer<void>(rest), rest_length, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
            // The old range stays the pages' only mapping, as before the call.
            const long error = -to_linux_errno(errno);
            ::munmap(as_pointer<void>(target), old_length);
            return error;
        }
        if (protection != (VM_PROT_READ | VM_PROT_WRITE)) {
            ::vm_protect(::mach_task_self(), static_cast<vm_address_t>(rest), static_cast<vm_size_t>(rest_length),
                         FALSE, protection);
        }
    }
    ::munmap(as_pointer<void>(address), old_length);
    return static_cast<long>(target);
}

}  // namespace

long sys_mremap(const apkrt_syscall_frame &frame, std::uint64_t address, std::uint64_t old_length,
                std::uint64_t new_length, int flags, std::uint64_t new_address) {
    // Linux's order of checks (mm/mremap.c): the flags, the address, the lengths, then the mapping.
    if (flags & ~(linux_mremap::maymove | linux_mremap::fixed)) return -linux_errno::einval;
    if ((flags & linux_mremap::fixed) && !(flags & linux_mremap::maymove)) return -linux_errno::einval;
    const std::uint64_t page = vm_page_size;
    if (address & (page - 1)) return -linux_errno::einval;
    old_length = page_align(old_length, page);
    new_length = page_align(new_length, page);
    if (new_length == 0) return -linux_errno::einval;
    const std::lock_guard lock(memory_lock);
    // An emulated 4 KiB-offset file mapping (offset_mappings) cannot be moved here: its guest address is not on a host
    // page, and its host range carries a delta the remap would lose. Refuse cleanly (EINVAL), as Linux does for a
    // misaligned address; a guest that must grow such a mapping maps anew. The start already fails the alignment check
    // above, but an interior 16 KiB-aligned address inside one would not.
    for (const auto &mapping : offset_mappings) {
        if (address >= mapping.host_begin + mapping.delta && address < mapping.host_end) return -linux_errno::einval;
    }
    if (address > ~std::uint64_t{0} - page || !inspect(address, address + page).mapped) {
        return -linux_errno::efault;  // vma_lookup finds no mapping
    }

    // Where Minato refuses, with Linux's answer for a sealed mapping: signed code stays where the loader put it,
    // and the runtime's frames on the calling thread's stack stay mapped: from the guest's stack pointer at the svc
    // down to this frame, as in sys_munmap, and the page below it for the calls made from here.
    const std::uint64_t old_end = address + std::max(old_length, page);
    if (old_end < address) return -linux_errno::efault;
    const auto low = (reinterpret_cast<std::uint64_t>(__builtin_frame_address(0)) & ~(page - 1)) - page;
    const auto high = reinterpret_cast<std::uint64_t>(&frame + 1) + 16;
    const Span old = inspect(address, old_end);
    if (old.executable || overlaps(address, old_end, low, high)) return -linux_errno::eperm;
    if (memory::test_pause != nullptr) memory::test_pause();

    if (flags & linux_mremap::fixed) {
        // mremap_to: the target is aligned, inside the address space and apart from the old range.
        if ((new_address & (page - 1)) || new_address + new_length < new_address) return -linux_errno::einval;
        if (overlaps(address, address + old_length, new_address, new_address + new_length)) {
            return -linux_errno::einval;
        }
        if (overlaps(new_address, new_address + new_length, low, high)) return -linux_errno::eperm;
        // vma_to_resize, checked before anything changes, where Linux has unmapped the target and the old range's
        // tail by then: a private mapping is not duplicated, and the range moves only within one mapping.
        if (old_length == 0) return -linux_errno::einval;
        const std::uint64_t moved = std::min(old_length, new_length);
        const Span source = inspect(address, address + moved);
        if (!source.mapped || !source.one) return -linux_errno::efault;
        if (new_length > old_length && source.file) return -linux_errno::enomem;
        // The pages move onto the target first, replacing what lies there as MAP_FIXED does, so that a failure
        // changes nothing; then the rest of the target, and the old range's longer tail goes.
        const long result = move_pages(address, moved, new_address, new_length, source.protection, false);
        if (result >= 0 && old_length > new_length) {
            ::munmap(as_pointer<void>(address + new_length), old_length - new_length);
        }
        if (result >= 0) {
            proc::remap(address, old_length, new_address, new_length);
            move_shared(address, old_length, new_address, new_length);
        }
        return result;
    }

    // Shrinking unmaps the tail, whatever lies there, as do_vmi_munmap does; equal lengths change nothing.
    if (old_length >= new_length) {
        if (old_length > new_length && ::munmap(as_pointer<void>(address + new_length), old_length - new_length) != 0) {
            return -to_linux_errno(errno);
        }
        proc::remap(address, old_length, address, new_length);
        move_shared(address, old_length, address, new_length);
        return static_cast<long>(address);
    }

    // Growing (vma_to_resize): a private mapping is not duplicated (an old length of 0), and the old range must be
    // one mapping. A file mapping does not grow here, where Linux would map more of the file.
    if (old_length == 0) return -linux_errno::einval;
    if (!old.mapped || !old.one) return -linux_errno::efault;
    if (old.file) return -linux_errno::enomem;

    // In place when the pages after it are free: vm_allocate at a fixed address never lands on a mapping.
    vm_address_t tail = static_cast<vm_address_t>(address + old_length);
    const auto added = static_cast<vm_size_t>(new_length - old_length);
    if (::vm_allocate(::mach_task_self(), &tail, added, VM_FLAGS_FIXED) == KERN_SUCCESS) {
        if (old.protection != (VM_PROT_READ | VM_PROT_WRITE)) {
            ::vm_protect(::mach_task_self(), tail, added, FALSE, old.protection);
        }
        return static_cast<long>(address);
    }
    if (!(flags & linux_mremap::maymove)) return -linux_errno::enomem;

    // Elsewhere: a new range where the host places sys_mmap's mappings, never on another mapping.
    void *const target = ::mmap(nullptr, new_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (target == MAP_FAILED) return -linux_errno::enomem;
    const long result =
        move_pages(address, old_length, reinterpret_cast<std::uint64_t>(target), new_length, old.protection, true);
    if (result < 0) ::munmap(target, new_length);
    if (result >= 0) {
        proc::remap(address, old_length, reinterpret_cast<std::uint64_t>(target), new_length);
        move_shared(address, old_length, reinterpret_cast<std::uint64_t>(target), new_length);
    }
    return result;
}

namespace {

// madvise advice (include/uapi/asm-generic/mman-common.h).
namespace linux_madv {
constexpr int normal = 0;
constexpr int willneed = 3;  // 0-3 are the same on Darwin
constexpr int dontneed = 4;
constexpr int free = 8;
constexpr int dontfork = 10;
constexpr int dofork = 11;
constexpr int mergeable = 12;
constexpr int unmergeable = 13;
constexpr int hugepage = 14;
constexpr int nohugepage = 15;
constexpr int dontdump = 16;
constexpr int dodump = 17;
constexpr int wipeonfork = 18;
constexpr int keeponfork = 19;
constexpr int cold = 20;
constexpr int pageout = 21;
constexpr int populate_read = 22;
constexpr int populate_write = 23;
}  // namespace linux_madv

/// Whether a shared mapping the runtime records (a memfd's or ashmem's MAP_SHARED pages) overlaps [begin, end). Under
/// memory_lock.
bool shared_mapping_overlaps(std::uint64_t begin, std::uint64_t end) {
    for (const auto &mapping : shared_mappings) {
        if (mapping.begin < end && mapping.end > begin) return true;
    }
    return false;
}

/// Zeroes the private-anonymous sub-host-page remainder [at, stop) in place with MADV_ZERO, as Linux zeroes a 4 KiB
/// sub-range of a 16 KiB page; the range is known private anonymous, so MADV_ZERO applies. Its footprint stays (below
/// one host page an end), unlike Linux, but the remainder is tiny. Under memory_lock.
long zero_anon(std::uint64_t at, std::uint64_t stop) {
    if (at >= stop) return 0;
    if (::madvise(as_pointer<void>(at), static_cast<std::size_t>(stop - at), MADV_ZERO) != 0) {
        return -to_linux_errno(errno);
    }
    return 0;
}

/// Darwin's MADV_DONTNEED over [at, stop), for the ranges dont_need does not replace -- a file or a shared mapping. It
/// keeps a shared mapping's data (shmem) and a private file's written bytes (never MADV_ZERO, which would zero shared
/// anonymous memory) and frees what clean pages it can. Under memory_lock.
long advise_keep(std::uint64_t at, std::uint64_t stop) {
    if (at >= stop) return 0;
    if (::madvise(as_pointer<void>(at), static_cast<std::size_t>(stop - at), MADV_DONTNEED) != 0) {
        return -to_linux_errno(errno);
    }
    return 0;
}

/// Replaces the whole host pages [at, stop) with a fresh MAP_FIXED | MAP_PRIVATE | MAP_ANON mapping at `protection`, so
/// their physical pages go and phys_footprint falls. Darwin's MAP_FIXED overwrites in place (VM_FLAGS_OVERWRITE), so no
/// window unmaps the range for another thread. The guest's own anonymous mappings carry VM tag 0, so a -1 descriptor
/// keeps the tag; executable is never set (ADR-0002). Under memory_lock.
bool replace_anon(std::uint64_t at, std::uint64_t stop, vm_prot_t protection) {
    const int prot = protection & (VM_PROT_READ | VM_PROT_WRITE);
    void *const got = ::mmap(as_pointer<void>(at), static_cast<std::size_t>(stop - at), prot,
                             MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    return got != MAP_FAILED;
}

/// Re-maps the whole host pages [begin, stop) of a written private file mapping to the file's bytes, as Linux's
/// MADV_DONTNEED does by dropping the private copies: reopen the file by the guest path proc recorded (file_mapping_at
/// + vfs::locate, read-only, with RESOLVE_BENEATH so no link or ".." leads out of the mount) and map it
/// MAP_FIXED | MAP_PRIVATE at the matching offset, so the next read shows the file and the private copies' footprint
/// falls. Any step that cannot be done -- no record, the record does not cover the range, an unaligned offset, locate
/// or open or mmap fails -- keeps Darwin's MADV_DONTNEED (advise_keep), which keeps the written bytes: safe, never a
/// crash and never a read of another file. [begin, stop) are whole host pages. Under memory_lock.
long remap_private_file(std::uint64_t begin, std::uint64_t stop, vm_prot_t protection) {
    const auto mapping = proc::file_mapping_at(begin);
    if (!mapping || mapping->begin > begin || mapping->end < stop) return advise_keep(begin, stop);
    const std::uint64_t file_offset = mapping->offset + (begin - mapping->begin);
    if ((file_offset & (vm_page_size - 1)) != 0) return advise_keep(begin, stop);
    const auto location = vfs::locate(mapping->path);
    if (!location) return advise_keep(begin, stop);
    const int flags = O_RDONLY | O_CLOEXEC | (location->folder == AT_FDCWD ? 0 : O_RESOLVE_BENEATH);
    const int fd = ::openat(location->folder, location->path.c_str(), flags);
    if (fd < 0) return advise_keep(begin, stop);
    // The path may name a different file now (renamed, replaced, unlinked+recreated since the mmap); Linux keeps
    // showing the mapped inode, so re-map only when the reopened file is still the recorded one, else keep what is
    // written -- never a foreign file's bytes. map_file split the device into Linux's major/minor the same way.
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        ::close(fd);
        return advise_keep(begin, stop);
    }
    const std::uint32_t major = (static_cast<std::uint32_t>(status.st_dev) >> 24) & 0xff;
    const std::uint32_t minor = static_cast<std::uint32_t>(status.st_dev) & 0xff'ffff;
    const bool same = mapping->major == major && mapping->minor == minor &&
                      mapping->inode == static_cast<std::uint64_t>(status.st_ino);
    if (!same) {
        ::close(fd);
        return advise_keep(begin, stop);
    }
    const int prot = protection & (VM_PROT_READ | VM_PROT_WRITE);
    void *const got = ::mmap(as_pointer<void>(begin), static_cast<std::size_t>(stop - begin), prot,
                             MAP_FIXED | MAP_PRIVATE, fd, static_cast<off_t>(file_offset));
    ::close(fd);
    return got == MAP_FAILED ? advise_keep(begin, stop) : 0;
}

/// MADV_DONTNEED over [begin, end): on Linux the pages are freed -- private anonymous read back as zero (allocators
/// Scudo relies on it), a private file's copies drop, and resident size and footprint fall. On Darwin neither
/// MADV_ZERO nor MADV_DONTNEED frees the physical pages: they stay in phys_footprint (the iOS jetsam metric), so an
/// allocator's releaseToOS or a GC would never shrink the process (ADR-0007 change, 2026-11-07). So for every whole
/// host page that is private and anonymous, replace it with a fresh anonymous mapping: the pages go and the next touch
/// reads zero, as on Linux. A sub-host-page end of such a range (a guest that keeps 4 KiB pages: Linux zeroes a 4 KiB
/// sub-range of a 16 KiB page) is zeroed in place. A written private FILE mapping re-maps the file over its whole pages
/// (remap_private_file), so the next read shows the file's bytes, as Linux's dropping of the private copies does; its
/// sub-page ends and a shared mapping keep Darwin's MADV_DONTNEED, which keeps their data. Classification uses the
/// inheritance and the runtime's own records, NOT share_mode: XNU leaves a private
/// mapping moved by mremap (vm_remap) as SM_TRUESHARED, so share_mode would wrongly keep it. A mapping counts as shared
/// on VM_INHERIT_SHARE (which MAP_SHARED sets and move_pages preserves across a move) or the runtime's own shared
/// record (a memfd's or ashmem's pages); file-backed is the runtime's own map_file record, not the host's pager,
/// because a written MAP_PRIVATE file gets an anonymous shadow that hides the pager. ENOMEM at a hole.
long dont_need(std::uint64_t begin, std::uint64_t end) {
    const std::uint64_t page = vm_page_size;
    for (std::uint64_t at = begin; at < end;) {
        vm_address_t query = static_cast<vm_address_t>(at);
        vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_short_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
        if (::vm_region_recurse_64(::mach_task_self(), &query, &size, &depth,
                                   reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS ||
            query > at) {
            return -linux_errno::enomem;  // a hole, as Linux answers for a part that is not mapped
        }
        const std::uint64_t region_end = std::min<std::uint64_t>(end, query + size);
        const bool shared = info.inheritance == VM_INHERIT_SHARE || shared_mapping_overlaps(at, region_end);
        const bool file = proc::range_is_file_backed(at, region_end);
        const bool guest_private = !info.is_submap && info.user_tag == 0 && !shared;
        const bool replaceable = guest_private && !file;         // a private anonymous mapping
        // Not executable: the remap masks to R|W and would drop EXECUTE, so an exec region keeps advise_keep instead.
        const bool remappable_file = guest_private && file && (info.protection & VM_PROT_EXECUTE) == 0;
        const std::uint64_t whole_begin = (at + page - 1) & ~(page - 1);
        const std::uint64_t whole_end = region_end & ~(page - 1);
        if (replaceable && whole_end > whole_begin) {
            if (!replace_anon(whole_begin, whole_end, info.protection)) return -to_linux_errno(errno);
            if (const long error = zero_anon(at, whole_begin); error != 0) return error;        // sub-page head
            if (const long error = zero_anon(whole_end, region_end); error != 0) return error;  // sub-page tail
        } else if (replaceable) {  // a private anonymous region below one whole host page: zero it in place
            if (const long error = zero_anon(at, region_end); error != 0) return error;
        } else if (remappable_file && whole_end > whole_begin) {  // re-map whole pages to the file, keep any sub-page
            if (const long error = remap_private_file(whole_begin, whole_end, info.protection)) return error;
            if (const long error = advise_keep(at, whole_begin); error != 0) return error;        // sub-page head
            if (const long error = advise_keep(whole_end, region_end); error != 0) return error;  // sub-page tail
        } else if (const long error = advise_keep(at, region_end); error != 0) {  // shared, submap, or sub-page file
            return error;
        }
        at = region_end;
    }
    return 0;
}

}  // namespace

long sys_msync(std::uint64_t address, std::uint64_t length, int flags) {
    // include/uapi/asm-generic/mman-common.h; Darwin's MS_SYNC is 0x10, its MS_ASYNC and MS_INVALIDATE are Linux's.
    constexpr int linux_async = 1, linux_invalidate = 2, linux_sync = 4;
    if ((flags & ~(linux_async | linux_invalidate | linux_sync)) != 0 ||
        ((flags & linux_async) != 0 && (flags & linux_sync) != 0)) {
        return -linux_errno::einval;
    }
    int host_flags = flags & (linux_async | linux_invalidate);
    if ((flags & linux_sync) != 0) host_flags |= MS_SYNC;
    if ((flags & (linux_async | linux_sync)) == 0) host_flags |= MS_ASYNC;  // Linux accepts neither; Darwin wants one
    if (length == 0) return 0;
    const std::lock_guard lock(memory_lock);
    // An emulated 4 KiB-offset file mapping: act on the real host range, whose start is on a host page. Only a mapping
    // we did not shift must meet Linux's page-alignment rule.
    const HostRange host = to_host_range(address, length);
    if (host.address == address && (address & (static_cast<std::uint64_t>(::getpagesize()) - 1)) != 0) {
        return -linux_errno::einval;
    }
    // Linux answers ENOMEM for a range with a hole, also before it syncs the part that is mapped.
    if (!inspect(host.address, host.address + host.length).mapped) return -linux_errno::enomem;
    return result_or_error(::msync(as_pointer<void>(host.address), host.length, host_flags));
}

long sys_madvise(std::uint64_t address, std::uint64_t length, int advice) {
    // An emulated 4 KiB-offset file mapping: advise the real host range (its guest address is not on a host page). The
    // scoped lock is released before the advice cases take memory_lock of their own.
    {
        const std::lock_guard lock(memory_lock);
        const HostRange host = to_host_range(address, length);
        address = host.address;
        length = host.length;
    }
    void *const start = as_pointer<void>(address);
    switch (advice) {
        case linux_madv::normal:
        case 1:  // MADV_RANDOM
        case 2:  // MADV_SEQUENTIAL
        case linux_madv::willneed:
            return result_or_error(::madvise(start, length, advice));

        case linux_madv::dontneed: {
            // Linux frees the range: private anonymous pages read back as zero (allocators like Scudo rely on it), a
            // private file's copies drop, and the footprint falls. Darwin's MADV_ZERO and MADV_DONTNEED both keep the
            // pages in phys_footprint, so dont_need replaces private anonymous host pages with fresh ones instead.
            const std::lock_guard lock(memory_lock);
            return dont_need(address, address + length);
        }

        case linux_madv::free: {
            // Contents undefined until written again, on both systems.
            const std::lock_guard lock(memory_lock);
            return result_or_error(::madvise(start, length, MADV_FREE));
        }

        case linux_madv::dontfork:
        case linux_madv::dofork:
        case linux_madv::wipeonfork:
        case linux_madv::keeponfork:
            // Only matter for fork, and iOS has no fork (F11).
        case linux_madv::mergeable:
        case linux_madv::unmergeable:
        case linux_madv::hugepage:
        case linux_madv::nohugepage:
        case linux_madv::dontdump:
        case linux_madv::dodump:
        case linux_madv::cold:
        case linux_madv::pageout:
        case linux_madv::populate_read:
        case linux_madv::populate_write:
            // Advice about sharing, huge pages, core dumps and paging: nothing Darwin offers or a
            // program can observe beyond timing.
            return 0;

        default:
            return -linux_errno::einval;  // MADV_REMOVE, MADV_HWPOISON and the like
    }
}

namespace memory {

bool copy_checked(std::uint64_t target, std::uint64_t source, std::uint64_t length) {
    if (length == 0) return true;
    vm_size_t copied = 0;
    return ::vm_read_overwrite(::mach_task_self(), static_cast<vm_address_t>(source), static_cast<vm_size_t>(length),
                               static_cast<vm_address_t>(target), &copied) == KERN_SUCCESS && copied == length;
}

}  // namespace memory

long sys_process_vm_readv(int pid, std::uint64_t local_address, std::uint64_t local_count,
                          std::uint64_t remote_address, std::uint64_t remote_count, std::uint64_t flags) {
    constexpr std::uint64_t iov_max = 1024;  // UIO_MAXIOV
    if (flags != 0 || local_count > iov_max || remote_count > iov_max) return -linux_errno::einval;
    if (pid != ::getpid()) return -linux_errno::esrch;
    // The vectors are read one element at a time: no allocation, little stack, as in a crash handler's unwinder.
    struct Iovec {
        std::uint64_t base = 0, length = 0;
    } local, remote;
    const auto read_element = [](Iovec &element, std::uint64_t vector, std::uint64_t index) {
        return memory::copy_checked(reinterpret_cast<std::uint64_t>(&element), vector + index * sizeof element,
                                    sizeof element);
    };
    std::uint64_t local_index = 0, used = 0, total = 0;
    const auto failed = [&] { return total > 0 ? static_cast<long>(total) : -linux_errno::efault; };
    for (std::uint64_t remote_index = 0; remote_index < remote_count; ++remote_index) {
        if (!read_element(remote, remote_address, remote_index)) return failed();
        std::uint64_t done = 0;
        while (done < remote.length) {
            if (used == local.length) {
                if (local_index == local_count) return static_cast<long>(total + done);  // no room left
                if (!read_element(local, local_address, local_index++)) return failed();
                used = 0;
                continue;
            }
            const std::uint64_t chunk = std::min(remote.length - done, local.length - used);
            if (!memory::copy_checked(local.base + used, remote.base + done, chunk)) return failed();  // never split
            done += chunk;
            used += chunk;
        }
        total += done;
    }
    return static_cast<long>(total);
}

}  // namespace apkrt::sys
