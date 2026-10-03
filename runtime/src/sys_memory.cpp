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
