// Memory syscalls of the Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): mmap, mprotect and
// madvise with Linux flags on Darwin's virtual memory. Executable memory is never handed out.
#include "descriptors.h"
#include "syscalls.h"
#include "threads.h"

#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>

namespace apkrt::sys {
namespace {

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

    void *const wanted = reinterpret_cast<void *>(static_cast<std::uintptr_t>(address));
    void *const result = ::mmap(wanted, length, protection, host_flags, host_fd, static_cast<off_t>(offset));
    if (result == MAP_FAILED) return -to_linux_errno(errno);
    // MAP_FIXED_NOREPLACE: at exactly this address or not at all. Darwin takes the address as a hint
    // and places the mapping elsewhere when something is there already.
    if ((flags & linux_map::fixed_noreplace) && !(flags & linux_map::fixed) && result != wanted) {
        ::munmap(result, length);
        return -linux_errno::eexist;
    }
    return static_cast<long>(reinterpret_cast<std::uintptr_t>(result));
}

long sys_munmap(const apkrt_syscall_frame &frame, std::uint64_t address, std::uint64_t length) {
    // bionic's detached threads unmap their own stack and exit right after (_exit_with_stack_teardown).
    // The syscall layer runs on that stack, from here up to the guest's stack pointer at the svc.
    const auto low = reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
    const auto high = reinterpret_cast<std::uint64_t>(&frame + 1) + 16;
    if (auto *thread = threads::current(); thread != nullptr && address < high && address + length > low) {
        thread->deferred_unmap_start = address;
        thread->deferred_unmap_length = length;
        return 0;
    }
    return result_or_error(::munmap(as_pointer<void>(address), length));
}

long sys_mprotect(std::uint64_t address, std::uint64_t length, int protection) {
    if (auto error = check_protection(protection)) return *error;
    return result_or_error(::mprotect(reinterpret_cast<void *>(static_cast<std::uintptr_t>(address)), length, protection));
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
    if (::vm_remap(::mach_task_self(), &remapped, static_cast<vm_size_t>(old_length), 0,
                   VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, ::mach_task_self(), static_cast<vm_address_t>(address), FALSE,
                   &current, &maximum, VM_INHERIT_DEFAULT) != KERN_SUCCESS) {
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
        return result;
    }

    // Shrinking unmaps the tail, whatever lies there, as do_vmi_munmap does; equal lengths change nothing.
    if (old_length >= new_length) {
        if (old_length > new_length && ::munmap(as_pointer<void>(address + new_length), old_length - new_length) != 0) {
            return -to_linux_errno(errno);
        }
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

}  // namespace

long sys_madvise(std::uint64_t address, std::uint64_t length, int advice) {
    void *const start = as_pointer<void>(address);
    switch (advice) {
        case linux_madv::normal:
        case 1:  // MADV_RANDOM
        case 2:  // MADV_SEQUENTIAL
        case linux_madv::willneed:
            return result_or_error(::madvise(start, length, advice));

        case linux_madv::dontneed:
            // Linux: private anonymous pages read back as zero afterwards, and allocators (Scudo)
            // rely on that. Darwin's MADV_DONTNEED keeps the contents; MADV_ZERO clears them.
            return result_or_error(::madvise(start, length, MADV_ZERO));

        case linux_madv::free:
            // Contents undefined until written again, on both systems.
            return result_or_error(::madvise(start, length, MADV_FREE));

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

namespace {

/// Copies `length` bytes of the process's memory from `source` to `target` as the kernel copies between user
/// addresses: false instead of a fault when either cannot be read or written.
bool copy_checked(std::uint64_t target, std::uint64_t source, std::uint64_t length) {
    if (length == 0) return true;
    vm_size_t copied = 0;
    return ::vm_read_overwrite(::mach_task_self(), static_cast<vm_address_t>(source), static_cast<vm_size_t>(length),
                               static_cast<vm_address_t>(target), &copied) == KERN_SUCCESS && copied == length;
}

}  // namespace

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
        return copy_checked(reinterpret_cast<std::uint64_t>(&element), vector + index * sizeof element, sizeof element);
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
            if (!copy_checked(local.base + used, remote.base + done, chunk)) return failed();  // never split
            done += chunk;
            used += chunk;
        }
        total += done;
    }
    return static_cast<long>(total);
}

}  // namespace apkrt::sys
