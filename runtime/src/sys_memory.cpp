// Memory syscalls of the Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): mmap, mprotect and
// madvise with Linux flags on Darwin's virtual memory. Executable memory is never handed out.
#include "syscalls.h"
#include "threads.h"

#include <sys/mman.h>

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
    const int host_fd = (flags & linux_map::anonymous) ? -1 : fd;

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

}  // namespace apkrt::sys
