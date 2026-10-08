// Checks the Linux semantics of the syscall layer (ADR-0009): each call goes through the dispatcher
// with Linux numbers, flags and structures, exactly as a guest's svc veneer delivers them. Descriptors
// are guest numbers (ADR-0027); host_of() shows the host descriptor behind one.
#include "descriptors.h"
#include "hwcap.h"
#include "proc.h"
#include "syscall_frame.h"
#include "syscalls.h"
#include "vfs.h"

#include "check.h"

#include <dirent.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Linux AArch64 syscall numbers and constants (asm-generic headers).
constexpr std::uint64_t nr_munmap = 215;
constexpr std::uint64_t nr_mremap = 216;
constexpr std::uint64_t nr_mmap = 222;
constexpr std::uint64_t nr_mprotect = 226;
constexpr std::uint64_t nr_madvise = 233;
constexpr std::uint64_t nr_rt_sigaction = 134;
constexpr std::uint64_t nr_rt_sigprocmask = 135;
constexpr std::uint64_t nr_getrandom = 278;
constexpr std::uint64_t nr_fcntl = 25;
constexpr std::uint64_t nr_sched_getscheduler = 120;
constexpr std::uint64_t nr_sched_getparam = 121;
constexpr std::uint64_t nr_sigaltstack = 132;
constexpr std::uint64_t nr_openat = 56;
constexpr std::uint64_t nr_faccessat = 48;
constexpr std::uint64_t nr_faccessat2 = 439;
constexpr std::uint64_t nr_newfstatat = 79;
constexpr std::uint64_t nr_fstat = 80;
constexpr std::uint64_t nr_futex = 98;
constexpr std::uint64_t nr_clock_gettime = 113;
constexpr std::uint64_t nr_sched_getaffinity = 123;
constexpr std::uint64_t nr_prctl = 167;
constexpr std::uint64_t nr_close = 57;
constexpr std::uint64_t nr_pipe2 = 59;

constexpr std::uint64_t prot_read = 1, prot_write = 2, prot_exec = 4;
constexpr std::uint64_t map_private = 0x02, map_fixed = 0x10, map_anonymous = 0x20, map_stack = 0x20000;
constexpr std::uint64_t map_fixed_noreplace = 0x10'0000, map_hugetlb = 0x40000;
constexpr std::uint64_t madv_dontneed = 4, madv_free = 8, madv_wipeonfork = 18, madv_hwpoison = 100;
constexpr std::uint64_t mremap_maymove = 1, mremap_fixed = 2, mremap_dontunmap = 4;
constexpr std::int64_t eperm = 1, ebadf = 9, enomem = 12, eacces = 13, efault = 14, eexist = 17, einval = 22,
                         esrch = 3, enoent = 2, eagain = 11, etimedout = 110;

constexpr std::uint64_t page = 16 * 1024;

/// Room for a Linux struct termios (60 bytes on arm64).
void *buffer_for_termios() {
    static char buffer[64];
    return buffer;
}

std::int64_t call(std::uint64_t number, std::initializer_list<std::uint64_t> arguments) {
    apkrt_syscall_frame frame{};
    std::size_t index = 0;
    for (const auto argument : arguments) frame.x[index++] = argument;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

/// The host descriptor behind the guest's `fd`, or -1.
int host_of(std::int64_t fd) {
    const auto description = apkrt::descriptors::get(static_cast<int>(fd));
    return description ? description->host() : -1;
}

/// A new pipe of the guest's: its read end in [0], its write end in [1].
void guest_pipe(int (&ends)[2]) {
    ends[0] = ends[1] = -1;
    CHECK(call(nr_pipe2, {reinterpret_cast<std::uint64_t>(ends), 0}) == 0);
}

std::uint64_t map_anonymous_pages(std::size_t count) {
    return static_cast<std::uint64_t>(call(nr_mmap, {0, count * page, prot_read | prot_write,
                                                     map_private | map_anonymous, ~std::uint64_t{0}, 0}));
}

TEST(mmap_maps_anonymous_memory_the_linux_way) {
    const auto address = static_cast<std::uint64_t>(
        call(nr_mmap, {0, 2 * page, prot_read | prot_write, map_private | map_anonymous | map_stack, ~0ull, 0}));
    CHECK(address % page == 0 && address > 0x1'0000'0000);  // an address, not -errno
    auto *bytes = reinterpret_cast<volatile std::uint8_t *>(address);
    CHECK(bytes[0] == 0 && bytes[2 * page - 1] == 0);
    bytes[page] = 7;
    CHECK(bytes[page] == 7);
    CHECK(call(nr_mprotect, {address, page, prot_read}) == 0);
    CHECK(call(nr_munmap, {address, 2 * page}) == 0);
}

TEST(mmap_never_hands_out_executable_memory) {
    CHECK(call(nr_mmap, {0, page, prot_read | prot_exec, map_private | map_anonymous, ~0ull, 0}) == -eacces);
    const auto address = map_anonymous_pages(1);
    CHECK(call(nr_mprotect, {address, page, prot_read | prot_exec}) == -eacces);
    CHECK(call(nr_munmap, {address, page}) == 0);
}

TEST(mmap_refuses_flags_it_does_not_know) {
    CHECK(call(nr_mmap, {0, page, prot_read, map_private | map_anonymous | map_hugetlb, ~0ull, 0}) == -einval);
    CHECK(call(nr_mmap, {0, page, prot_read, map_anonymous, ~0ull, 0}) == -einval);  // neither private nor shared
    CHECK(call(nr_mmap, {0, page, prot_read | 0x10 /* PROT_BTI */, map_private | map_anonymous, ~0ull, 0}) == -einval);
}

TEST(mmap_fixed_noreplace_does_not_replace) {
    const auto taken = map_anonymous_pages(1);
    CHECK(call(nr_mmap, {taken, page, prot_read, map_private | map_anonymous | map_fixed_noreplace, ~0ull, 0}) ==
          -eexist);
    CHECK(*reinterpret_cast<volatile std::uint8_t *>(taken) == 0);  // still mapped, still ours
    CHECK(call(nr_munmap, {taken, page}) == 0);

    // A free address is used exactly.
    CHECK(call(nr_mmap, {taken, page, prot_read, map_private | map_anonymous | map_fixed_noreplace, ~0ull, 0}) ==
          static_cast<std::int64_t>(taken));
    CHECK(call(nr_mmap, {taken, page, prot_read | prot_write, map_private | map_anonymous | map_fixed, ~0ull, 0}) ==
          static_cast<std::int64_t>(taken));
    CHECK(call(nr_munmap, {taken, page}) == 0);
}

TEST(madvise_dontneed_reads_back_as_zero) {
    // Linux guarantees zero pages after MADV_DONTNEED on private anonymous memory; allocators rely
    // on it. Darwin's MADV_DONTNEED keeps the contents.
    const auto address = map_anonymous_pages(2);
    std::memset(reinterpret_cast<void *>(address), 0x5a, 2 * page);
    CHECK(call(nr_madvise, {address, 2 * page, madv_dontneed}) == 0);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(address);
    CHECK(bytes[0] == 0 && bytes[page] == 0 && bytes[2 * page - 1] == 0);
    CHECK(call(nr_munmap, {address, 2 * page}) == 0);
}

TEST(madvise_accepts_fork_advice_and_refuses_unknown_advice) {
    const auto address = map_anonymous_pages(1);
    CHECK(call(nr_madvise, {address, page, madv_wipeonfork}) == 0);  // iOS has no fork (F11)
    CHECK(call(nr_madvise, {address, page, madv_hwpoison}) == -einval);
    CHECK(call(nr_munmap, {address, page}) == 0);
}

/// Whether [address, address + length) is free: MAP_FIXED_NOREPLACE maps exactly there only then.
bool is_free(std::uint64_t address, std::uint64_t length) {
    const auto result =
        call(nr_mmap, {address, length, prot_read, map_private | map_anonymous | map_fixed_noreplace, ~0ull, 0});
    if (result != static_cast<std::int64_t>(address)) return false;
    CHECK(call(nr_munmap, {address, length}) == 0);
    return true;
}

/// The host's current protection of the page at `address`, or -1 where nothing is mapped.
int protection_at(std::uint64_t address) {
    vm_address_t region = static_cast<vm_address_t>(address);
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (::vm_region_64(::mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS) {
        return -1;
    }
    if (object != MACH_PORT_NULL) ::mach_port_deallocate(::mach_task_self(), object);
    return region <= address ? info.protection : -1;
}

TEST(mremap_grows_a_mapping_where_it_lies) {
    // Three pages, of which the last two are given back: the first grows into them in place.
    const auto address = map_anonymous_pages(3);
    CHECK(call(nr_munmap, {address + page, 2 * page}) == 0);
    std::memset(reinterpret_cast<void *>(address), 0x5a, page);
    CHECK(call(nr_mremap, {address, page, 3 * page, 0, 0}) == static_cast<std::int64_t>(address));
    auto *bytes = reinterpret_cast<volatile std::uint8_t *>(address);
    CHECK(bytes[0] == 0x5a && bytes[page - 1] == 0x5a && bytes[page] == 0 && bytes[3 * page - 1] == 0);
    bytes[2 * page] = 1;  // writable, as the mapping is
    CHECK(protection_at(address + 2 * page) == (VM_PROT_READ | VM_PROT_WRITE));
    CHECK(call(nr_munmap, {address, 3 * page}) == 0);
}

TEST(mremap_moves_a_mapping_that_cannot_grow_where_it_lies) {
    // The first page of a two-page mapping ends inside it, so it cannot grow in place, as on Linux: ENOMEM
    // without MREMAP_MAYMOVE, a move with it. Lengths round up to the page.
    const auto address = map_anonymous_pages(2);
    std::memset(reinterpret_cast<void *>(address), 0x5a, page);
    reinterpret_cast<volatile std::uint8_t *>(address)[page] = 0x77;
    CHECK(call(nr_mremap, {address, page, 2 * page, 0, 0}) == -enomem);
    const auto moved = call(nr_mremap, {address, page - 100, 3 * page - 100, mremap_maymove, 0});
    const auto target = static_cast<std::uint64_t>(moved);
    CHECK(moved > 0 && target != address && target % page == 0);
    const auto *bytes = reinterpret_cast<const volatile std::uint8_t *>(target);
    CHECK(bytes[0] == 0x5a && bytes[page - 1] == 0x5a && bytes[page] == 0 && bytes[3 * page - 1] == 0);
    CHECK(is_free(address, page));  // the old range is gone
    CHECK(reinterpret_cast<const volatile std::uint8_t *>(address)[page] == 0x77);  // the rest of its mapping stays
    // A moved mapping grows again, as bionic's array does at its next doubling.
    const auto again = call(nr_mremap, {target, 3 * page, 6 * page, mremap_maymove, 0});
    CHECK(again > 0 && reinterpret_cast<const volatile std::uint8_t *>(again)[page - 1] == 0x5a);
    // Scudo frees with MADV_DONTNEED and takes zero pages back, after a move too.
    CHECK(call(nr_madvise, {static_cast<std::uint64_t>(again), page, madv_dontneed}) == 0);
    CHECK(reinterpret_cast<const volatile std::uint8_t *>(again)[page - 1] == 0);
    CHECK(call(nr_munmap, {static_cast<std::uint64_t>(again), 6 * page}) == 0);
    CHECK(call(nr_munmap, {address + page, page}) == 0);
}

TEST(mremap_shrinks_a_mapping_by_unmapping_its_tail) {
    const auto address = map_anonymous_pages(3);
    reinterpret_cast<volatile std::uint8_t *>(address)[0] = 9;
    CHECK(call(nr_mremap, {address, 3 * page, page, 0, 0}) == static_cast<std::int64_t>(address));
    CHECK(reinterpret_cast<const volatile std::uint8_t *>(address)[0] == 9);
    CHECK(is_free(address + page, 2 * page));
    // Equal lengths, after rounding up to the page, change nothing.
    CHECK(call(nr_mremap, {address, page, page - 100, 0, 0}) == static_cast<std::int64_t>(address));
    CHECK(call(nr_mremap, {address, page - 100, page, 0, 0}) == static_cast<std::int64_t>(address));
    CHECK(call(nr_munmap, {address, page}) == 0);
}

TEST(mremap_grows_a_read_only_array_as_bionic_atexit_does) {
    // bionic's __cxa_atexit keeps its handler array read-only and grows it with MREMAP_MAYMOVE; then it makes the
    // page of the next entry writable for a moment.
    const auto address = map_anonymous_pages(1);
    std::memset(reinterpret_cast<void *>(address), 0x5a, page);
    CHECK(call(nr_mprotect, {address, page, prot_read}) == 0);
    const auto grown = call(nr_mremap, {address, page, 2 * page, mremap_maymove, 0});
    const auto base = static_cast<std::uint64_t>(grown);
    CHECK(grown > 0 && base % page == 0);
    CHECK(protection_at(base) == VM_PROT_READ && protection_at(base + page) == VM_PROT_READ);
    CHECK(reinterpret_cast<const volatile std::uint8_t *>(base)[page - 1] == 0x5a);
    CHECK(reinterpret_cast<const volatile std::uint8_t *>(base)[page] == 0);
    CHECK(call(nr_mprotect, {base + page, page, prot_read | prot_write}) == 0);
    reinterpret_cast<volatile std::uint8_t *>(base)[page] = 1;
    CHECK(call(nr_mprotect, {base + page, page, prot_read}) == 0);
    CHECK(call(nr_munmap, {base, 2 * page}) == 0);
}

TEST(mremap_moves_inaccessible_pages_without_reading_them) {
    // PROT_NONE pages move with their contents and their protection; nothing reads them on the way.
    const auto address = map_anonymous_pages(2);
    reinterpret_cast<volatile std::uint8_t *>(address)[0] = 3;
    CHECK(call(nr_mprotect, {address, page, 0}) == 0);
    const auto moved = call(nr_mremap, {address, page, 2 * page, mremap_maymove, 0});
    CHECK(moved > 0 && static_cast<std::uint64_t>(moved) != address);
    const auto target = static_cast<std::uint64_t>(moved);
    CHECK(protection_at(target) == VM_PROT_NONE && protection_at(target + page) == VM_PROT_NONE);
    CHECK(call(nr_mprotect, {target, page, prot_read}) == 0);
    CHECK(reinterpret_cast<const volatile std::uint8_t *>(target)[0] == 3);
    CHECK(call(nr_munmap, {target, 2 * page}) == 0);
    CHECK(call(nr_munmap, {address + page, page}) == 0);
}

TEST(mremap_grows_only_within_one_mapping) {
    // Two pages of different protections are two mappings on Linux: growing across them answers EFAULT.
    const auto address = map_anonymous_pages(2);
    CHECK(call(nr_mprotect, {address + page, page, prot_read}) == 0);
    CHECK(call(nr_mremap, {address, 2 * page, 3 * page, mremap_maymove, 0}) == -efault);
    // Shrinking crosses them freely, as Linux unmaps whatever lies in the tail.
    CHECK(call(nr_mremap, {address, 2 * page, page, 0, 0}) == static_cast<std::int64_t>(address));
    CHECK(is_free(address + page, page));
    // Two neighbours of one protection, mapped one by one, are one mapping, as Linux merges them.
    CHECK(call(nr_mmap, {address + page, page, prot_read | prot_write, map_private | map_anonymous | map_fixed, ~0ull,
                         0}) == static_cast<std::int64_t>(address + page));
    const auto grown = call(nr_mremap, {address, 2 * page, 3 * page, mremap_maymove, 0});
    CHECK(grown > 0);
    CHECK(call(nr_munmap, {static_cast<std::uint64_t>(grown), 3 * page}) == 0);
    // A hole is no mapping either.
    const auto holed = map_anonymous_pages(3);
    CHECK(call(nr_munmap, {holed + page, page}) == 0);
    CHECK(call(nr_mremap, {holed, 3 * page, 4 * page, mremap_maymove, 0}) == -efault);
    CHECK(call(nr_munmap, {holed, 3 * page}) == 0);
}

/// mremap of the page right below the one that the call's runtime frames lie in, made from near the top of a page
/// so that sys_mremap and the calls it makes share that page: only the guard's extra page covers the one below.
[[gnu::noinline]] std::int64_t remap_below_the_calls() {
    std::uint8_t mark = 0;
    const auto here = reinterpret_cast<std::uint64_t>(&mark);
    auto *room = static_cast<volatile std::uint8_t *>(__builtin_alloca((here & (page - 1)) + 512));
    room[0] = mark;
    const auto calls = reinterpret_cast<std::uint64_t>(room) & ~(page - 1);
    return call(nr_mremap, {calls - page, page, 2 * page, mremap_maymove, 0});
}

TEST(mremap_leaves_the_runtime_frames_of_the_calling_thread_mapped) {
    // The syscall layer runs on the guest thread's stack, as in sys_munmap: neither the old range nor a FIXED
    // target may take the pages it runs on (EPERM, as for executable pages).
    std::uint8_t local = 0;
    const auto here = (reinterpret_cast<std::uint64_t>(&local) & ~(page - 1)) - page;
    CHECK(call(nr_mremap, {here, 4 * page, 5 * page, mremap_maymove, 0}) == -eperm);
    const auto spare = map_anonymous_pages(1);
    CHECK(call(nr_mremap, {spare, page, 4 * page, mremap_maymove | mremap_fixed, here}) == -eperm);
    CHECK(call(nr_munmap, {spare, page}) == 0);
    CHECK(local == 0);
    // The page below the calls' own is kept too, for the calls sys_mremap makes.
    CHECK(remap_below_the_calls() == -eperm);
}

TEST(mremap_fixed_moves_onto_its_target) {
    const auto source = map_anonymous_pages(1);
    std::memset(reinterpret_cast<void *>(source), 0x5a, page);
    const auto target = map_anonymous_pages(3);
    std::memset(reinterpret_cast<void *>(target), 0x11, 3 * page);
    CHECK(call(nr_mremap, {source, page, 2 * page, mremap_fixed, target}) == -einval);  // FIXED needs MAYMOVE
    CHECK(call(nr_mremap, {source, page, 2 * page, mremap_maymove | mremap_fixed, target + 1}) == -einval);
    CHECK(call(nr_mremap, {source, page, 2 * page, mremap_maymove | mremap_fixed, source - page}) == -einval);
    CHECK(call(nr_mremap, {source, page, 2 * page, mremap_maymove | mremap_fixed, target}) ==
          static_cast<std::int64_t>(target));
    const auto *bytes = reinterpret_cast<const volatile std::uint8_t *>(target);
    // What lay at the target went: the source's page, then zero; beyond the new length the target's mapping stays.
    CHECK(bytes[0] == 0x5a && bytes[page - 1] == 0x5a && bytes[page] == 0 && bytes[2 * page - 1] == 0);
    CHECK(bytes[2 * page] == 0x11);
    CHECK(is_free(source, page));
    CHECK(call(nr_munmap, {target, 3 * page}) == 0);

    // A shorter FIXED move takes the old range's first pages, and its tail goes.
    const auto longer = map_anonymous_pages(3);
    std::memset(reinterpret_cast<void *>(longer), 0x22, 3 * page);
    const auto place = map_anonymous_pages(1);
    CHECK(call(nr_mremap, {longer, 3 * page, page, mremap_maymove | mremap_fixed, place}) ==
          static_cast<std::int64_t>(place));
    CHECK(reinterpret_cast<const volatile std::uint8_t *>(place)[page - 1] == 0x22);
    CHECK(is_free(longer, 3 * page));
    CHECK(call(nr_munmap, {place, page}) == 0);
}

TEST(mremap_moves_a_file_mapping_but_grows_none) {
    // Growing a file mapping would map more of the file on Linux; here it answers ENOMEM (named). A move keeps a
    // private mapping of the file: its pages show the file, and a write stays in the mapping.
    char path[] = "/tmp/apkrt-mremap-XXXXXX";
    const int host_fd = ::mkstemp(path);
    CHECK(host_fd >= 0);
    const std::vector<char> content(2 * page, 'f');
    CHECK(::write(host_fd, content.data(), content.size()) == static_cast<ssize_t>(content.size()));
    const auto fd = static_cast<std::uint64_t>(apkrt::descriptors::add(::dup(host_fd), false));
    const auto mapped = call(nr_mmap, {0, page, prot_read | prot_write, map_private, fd, 0});
    CHECK(mapped > 0);
    const auto address = static_cast<std::uint64_t>(mapped);
    CHECK(call(nr_mremap, {address, page, 2 * page, mremap_maymove, 0}) == -enomem);
    const auto target = map_anonymous_pages(1);
    CHECK(call(nr_mremap, {address, page, page, mremap_maymove | mremap_fixed, target}) ==
          static_cast<std::int64_t>(target));
    auto *bytes = reinterpret_cast<volatile char *>(target);
    CHECK(bytes[0] == 'f' && bytes[page - 1] == 'f');
    bytes[0] = 'x';
    char first = 0;
    CHECK(::pread(host_fd, &first, 1, 0) == 1 && first == 'f');
    CHECK(call(nr_munmap, {target, page}) == 0);
    CHECK(apkrt::descriptors::close(static_cast<int>(fd)) == 0);
    ::close(host_fd);
    ::unlink(path);
}

TEST(mremap_answers_overflowing_lengths) {
    const auto address = map_anonymous_pages(1);
    // An old range that wraps past the end of the address space: EFAULT, where Linux's munmap of the tail answers
    // EINVAL (named).
    CHECK(call(nr_mremap, {address, ~std::uint64_t{0} - 2 * page + 1, page, 0, 0}) == -efault);
    // A FIXED target that wraps: EINVAL, as on Linux.
    CHECK(call(nr_mremap, {address, page, page, mremap_maymove | mremap_fixed, ~std::uint64_t{0} - page + 1}) ==
          -einval);
    // An old length that rounds up to 0: a duplicate of a private mapping, EINVAL, as on Linux.
    CHECK(call(nr_mremap, {address, ~std::uint64_t{0}, 2 * page, mremap_maymove, 0}) == -einval);
    CHECK(call(nr_munmap, {address, page}) == 0);
}

TEST(mremap_leaves_neighbouring_host_memory_alone) {
    // A guest mapping right below host memory of another tag (malloc's): one range over both is no mapping of
    // Linux's, so growing it answers EFAULT and never moves the host's pages.
    void *const host = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                              VM_MAKE_TAG(VM_MEMORY_MALLOC), 0);
    CHECK(host != MAP_FAILED);
    const auto above = reinterpret_cast<std::uint64_t>(host) + page;
    CHECK(::munmap(host, page) == 0);  // the guest's page goes right below the host's
    const auto below = above - page;
    CHECK(call(nr_mmap, {below, page, prot_read | prot_write, map_private | map_anonymous | map_fixed_noreplace,
                         ~0ull, 0}) == static_cast<std::int64_t>(below));
    reinterpret_cast<volatile std::uint8_t *>(above)[0] = 0x44;
    CHECK(call(nr_mremap, {below, 2 * page, 3 * page, mremap_maymove, 0}) == -efault);
    CHECK(reinterpret_cast<const volatile std::uint8_t *>(above)[0] == 0x44);
    CHECK(call(nr_munmap, {below, page}) == 0);
    CHECK(::munmap(reinterpret_cast<void *>(above), page) == 0);
}

void code_page_marker() {}

TEST(mremap_leaves_executable_pages_where_they_are) {
    // Signed code stays where the loader put it: remapping it answers EPERM, Linux's answer for a sealed mapping,
    // and the code still runs.
    const auto code = reinterpret_cast<std::uint64_t>(&code_page_marker) & ~(page - 1);
    CHECK((protection_at(code) & VM_PROT_EXECUTE) != 0);
    CHECK(call(nr_mremap, {code, page, 2 * page, mremap_maymove, 0}) == -eperm);
    CHECK(call(nr_mremap, {code, 2 * page, page, 0, 0}) == -eperm);
    const auto spare = map_anonymous_pages(1);
    CHECK(call(nr_mremap, {code, page, page, mremap_maymove | mremap_fixed, spare}) == -eperm);
    CHECK(call(nr_munmap, {spare, page}) == 0);
    code_page_marker();
}

TEST(mremap_answers_linux_errors) {
    const auto address = map_anonymous_pages(1);
    CHECK(call(nr_mremap, {address, page, 2 * page, 8, 0}) == -einval);  // an unknown flag
    CHECK(call(nr_mremap, {address + 1, page, 2 * page, mremap_maymove, 0}) == -einval);  // an unaligned address
    CHECK(call(nr_mremap, {address, page, 0, mremap_maymove, 0}) == -einval);  // a new length of 0
    CHECK(call(nr_mremap, {address, 0, page, mremap_maymove, 0}) == -einval);  // duplicating a private mapping
    // MREMAP_DONTUNMAP is not offered, as before Linux 5.7, which refuses it as an unknown flag.
    CHECK(call(nr_mremap, {address, page, page, mremap_maymove | mremap_dontunmap, 0}) == -einval);
    CHECK(call(nr_munmap, {address, page}) == 0);
    CHECK(call(nr_mremap, {address, page, 2 * page, mremap_maymove, 0}) == -efault);  // nothing mapped there
}

/// Linux's mmap_lock: while sys_mremap has looked at the VM map and not yet changed it (memory::test_pause), another
/// thread's memory call waits. `other` is that call; the pause holds the look for up to 200 ms and records whether
/// the call finished meanwhile.
namespace memory_lock_test {
std::uint64_t old_range = 0;
void (*other)() = nullptr;
std::atomic<bool> finished{false};
std::thread thread;
bool waited = false;

void run_the_other_call() {
    apkrt::sys::memory::test_pause = nullptr;  // once
    finished = false;
    thread = std::thread([] {
        other();
        finished = true;
    });
    for (int i = 0; i < 200 && !finished.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    waited = !finished.load();
}

/// An mremap that grows the first page of a two-page mapping to two pages, which moves it, while `call_from_other`
/// changes the second page on another thread.
void check_waits(const char *what, void (*call_from_other)()) {
    old_range = map_anonymous_pages(2);
    other = call_from_other;
    waited = false;
    apkrt::sys::memory::test_pause = run_the_other_call;
    const auto moved = call(nr_mremap, {old_range, page, 2 * page, mremap_maymove, 0});
    apkrt::sys::memory::test_pause = nullptr;
    const bool paused = thread.joinable();
    if (paused) thread.join();
    if (!waited) std::fprintf(stderr, "%s did not wait for mremap\n", what);
    CHECK(paused && waited);
    CHECK(moved > 0);
    CHECK(call(nr_munmap, {static_cast<std::uint64_t>(moved), 2 * page}) == 0);
    call(nr_munmap, {old_range, 2 * page});
}

}  // namespace memory_lock_test

TEST(memory_calls_wait_while_mremap_looks_and_moves) {
    using namespace memory_lock_test;
    // The calls that open a hole in the range mremap is about to grow into, or change what lies there.
    check_waits("munmap", [] { call(nr_munmap, {old_range + page, page}); });
    check_waits("mmap", [] {
        call(nr_mmap, {old_range + page, page, prot_read, map_private | map_anonymous | map_fixed, ~0ull, 0});
    });
    check_waits("mprotect", [] { call(nr_mprotect, {old_range + page, page, prot_read}); });
    check_waits("madvise DONTNEED", [] { call(nr_madvise, {old_range + page, page, madv_dontneed}); });
    check_waits("madvise FREE", [] { call(nr_madvise, {old_range + page, page, madv_free}); });
    // A thread's exit, which unmaps the stack it deferred: threads_test.cpp.
}

TEST(rt_sigprocmask_keeps_real_time_signals_in_the_guest_mask) {
    const std::uint64_t sigusr1 = 1ull << (10 - 1);
    const std::uint64_t sigrtmin_plus_3 = 1ull << (35 - 1);  // one of bionic's own
    const std::uint64_t sigkill = 1ull << (9 - 1);
    std::uint64_t old_mask = 0;
    std::uint64_t mask = sigusr1 | sigrtmin_plus_3 | sigkill;
    CHECK(call(nr_rt_sigprocmask, {0 /* SIG_BLOCK */, reinterpret_cast<std::uint64_t>(&mask),
                                   reinterpret_cast<std::uint64_t>(&old_mask), 8}) == 0);
    std::uint64_t now = 0;
    CHECK(call(nr_rt_sigprocmask, {0, 0, reinterpret_cast<std::uint64_t>(&now), 8}) == 0);
    CHECK((now & sigusr1) != 0);
    CHECK((now & sigrtmin_plus_3) != 0);
    CHECK((now & sigkill) == 0);  // cannot be blocked
    CHECK(call(nr_rt_sigprocmask, {2 /* SIG_SETMASK */, reinterpret_cast<std::uint64_t>(&old_mask), 0, 8}) == 0);
    CHECK(call(nr_rt_sigprocmask, {0, 0, reinterpret_cast<std::uint64_t>(&now), 8}) == 0);
    CHECK(now == old_mask);
    CHECK(call(nr_rt_sigprocmask, {0, 0, reinterpret_cast<std::uint64_t>(&now), 4}) == -einval);
}

TEST(rt_sigaction_records_dispositions_of_signals_darwin_lacks) {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } ignore{1 /* SIG_IGN */, 0, 0, 0}, previous{};
    CHECK(call(nr_rt_sigaction, {36, reinterpret_cast<std::uint64_t>(&ignore), 0, 8}) == 0);
    CHECK(call(nr_rt_sigaction, {36, 0, reinterpret_cast<std::uint64_t>(&previous), 8}) == 0);
    CHECK(previous.handler == 1);
    CHECK(call(nr_rt_sigaction, {9 /* SIGKILL */, reinterpret_cast<std::uint64_t>(&ignore), 0, 8}) == -einval);
    CHECK(call(nr_rt_sigaction, {65, 0, reinterpret_cast<std::uint64_t>(&previous), 8}) == -einval);
}

TEST(getrandom_fills_the_whole_buffer) {
    std::uint8_t buffer[64] = {};
    CHECK(call(nr_getrandom, {reinterpret_cast<std::uint64_t>(buffer), sizeof buffer, 1 /* GRND_NONBLOCK */}) == 64);
    int zeros = 0;
    for (const auto byte : buffer) zeros += byte == 0;
    CHECK(zeros < 16);
    CHECK(call(nr_getrandom, {reinterpret_cast<std::uint64_t>(buffer), sizeof buffer, 0x80}) == -einval);
}

TEST(fcntl_speaks_linux_flags) {
    int fds[2];
    guest_pipe(fds);
    constexpr std::uint64_t f_dupfd = 0, f_getfd = 1, f_setfd = 2, f_getfl = 3, f_setfl = 4, f_dupfd_cloexec = 1030;
    constexpr std::int64_t linux_o_wronly = 01, linux_o_nonblock = 04000;
    const auto write_end = static_cast<std::uint64_t>(fds[1]);

    CHECK(call(nr_fcntl, {write_end, f_getfl}) == linux_o_wronly);
    CHECK(call(nr_fcntl, {write_end, f_setfl, static_cast<std::uint64_t>(linux_o_nonblock)}) == 0);
    CHECK(call(nr_fcntl, {write_end, f_getfl}) == (linux_o_wronly | linux_o_nonblock));
    CHECK((::fcntl(host_of(fds[1]), F_GETFL) & O_NONBLOCK) != 0);  // really set on the host

    CHECK(call(nr_fcntl, {write_end, f_getfd}) == 0);
    CHECK(call(nr_fcntl, {write_end, f_setfd, 1 /* FD_CLOEXEC */}) == 0);
    CHECK(call(nr_fcntl, {write_end, f_getfd}) == 1);

    const auto copy = call(nr_fcntl, {write_end, f_dupfd_cloexec, 100});
    CHECK(copy >= 100);
    CHECK(call(nr_fcntl, {static_cast<std::uint64_t>(copy), f_getfd}) == 1);
    const auto plain_copy = call(nr_fcntl, {write_end, f_dupfd, 0});
    CHECK(plain_copy >= 0 && call(nr_fcntl, {static_cast<std::uint64_t>(plain_copy), f_getfd}) == 0);

    // bionic's start-up checks stdin, stdout and stderr like this and accepts only EBADF as failure.
    CHECK(call(nr_fcntl, {999, f_getfl}) == -ebadf);

    for (const auto fd : {std::int64_t{fds[0]}, std::int64_t{fds[1]}, copy, plain_copy}) {
        call(nr_close, {static_cast<std::uint64_t>(fd)});
    }
}

TEST(sched_queries_answer_like_an_ordinary_linux_thread) {
    CHECK(call(nr_sched_getscheduler, {0}) == 0);  // SCHED_OTHER
    CHECK(call(nr_sched_getscheduler, {static_cast<std::uint64_t>(::getpid())}) == 0);
    CHECK(call(nr_sched_getscheduler, {999'999}) == -esrch);
    int priority = -1;
    CHECK(call(nr_sched_getparam, {0, reinterpret_cast<std::uint64_t>(&priority)}) == 0);
    CHECK(priority == 0);
}

TEST(sigaltstack_keeps_the_guest_stack_in_linux_layout) {
    // Linux arm64 stack_t: ss_sp, ss_flags (int, padded), ss_size. Darwin orders size before flags.
    struct LinuxStack {
        std::uint64_t sp;
        std::int32_t flags;
        std::int32_t padding;
        std::uint64_t size;
    };
    constexpr std::int32_t ss_disable = 2;
    static std::uint8_t area[16 * 1024];

    LinuxStack old{};
    CHECK(call(nr_sigaltstack, {0, reinterpret_cast<std::uint64_t>(&old)}) == 0);
    CHECK(old.flags == ss_disable);  // a new thread has none

    const LinuxStack wanted{reinterpret_cast<std::uint64_t>(area), 0, 0, sizeof area};
    CHECK(call(nr_sigaltstack, {reinterpret_cast<std::uint64_t>(&wanted), 0}) == 0);
    LinuxStack now{};
    CHECK(call(nr_sigaltstack, {0, reinterpret_cast<std::uint64_t>(&now)}) == 0);
    CHECK(now.sp == wanted.sp && now.size == wanted.size && now.flags == 0);

    const LinuxStack too_small{reinterpret_cast<std::uint64_t>(area), 0, 0, 1024};
    CHECK(call(nr_sigaltstack, {reinterpret_cast<std::uint64_t>(&too_small), 0}) == -12);  // ENOMEM

    const LinuxStack off{0, ss_disable, 0, 0};
    CHECK(call(nr_sigaltstack, {reinterpret_cast<std::uint64_t>(&off), 0}) == 0);
    CHECK(call(nr_sigaltstack, {0, reinterpret_cast<std::uint64_t>(&now)}) == 0);
    CHECK(now.flags == ss_disable);
}

/// struct stat as the Linux kernel fills it on arm64 (include/uapi/asm-generic/stat.h), 128 bytes.
struct LinuxStat {
    std::uint64_t dev;
    std::uint64_t ino;
    std::uint32_t mode;
    std::uint32_t nlink;
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint64_t rdev;
    std::uint64_t pad1;
    std::int64_t size;
    std::int32_t blksize;
    std::int32_t pad2;
    std::int64_t blocks;
    std::int64_t atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    std::uint32_t unused4, unused5;
};
static_assert(sizeof(LinuxStat) == 128);

TEST(fstat_fills_the_linux_struct_stat) {
    char path[] = "/tmp/apkrt-fstat-XXXXXX";
    const int host_fd = ::mkstemp(path);
    CHECK(host_fd >= 0);
    CHECK(::write(host_fd, "twelve bytes", 12) == 12);
    const auto fd = apkrt::descriptors::add(::dup(host_fd), false);  // as the runtime takes one in
    LinuxStat linux_stat;
    std::memset(&linux_stat, 0xee, sizeof linux_stat);
    CHECK(call(nr_fstat, {static_cast<std::uint64_t>(fd), reinterpret_cast<std::uint64_t>(&linux_stat)}) == 0);
    struct stat host;
    CHECK(::fstat(host_fd, &host) == 0);
    CHECK(linux_stat.size == 12);
    CHECK((linux_stat.mode & 0170000) == 0100000);  // S_IFREG, same bits on both systems
    CHECK((linux_stat.mode & 0777) == (host.st_mode & 0777));
    CHECK(linux_stat.ino == host.st_ino);
    CHECK(linux_stat.nlink == 1 && linux_stat.uid == 0 && linux_stat.gid == 0);  // outside the guest's paths: root's
    CHECK(linux_stat.blksize > 0);
    CHECK(linux_stat.mtime == host.st_mtimespec.tv_sec && linux_stat.mtime_nsec == host.st_mtimespec.tv_nsec);
    CHECK(linux_stat.pad1 == 0 && linux_stat.pad2 == 0 && linux_stat.unused4 == 0 && linux_stat.unused5 == 0);

    // newfstatat with an empty path and AT_EMPTY_PATH is fstat, too.
    LinuxStat same{};
    CHECK(call(nr_newfstatat, {static_cast<std::uint64_t>(fd), reinterpret_cast<std::uint64_t>(""),
                               reinterpret_cast<std::uint64_t>(&same), 0x1000}) == 0);
    CHECK(same.ino == linux_stat.ino && same.size == 12);

    int fds[2];
    guest_pipe(fds);
    CHECK(call(nr_fstat, {static_cast<std::uint64_t>(fds[1]), reinterpret_cast<std::uint64_t>(&linux_stat)}) == 0);
    CHECK((linux_stat.mode & 0170000) == 0010000);  // S_IFIFO
    CHECK(call(nr_fstat, {999, reinterpret_cast<std::uint64_t>(&linux_stat)}) == -ebadf);
    for (const auto open_fd : {std::int64_t{fds[0]}, std::int64_t{fds[1]}, static_cast<std::int64_t>(fd)}) {
        call(nr_close, {static_cast<std::uint64_t>(open_fd)});
    }
    ::close(host_fd);
    ::unlink(path);
}

TEST(paths_outside_every_mount_do_not_exist) {
    // The guest sees only what the path table puts under Android paths (ARCHITECTURE.md 4.5).
    apkrt::vfs::reset();
    constexpr std::uint64_t at_fdcwd = static_cast<std::uint64_t>(-100);
    LinuxStat buffer{};
    CHECK(call(nr_newfstatat, {at_fdcwd, reinterpret_cast<std::uint64_t>("/dev/__properties__"),
                               reinterpret_cast<std::uint64_t>(&buffer), 0}) == -enoent);
    CHECK(call(nr_openat, {at_fdcwd, reinterpret_cast<std::uint64_t>("/etc/hosts"), 0, 0}) == -enoent);
    CHECK(call(nr_openat, {at_fdcwd, reinterpret_cast<std::uint64_t>("/"), 0, 0}) == -enoent);
}

TEST(the_basic_devices_are_there) {
    // /dev/null, /dev/zero and the random devices are always there: bionic's arc4random checks
    // access("/dev/urandom") early and otherwise lives on the 16 bytes of AT_RANDOM alone.
    apkrt::vfs::reset();
    constexpr std::uint64_t at_fdcwd = static_cast<std::uint64_t>(-100);
    constexpr std::uint64_t o_wronly = 1, o_nonblock = 04000, o_cloexec = 02000000;
    const auto path = [](const char *text) { return reinterpret_cast<std::uint64_t>(text); };

    const auto null = static_cast<int>(call(nr_openat, {at_fdcwd, path("/dev/null"), o_wronly | o_cloexec, 0}));
    CHECK(null >= 0);
    if (null >= 0) {
        CHECK(::write(host_of(null), "x", 1) == 1);
        CHECK(call(nr_fcntl, {static_cast<std::uint64_t>(null), 1 /* F_GETFD */}) == 1);  // O_CLOEXEC
        CHECK((::fcntl(host_of(null), F_GETFL) & O_ACCMODE) == O_WRONLY);
        call(nr_close, {static_cast<std::uint64_t>(null)});
    }
    const auto random = static_cast<int>(call(nr_openat, {at_fdcwd, path("/dev/urandom"), o_nonblock, 0}));
    CHECK(random >= 0);
    if (random >= 0) {
        unsigned char bytes[32] = {};
        CHECK(::read(host_of(random), bytes, sizeof bytes) == sizeof bytes);
        CHECK(std::count(std::begin(bytes), std::end(bytes), 0) < 8);
        CHECK(call(nr_fcntl, {static_cast<std::uint64_t>(random), 1 /* F_GETFD */}) == 0);
        CHECK((::fcntl(host_of(random), F_GETFL) & O_NONBLOCK) != 0);
        call(nr_close, {static_cast<std::uint64_t>(random)});
    }
    CHECK(call(nr_faccessat, {at_fdcwd, path("/dev/urandom"), 4 /* R_OK */}) == 0);
    CHECK(call(nr_faccessat, {at_fdcwd, path("/dev/random"), 0 /* F_OK */}) == 0);
    CHECK(call(nr_faccessat2, {at_fdcwd, path("/dev/null"), 2 /* W_OK */, 0}) == 0);
    CHECK(call(nr_faccessat, {at_fdcwd, path("/dev/nonsense"), 0}) == -enoent);
    CHECK(call(nr_faccessat, {at_fdcwd, path("/data/local/tmp"), 0}) == -enoent);
    CHECK(call(nr_faccessat, {at_fdcwd, path("dev/null"), 0}) == 0);  // relative to the working directory "/"
}

// --- Files through the path table (ARCHITECTURE.md 4.5) ---------------------------------------------

constexpr std::uint64_t nr_getcwd = 17, nr_dup = 23, nr_dup3 = 24, nr_ioctl = 29, nr_mkdirat = 34, nr_unlinkat = 35,
                        nr_symlinkat = 36, nr_statfs = 43, nr_ftruncate = 46, nr_chdir = 49, nr_fchdir = 50,
                        nr_getdents64 = 61, nr_lseek = 62, nr_read = 63, nr_write = 64,
                        nr_readv = 65, nr_writev = 66, nr_pread64 = 67, nr_pwrite64 = 68, nr_readlinkat = 78,
                        nr_fsync = 82, nr_fdatasync = 83, nr_utimensat = 88, nr_renameat2 = 276;
constexpr std::uint64_t o_rdonly = 0, o_wronly = 1, o_rdwr = 2, o_creat = 0100, o_excl = 0200, o_trunc = 01000,
                        o_append = 02000, o_nonblock = 04000, o_directory = 040000, o_cloexec = 02000000;
constexpr std::uint64_t at_fdcwd = static_cast<std::uint64_t>(-100);
constexpr std::uint64_t at_symlink_nofollow = 0x100, at_removedir = 0x200;
constexpr std::int64_t eisdir = 21, enotdir = 20, enotempty = 39, erange = 34, espipe = 29, enotty = 25;

std::uint64_t address(const void *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

std::uint64_t unsigned_of(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

/// An empty host directory as the guest's /data/local/tmp, which is also the working directory.
struct Scratch {
    std::string host;

    Scratch() {
        char pattern[] = "/tmp/apkrt-files-XXXXXX";
        char resolved[PATH_MAX];
        host = ::realpath(::mkdtemp(pattern), resolved);
        apkrt::vfs::reset();
        apkrt::vfs::mount("/data/local/tmp", host);
        apkrt::vfs::set_working_directory("/data/local/tmp");
    }
    ~Scratch() {
        const std::string command = "rm -rf '" + host + "'";
        static_cast<void>(std::system(command.c_str()));
        apkrt::vfs::reset();
    }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;

    std::string contents(const std::string &name) const {
        std::string result;
        if (FILE *file = std::fopen((host + "/" + name).c_str(), "rb")) {
            char buffer[256];
            std::size_t count;
            while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0) result.append(buffer, count);
            std::fclose(file);
        }
        return result;
    }
};

std::int64_t open_guest(std::uint64_t directory, const char *path, std::uint64_t flags, std::uint64_t mode = 0) {
    return call(nr_openat, {directory, address(path), flags, mode});
}

std::int64_t write_text(std::int64_t fd, std::string_view text) {
    return call(nr_write, {unsigned_of(fd), address(text.data()), text.size()});
}

std::string read_text(std::int64_t fd, std::size_t size) {
    std::string buffer(size, '\0');
    const auto count = call(nr_read, {unsigned_of(fd), address(buffer.data()), size});
    return count < 0 ? "error " + std::to_string(-count) : buffer.substr(0, static_cast<std::size_t>(count));
}

TEST(files_under_a_mount_are_host_files) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "/data/local/tmp/a.txt", o_creat | o_wronly | o_trunc, 0640);
    CHECK(fd >= 0);
    CHECK(write_text(fd, "hello") == 5);
    CHECK(call(nr_close, {unsigned_of(fd)}) == 0);
    CHECK(scratch.contents("a.txt") == "hello");
    struct stat host;
    CHECK(::stat((scratch.host + "/a.txt").c_str(), &host) == 0 && (host.st_mode & 0777) == 0640);

    const auto again = open_guest(at_fdcwd, "/data/local/tmp/a.txt", o_rdonly);
    CHECK(read_text(again, 16) == "hello");
    CHECK(read_text(again, 16).empty());  // end of file
    CHECK(call(nr_close, {unsigned_of(again)}) == 0);
    CHECK(call(nr_close, {unsigned_of(again)}) == -ebadf);
}

// ADR-0039 decision 3: every app runs as one app uid, 10100 (u0_a100), with the same number as its gid.
TEST(the_program_runs_as_the_app_uid) {
    constexpr std::uint64_t nr_getresuid = 148, nr_getresgid = 150, nr_getuid = 174, nr_geteuid = 175,
                            nr_getgid = 176, nr_getegid = 177;
    for (const auto number : {nr_getuid, nr_geteuid, nr_getgid, nr_getegid}) CHECK(call(number, {}) == 10100);
    for (const auto number : {nr_getresuid, nr_getresgid}) {
        std::uint32_t ids[3] = {1, 1, 1};
        CHECK(call(number, {address(&ids[0]), address(&ids[1]), address(&ids[2])}) == 0);
        CHECK(ids[0] == 10100 && ids[1] == 10100 && ids[2] == 10100);
    }
}

// ADR-0039 decision 3, with the owners Android 15 gives an app's files (the emulator, 2026-10-01): its code
// folder under /data/app is system's (1000), the rest of /data is the app's, external storage the app's with
// the group ext_data_rw (1078), a pipe the process's, and the rest root's.
TEST(files_belong_to_whom_android_15_gives_them) {
    Scratch scratch;
    for (const char *folder : {"code", "data", "storage", "system"}) {
        CHECK(::mkdir((scratch.host + "/" + folder).c_str(), 0755) == 0);
    }
    apkrt::vfs::mount("/data/app/~~AAAA/io.example-BBBB", scratch.host + "/code");
    apkrt::vfs::mount("/data/user/0/io.example", scratch.host + "/data");
    apkrt::vfs::mount("/storage/emulated/0/Android/data/io.example", scratch.host + "/storage");
    apkrt::vfs::mount("/system", scratch.host + "/system");
    const auto owner = [](const char *path) {  // "uid:gid"
        LinuxStat status{};
        CHECK(call(nr_newfstatat, {at_fdcwd, address(path), address(&status), 0}) == 0);
        return std::to_string(status.uid) + ":" + std::to_string(status.gid);
    };
    CHECK(owner("/data/app/~~AAAA/io.example-BBBB") == "1000:1000");
    CHECK(owner("/data/user/0/io.example") == "10100:10100");
    CHECK(owner("/data/local/tmp") == "10100:10100");
    CHECK(owner("/storage/emulated/0/Android/data/io.example") == "10100:1078");
    CHECK(owner("/system") == "0:0");
    CHECK(owner("/dev/null") == "0:0");

    // fstat finds the owner by the descriptor's file; a pipe and the files of /proc/self belong to the process.
    const auto fd = open_guest(at_fdcwd, "/data/user/0/io.example/new", o_creat | o_wronly, 0600);
    LinuxStat status{};
    CHECK(call(nr_fstat, {unsigned_of(fd), address(&status)}) == 0 && status.uid == 10100 && status.gid == 10100);
    call(nr_close, {unsigned_of(fd)});
    const auto code = open_guest(at_fdcwd, "/data/app/~~AAAA/io.example-BBBB", o_rdonly | o_directory);
    CHECK(call(nr_fstat, {unsigned_of(code), address(&status)}) == 0 && status.uid == 1000 && status.gid == 1000);
    call(nr_close, {unsigned_of(code)});
    int ends[2];
    guest_pipe(ends);
    CHECK(call(nr_fstat, {unsigned_of(ends[0]), address(&status)}) == 0 && status.uid == 10100 &&
          status.gid == 10100);
    for (const int end : ends) call(nr_close, {unsigned_of(end)});
    const auto proc = open_guest(at_fdcwd, "/proc/self/stat", o_rdonly);
    CHECK(call(nr_fstat, {unsigned_of(proc), address(&status)}) == 0 && status.uid == 10100 && status.gid == 10100);
    call(nr_close, {unsigned_of(proc)});
}

// ADR-0039 decision 3: access() checks reading, writing and executing for the app uid and gid against the mode
// bits of the owner and group that stat reports, as Linux's generic_permission does; AT_EACCESS answers the same,
// since the effective ids are the real ones. So an app's base.apk, system's with 0644 on Android 15 (the
// emulator, 2026-10-01), is not writable to the app, which ART asks before it opens the app's dex.
TEST(access_checks_the_mode_bits_of_the_owner_stat_reports) {
    Scratch scratch;
    for (const char *folder : {"code", "data", "system"}) {
        CHECK(::mkdir((scratch.host + "/" + folder).c_str(), 0755) == 0);
    }
    for (const auto &[name, mode] : {std::pair{"code/base.apk", 0644}, std::pair{"data/own", 0600},
                                     std::pair{"system/tool", 0755}, std::pair{"system/shared", 0666}}) {
        const auto file = scratch.host + "/" + name;
        CHECK(::close(::open(file.c_str(), O_CREAT | O_WRONLY, 0600)) == 0 &&
              ::chmod(file.c_str(), static_cast<mode_t>(mode)) == 0);
    }
    apkrt::vfs::mount("/data/app/~~AAAA/io.example-BBBB", scratch.host + "/code");
    apkrt::vfs::mount("/data/user/0/io.example", scratch.host + "/data");
    apkrt::vfs::mount("/mnt", scratch.host + "/system");  // root's, off the read-only partitions
    constexpr std::uint64_t r_ok = 4, w_ok = 2, x_ok = 1, at_eaccess = 0x200;
    const auto access = [](const char *path, std::uint64_t mode, std::uint64_t flags = 0) {
        return call(nr_faccessat2, {at_fdcwd, address(path), mode, flags});
    };
    // System's code: the others' bits.
    CHECK(access("/data/app/~~AAAA/io.example-BBBB/base.apk", 0 /* F_OK */) == 0);
    CHECK(access("/data/app/~~AAAA/io.example-BBBB/base.apk", r_ok) == 0);
    CHECK(access("/data/app/~~AAAA/io.example-BBBB/base.apk", w_ok) == -eacces);
    CHECK(access("/data/app/~~AAAA/io.example-BBBB/base.apk", w_ok, at_eaccess) == -eacces);
    CHECK(call(nr_faccessat, {at_fdcwd, address("/data/app/~~AAAA/io.example-BBBB/base.apk"), w_ok}) == -eacces);
    CHECK(access("/data/app/~~AAAA/io.example-BBBB", r_ok | x_ok) == 0);
    CHECK(access("/data/app/~~AAAA/io.example-BBBB", w_ok) == -eacces);
    // The app's own files: the owner's bits.
    CHECK(access("/data/user/0/io.example", r_ok | w_ok | x_ok) == 0);
    CHECK(access("/data/user/0/io.example/own", r_ok | w_ok) == 0);
    CHECK(access("/data/user/0/io.example/own", x_ok) == -eacces);
    // Root's: the others' bits.
    CHECK(access("/mnt/tool", r_ok | x_ok) == 0);
    CHECK(access("/mnt/tool", w_ok) == -eacces);
    CHECK(access("/mnt/shared", r_ok | w_ok) == 0);
    CHECK(access("/mnt/missing", 0) == -enoent);
    // A path relative to a directory descriptor has the owner stat reports for it as well.
    const auto code = open_guest(at_fdcwd, "/data/app/~~AAAA/io.example-BBBB", o_rdonly | o_directory);
    CHECK(call(nr_faccessat, {unsigned_of(code), address("base.apk"), r_ok}) == 0);
    CHECK(call(nr_faccessat, {unsigned_of(code), address("base.apk"), w_ok}) == -eacces);
    call(nr_close, {unsigned_of(code)});
}

// ADR-0039 decision 3, the calls that change a file or a folder's entries: the host's files all belong to the app's
// process, so the owners stat reports decide before the host, with Android 15's answers to an app (owner-rules-test
// on the emulator, 2026-10-05). The app's code, system's, refuses every change with EACCES; a hard link is EACCES, and
// EXDEV between mounts, as a rename is; the app's own folders take every other change.
constexpr std::uint64_t nr_linkat = 37, nr_truncate = 45, nr_fchmod = 52, nr_fchmodat = 53, nr_fchownat = 54,
                        nr_fchown = 55;
constexpr std::int64_t exdev = 18;
constexpr std::uint64_t unchanged_id = 0xffff'ffff;

/// The app's code with base.apk 0644 in a 0755 folder, as on Android 15, its data folder and its external files folder
/// in a scratch folder, each mounted on its own, as GuestStart mounts them. The host's own checks would let the app's
/// process write base.apk: only the owner rules refuse it (c5's 0444 aside).
struct Install : Scratch {
    const std::string code = "/data/app/~~AAAA/io.example-BBBB", apk = code + "/base.apk",
                      data = "/data/user/0/io.example", external = "/storage/emulated/0/Android/data/io.example";

    Install() {
        for (const char *folder : {"code", "data", "external"}) {
            CHECK(::mkdir((host + "/" + folder).c_str(), 0755) == 0);
        }
        CHECK(::close(::open((host + "/code/base.apk").c_str(), O_CREAT | O_WRONLY, 0644)) == 0);
        apkrt::vfs::mount(code, host + "/code");
        apkrt::vfs::mount(data, host + "/data");
        apkrt::vfs::mount(external, host + "/external");
    }

    /// Whether the scratch folder holds `name`, a path below it.
    bool has(const std::string &name) const {
        struct stat status;
        return ::lstat((host + "/" + name).c_str(), &status) == 0;
    }
};

std::uint64_t at(const std::string &path) {
    return address(path.c_str());
}

TEST(the_apps_code_refuses_every_change_as_on_android_15) {
    Install install;
    const auto &code = install.code, &apk = install.apk;
    const struct {
        std::int64_t seconds, nanoseconds;
    } times[2] = {{1'000'000'000, 0}, {1'000'000'000, 0}};
    CHECK(call(nr_fchmodat, {at_fdcwd, at(apk), 0644}) == -eacces);
    CHECK(call(nr_fchmodat, {at_fdcwd, at(code), 0775}) == -eacces);
    CHECK(call(nr_fchownat, {at_fdcwd, at(apk), 10100, 10100, 0}) == -eacces);
    CHECK(call(nr_utimensat, {at_fdcwd, at(apk), address(times), 0}) == -eacces);
    CHECK(call(nr_utimensat, {at_fdcwd, at(apk), 0, 0}) == -eacces);  // the current time: no write permission either
    CHECK(open_guest(at_fdcwd, apk.c_str(), o_wronly) == -eacces);
    CHECK(open_guest(at_fdcwd, apk.c_str(), o_rdonly | o_trunc) == -eacces);
    CHECK(call(nr_truncate, {at(apk), 0}) == -eacces);
    CHECK(open_guest(at_fdcwd, (code + "/new").c_str(), o_creat | o_wronly, 0600) == -eacces);
    CHECK(call(nr_mkdirat, {at_fdcwd, at(code + "/folder"), 0700}) == -eacces);
    CHECK(call(nr_symlinkat, {at(apk), at_fdcwd, at(code + "/symlink")}) == -eacces);
    CHECK(call(nr_renameat2, {at_fdcwd, at(apk), at_fdcwd, at(code + "/moved.apk"), 0}) == -eacces);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(apk), 0}) == -eacces);
    CHECK(call(nr_linkat, {at_fdcwd, at(apk), at_fdcwd, at(code + "/linked.apk"), 0}) == -eacces);
    // Out of the code's mount, EXDEV comes before any permission.
    CHECK(call(nr_renameat2, {at_fdcwd, at(apk), at_fdcwd, at(install.data + "/moved.apk"), 0}) == -exdev);
    CHECK(call(nr_linkat, {at_fdcwd, at(apk), at_fdcwd, at(install.data + "/linked.apk"), 0}) == -exdev);
    // Through a descriptor as well.
    const auto fd = open_guest(at_fdcwd, apk.c_str(), o_rdonly);
    CHECK(fd >= 0);
    CHECK(call(nr_fchmod, {unsigned_of(fd), 0644}) == -eacces);
    CHECK(call(nr_fchown, {unsigned_of(fd), 10100, 10100}) == -eacces);
    CHECK(call(nr_utimensat, {unsigned_of(fd), 0, 0, 0}) == -eacces);
    call(nr_close, {unsigned_of(fd)});
    // Linux's lookups answer first: ENOENT for a missing file, EEXIST for a present one.
    CHECK(call(nr_unlinkat, {at_fdcwd, at(code + "/missing"), 0}) == -enoent);
    CHECK(call(nr_fchmodat, {at_fdcwd, at(code + "/missing"), 0644}) == -enoent);
    CHECK(call(nr_mkdirat, {at_fdcwd, at(apk), 0700}) == -eexist);
    CHECK(call(nr_linkat, {at_fdcwd, at(code + "/missing"), at_fdcwd, at(code + "/x"), 0}) == -enoent);
    CHECK(call(nr_linkat, {at_fdcwd, at(apk), at_fdcwd, at(apk), 0}) == -eexist);
    // The host's file is as it was, and nothing came into the code folder or went out of it.
    struct stat status;
    CHECK(::stat((install.host + "/code/base.apk").c_str(), &status) == 0 && (status.st_mode & 07777) == 0644);
    for (const char *name : {"code/new", "code/folder", "code/symlink", "code/moved.apk", "code/linked.apk",
                             "data/moved.apk", "data/linked.apk"}) {
        CHECK(!install.has(name));
    }
}

TEST(the_apps_own_folders_take_every_change_but_a_hard_link) {
    Install install;
    const auto &data = install.data, &external = install.external;
    const auto own = data + "/own.txt";
    const struct {
        std::int64_t seconds, nanoseconds;
    } times[2] = {{1'000'000'000, 0}, {1'000'000'000, 0}};
    const auto fd = open_guest(at_fdcwd, own.c_str(), o_creat | o_rdwr, 0600);
    CHECK(fd >= 0);
    CHECK(call(nr_fchmod, {unsigned_of(fd), 0600}) == 0);
    call(nr_close, {unsigned_of(fd)});
    CHECK(call(nr_fchmodat, {at_fdcwd, at(own), 0640}) == 0);
    struct stat status;
    CHECK(::stat((install.host + "/data/own.txt").c_str(), &status) == 0 && (status.st_mode & 07777) == 0640);
    // chown: its own ids, which change nothing; another owner or group needs a capability the app lacks.
    CHECK(call(nr_fchownat, {at_fdcwd, at(own), 10100, 10100, 0}) == 0);
    CHECK(call(nr_fchownat, {at_fdcwd, at(own), unchanged_id, unchanged_id, 0}) == 0);
    CHECK(call(nr_fchownat, {at_fdcwd, at(own), 0, unchanged_id, 0}) == -eperm);
    CHECK(call(nr_fchownat, {at_fdcwd, at(own), unchanged_id, 1078, 0}) == -eperm);
    CHECK(call(nr_utimensat, {at_fdcwd, at(own), address(times), 0}) == 0);
    CHECK(::stat((install.host + "/data/own.txt").c_str(), &status) == 0 && status.st_mtime == 1'000'000'000);
    const auto again = open_guest(at_fdcwd, own.c_str(), o_wronly | o_trunc);
    CHECK(again >= 0);
    call(nr_close, {unsigned_of(again)});
    CHECK(call(nr_truncate, {at(own), 0}) == 0);
    CHECK(call(nr_symlinkat, {at(own), at_fdcwd, at(data + "/symlink")}) == 0);
    CHECK(call(nr_mkdirat, {at_fdcwd, at(data + "/folder"), 0700}) == 0);
    CHECK(call(nr_renameat2, {at_fdcwd, at(own), at_fdcwd, at(data + "/moved.txt"), 0}) == 0);
    // A hard link: EACCES within the folder, EXDEV to another mount, as for a rename.
    CHECK(call(nr_linkat, {at_fdcwd, at(data + "/moved.txt"), at_fdcwd, at(data + "/linked.txt"), 0}) == -eacces);
    CHECK(call(nr_linkat, {at_fdcwd, at(data + "/moved.txt"), at_fdcwd, at(external + "/linked.txt"), 0}) == -exdev);
    CHECK(call(nr_renameat2, {at_fdcwd, at(data + "/moved.txt"), at_fdcwd, at(external + "/moved.txt"), 0}) == -exdev);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(data + "/moved.txt"), 0}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(data + "/symlink"), 0}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(data + "/folder"), at_removedir}) == 0);
    CHECK(!install.has("data/linked.txt") && !install.has("external/linked.txt") && !install.has("data/moved.txt"));
    // The external files folder, the app's with the group ext_data_rw.
    const auto outside = external + "/own.txt";
    const auto created = open_guest(at_fdcwd, outside.c_str(), o_creat | o_wronly, 0660);
    CHECK(created >= 0);
    call(nr_close, {unsigned_of(created)});
    CHECK(call(nr_fchmodat, {at_fdcwd, at(outside), 0600}) == 0);
    CHECK(call(nr_renameat2, {at_fdcwd, at(outside), at_fdcwd, at(external + "/moved.txt"), 0}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(external + "/moved.txt"), 0}) == 0);
}

// On a host whose file system ignores case (the Mac's, so the Simulator's), /data/APP/... below a mount of /data is
// the same file as /data/app/...: the owners go by the folders without regard to case, so it is system's as well.
TEST(the_apps_code_is_systems_in_any_case) {
    Scratch scratch;
    const auto code = scratch.host + "/data/app/~~AAAA/io.example-BBBB";
    CHECK(std::system(("mkdir -p '" + code + "'").c_str()) == 0);
    CHECK(::close(::open((code + "/base.apk").c_str(), O_CREAT | O_WRONLY, 0644)) == 0);
    apkrt::vfs::mount("/data", scratch.host + "/data");  // as apk-run's harness mounts /data
    const bool ignores_case = ::pathconf(scratch.host.c_str(), _PC_CASE_SENSITIVE) == 0;
    const auto other_case = std::string("/data/APP/~~AAAA/io.example-BBBB/base.apk");
    CHECK(call(nr_fchmodat, {at_fdcwd, at(other_case), 0666}) == (ignores_case ? -eacces : -enoent));
    LinuxStat status{};
    if (ignores_case) {
        CHECK(call(nr_newfstatat, {at_fdcwd, at(other_case), address(&status), 0}) == 0 && status.uid == 1000);
    }
    struct stat host{};
    CHECK(::stat((code + "/base.apk").c_str(), &host) == 0 && (host.st_mode & 07777) == 0644);
}

// /data/resource-cache, where Android 15's SystemUI and idmap2 leave Monet's overlays (ADR-0043 f), is system's: the
// folder 0771 and its files 0644, so the app reads and stats them and makes, removes, renames and writes nothing
// there, with Android's EACCES. Its mount comes after /data's, as the Mac's harness has it.
TEST(the_resource_cache_is_systems) {
    Scratch scratch;
    const auto cache = scratch.host + "/resource-cache";
    CHECK(std::system(("mkdir -p '" + cache + "' '" + scratch.host + "/data'").c_str()) == 0);
    const int made = ::open((cache + "/overlay.frro").c_str(), O_CREAT | O_WRONLY, 0644);
    CHECK(made >= 0 && ::write(made, "FRRO", 4) == 4);
    ::close(made);
    apkrt::vfs::mount("/data", scratch.host + "/data");
    apkrt::vfs::mount("/data/resource-cache", cache);
    const std::string folder = "/data/resource-cache", overlay = folder + "/overlay.frro";
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, at(folder), address(&status), 0}) == 0);
    CHECK(status.uid == 1000 && status.gid == 1000 && (status.mode & 07777) == 0771);
    CHECK(call(nr_newfstatat, {at_fdcwd, at(overlay), address(&status), 0}) == 0);
    CHECK(status.uid == 1000 && status.gid == 1000 && (status.mode & 07777) == 0644);
    const auto file = open_guest(at_fdcwd, overlay.c_str(), o_rdonly);
    CHECK(file >= 0 && read_text(file, 8) == "FRRO");
    call(nr_close, {unsigned_of(file)});
    CHECK(open_guest(at_fdcwd, overlay.c_str(), o_wronly) == -eacces);
    CHECK(open_guest(at_fdcwd, (folder + "/new.frro").c_str(), o_creat | o_wronly, 0600) == -eacces);
    CHECK(call(nr_mkdirat, {at_fdcwd, at(folder + "/new"), 0700}) == -eacces);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(overlay), 0}) == -eacces);
    CHECK(call(nr_renameat2, {at_fdcwd, at(overlay), at_fdcwd, at(folder + "/moved.frro"), 0}) == -eacces);
    CHECK(call(nr_fchmodat, {at_fdcwd, at(overlay), 0666}) == -eacces);
    struct stat host{};
    CHECK(::stat((cache + "/overlay.frro").c_str(), &host) == 0 && (host.st_mode & 07777) == 0644 && host.st_size == 4);
    CHECK(::access((cache + "/new.frro").c_str(), F_OK) != 0 && ::access((cache + "/new").c_str(), F_OK) != 0);
}

// ADR-0052: a guest's symlinks stay inside the guest's file system. Every host call starts at its mount's host folder
// with Darwin's RESOLVE_BENEATH, so a link or ".." that would leave the mount answers EACCES, whatever the host has
// there; a link inside its mount still works.
TEST(a_link_out_of_its_mount_leads_nowhere) {
    Install install;
    const auto &data = install.data;
    // A relative text, which the host would follow from the data folder's host folder into the code's.
    CHECK(call(nr_symlinkat, {at("../code"), at_fdcwd, at(data + "/escape")}) == 0);
    CHECK(call(nr_fchmodat, {at_fdcwd, at(data + "/escape/base.apk"), 0666}) == -eacces);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(data + "/escape/base.apk"), 0}) == -eacces);
    CHECK(open_guest(at_fdcwd, (data + "/escape/base.apk").c_str(), o_rdonly) == -eacces);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, at(data + "/escape/base.apk"), address(&status), 0}) == -eacces);
    CHECK(call(nr_faccessat, {at_fdcwd, at(data + "/escape/base.apk"), 0}) == -eacces);
    // An absolute text, which the host would read as a host path.
    CHECK(call(nr_symlinkat, {at("/etc"), at_fdcwd, at(data + "/host-etc")}) == 0);
    CHECK(open_guest(at_fdcwd, (data + "/host-etc/hosts").c_str(), o_rdonly) == -eacces);
    // A directory descriptor leads no further: ".." out of the mount is a guest path no mount covers.
    const auto folder = open_guest(at_fdcwd, data.c_str(), o_rdonly | o_directory);
    CHECK(folder >= 0);
    CHECK(open_guest(unsigned_of(folder), "../code/base.apk", o_rdonly) == -enoent);
    CHECK(open_guest(unsigned_of(folder), "escape/base.apk", o_rdonly) == -eacces);
    call(nr_close, {unsigned_of(folder)});
    // A rename, the times and a new folder through it: refused as well.
    const auto own = open_guest(at_fdcwd, (data + "/own.txt").c_str(), o_creat | o_wronly, 0600);
    CHECK(own >= 0);
    call(nr_close, {unsigned_of(own)});
    CHECK(call(nr_renameat2, {at_fdcwd, at(data + "/own.txt"), at_fdcwd, at(data + "/escape/planted"), 0}) == -eacces);
    CHECK(call(nr_utimensat, {at_fdcwd, at(data + "/escape/base.apk"), 0, 0}) == -eacces);
    // Into another of the app's own mounts, where Linux would follow the link (ADR-0052's part 2): EACCES all the same.
    CHECK(call(nr_symlinkat, {at("../external"), at_fdcwd, at(data + "/to-external")}) == 0);
    CHECK(call(nr_mkdirat, {at_fdcwd, at(data + "/to-external/folder"), 0700}) == -eacces);
    CHECK(!install.has("code/planted") && !install.has("external/folder"));
    // The links themselves: their text, lstat, and unlink.
    char text[64] = {};
    CHECK(call(nr_readlinkat, {at_fdcwd, at(data + "/escape"), address(text), sizeof text}) == 7);
    CHECK(call(nr_newfstatat, {at_fdcwd, at(data + "/escape"), address(&status), at_symlink_nofollow}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(data + "/escape"), 0}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(data + "/host-etc"), 0}) == 0);
    // A link inside its mount.
    CHECK(call(nr_mkdirat, {at_fdcwd, at(data + "/folder"), 0700}) == 0);
    const auto file = open_guest(at_fdcwd, (data + "/folder/file").c_str(), o_creat | o_wronly, 0600);
    CHECK(file >= 0 && write_text(file, "inside") == 6);
    call(nr_close, {unsigned_of(file)});
    CHECK(call(nr_symlinkat, {at("folder/file"), at_fdcwd, at(data + "/inside")}) == 0);
    const auto again = open_guest(at_fdcwd, (data + "/inside").c_str(), o_rdonly);
    CHECK(again >= 0 && read_text(again, 16) == "inside");
    call(nr_close, {unsigned_of(again)});
    struct stat host{};
    CHECK(::stat((install.host + "/code/base.apk").c_str(), &host) == 0 && (host.st_mode & 07777) == 0644);
}

/// /data as one mount, as GuestStart's guest /data and apk-run's harness have it: the code folder with a JNI library,
/// /data/local/tmp, ART's dalvik-cache, and the app's data folder, which is a mount of its own.
struct WholeData : Scratch {
    const std::string code = "/data/app/~~AAAA/io.example-BBBB", data = "/data/user/0/io.example";

    WholeData() {
        CHECK(std::system(("cd '" + host + "' && mkdir -p data/app/~~AAAA/io.example-BBBB/lib/arm64 data/local/tmp "
                           "data/dalvik-cache/arm64 data/data data/user/0/io.example && touch "
                           "data/app/~~AAAA/io.example-BBBB/base.apk "
                           "data/app/~~AAAA/io.example-BBBB/lib/arm64/libfoo.so")
                              .c_str()) == 0);
        apkrt::vfs::reset();
        apkrt::vfs::mount("/data", host + "/data");
        apkrt::vfs::mount(data, host + "/data/user/0/io.example");
    }
};

// The lead's reviewer: inside one mount, a link planted where the app may write and pointing into the code folder
// must not let the app change the code. /data's own folders are system's, as on Android 15, so nothing is planted
// there, and the owner checks look at the folder or file the host call will touch, not at the path as written.
TEST(a_link_inside_one_mount_does_not_open_the_code) {
    WholeData whole;
    const auto &code = whole.code;
    // /data's own folders: system's, so no entry is made, renamed or removed there.
    CHECK(call(nr_symlinkat, {at("app/~~AAAA/io.example-BBBB/lib/arm64"), at_fdcwd, at("/data/evil")}) == -eacces);
    CHECK(call(nr_renameat2, {at_fdcwd, at("/data/app"), at_fdcwd, at("/data/appx"), 0}) == -eacces);
    CHECK(call(nr_mkdirat, {at_fdcwd, at("/data/user/0/other"), 0700}) == -eacces);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, at("/data"), address(&status), 0}) == 0 && status.uid == 1000);
    // A link in a folder the app may write, into the code folder.
    const std::string evil = "/data/local/tmp/evil";
    CHECK(call(nr_symlinkat, {at("../../app/~~AAAA/io.example-BBBB/lib/arm64"), at_fdcwd, at(evil)}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, at(evil + "/libfoo.so"), 0}) == -eacces);
    CHECK(call(nr_renameat2, {at_fdcwd, at(evil + "/libfoo.so"), at_fdcwd, at("/data/local/tmp/moved.so"), 0}) ==
          -eacces);
    CHECK(open_guest(at_fdcwd, (evil + "/new.so").c_str(), o_creat | o_wronly, 0600) == -eacces);
    CHECK(call(nr_mkdirat, {at_fdcwd, at(evil + "/folder"), 0700}) == -eacces);
    CHECK(call(nr_fchmodat, {at_fdcwd, at(evil + "/libfoo.so"), 0666}) == -eacces);
    CHECK(open_guest(at_fdcwd, (evil + "/libfoo.so").c_str(), o_wronly) == -eacces);
    // The link itself is the app's to remove.
    CHECK(call(nr_unlinkat, {at_fdcwd, at(evil), 0}) == 0);
    struct stat host{};
    CHECK(::stat((whole.host + "/data" + code.substr(5) + "/lib/arm64/libfoo.so").c_str(), &host) == 0);
}

// ADR-0052's change: iOS's sandbox lets an app open /dev/urandom but not read /dev (the iPhone's device suite,
// 2026-10-06), so a mounted file, a device or base.apk, is reached by its host path from the mount table, with no
// folder held open and no guest text after it: nothing lies below a file (ENOTDIR), and its folder is not the app's
// to change. A folder of mode 0100, which the app may search but not read, stands for /dev; mounted itself, it stays
// shut, as a mounted folder needs its descriptor for RESOLVE_BENEATH.
TEST(a_mounted_file_needs_no_folder_the_app_may_read) {
    Scratch scratch;
    const std::string sealed = scratch.host + "/sealed";
    CHECK(::mkdir(sealed.c_str(), 0700) == 0);
    const int device = ::open((sealed + "/device").c_str(), O_CREAT | O_WRONLY, 0644);
    CHECK(device >= 0 && ::write(device, "random", 6) == 6 && ::close(device) == 0);
    CHECK(::chmod(sealed.c_str(), 0100) == 0);
    apkrt::vfs::mount("/dev/sealed", sealed + "/device");
    apkrt::vfs::mount("/sealed", sealed);
    // What bionic's arc4random and an app do with a device.
    CHECK(call(nr_faccessat, {at_fdcwd, at("/dev/sealed"), 4 /* R_OK */}) == 0);
    const auto fd = open_guest(at_fdcwd, "/dev/sealed", o_rdonly);
    CHECK(fd >= 0 && read_text(fd, 16) == "random");
    call(nr_close, {unsigned_of(fd)});
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, at("/dev/sealed"), address(&status), 0}) == 0 && status.size == 6);
    std::int64_t file_system[15] = {};  // Linux's struct statfs
    CHECK(call(nr_statfs, {at("/dev/sealed"), address(file_system)}) == 0);
    // Nothing lies below it, and neither it nor its folder is the app's to change.
    CHECK(open_guest(at_fdcwd, "/dev/sealed/x", o_rdonly) == -enotdir);
    CHECK(call(nr_faccessat, {at_fdcwd, at("/dev/sealed/"), 0}) == -enotdir);
    CHECK(call(nr_mkdirat, {at_fdcwd, at("/dev/sealed/x"), 0700}) == -enotdir);
    CHECK(call(nr_chdir, {at("/dev/sealed")}) == -enotdir);
    CHECK(call(nr_mkdirat, {at_fdcwd, at("/dev/sealed"), 0700}) == -eexist);
    char text[16] = {};
    CHECK(call(nr_readlinkat, {at_fdcwd, at("/dev/sealed"), address(text), sizeof text}) == -einval);
    CHECK(call(nr_fchmodat, {at_fdcwd, at("/dev/sealed"), 0666}) == -eacces);
    CHECK(call(nr_unlinkat, {at_fdcwd, at("/dev/sealed"), 0}) == -eacces);
    CHECK(call(nr_renameat2, {at_fdcwd, at("/data/local/tmp/missing"), at_fdcwd, at("/dev/sealed"), 0}) == -enoent);
    // The folder mount stays shut.
    CHECK(call(nr_faccessat, {at_fdcwd, at("/sealed/device"), 0}) == -enoent);
    CHECK(::chmod(sealed.c_str(), 0700) == 0);
}

// ADR-0052's later step: Android 15 mounts the system's partitions read-only, so every change there answers EROFS, in
// Linux's order: after EEXIST for a name that is there, after EXDEV across mounts and after EISDIR for a folder opened
// for writing; before ENOENT where Linux checks the mount before the last name (unlink, rmdir, rename); before any
// permission (owner-rules-test's system and apex lines on the emulator, 2026-10-06).
struct System : Install {
    const std::string file = "/system/etc/public.libraries.txt", apex_file = "/apex/com.android.art/javalib/core-oj.jar";

    System() {
        CHECK(std::system(("mkdir -p '" + host + "/system/etc' '" + host + "/apex/com.android.art/javalib'").c_str()) ==
              0);
        for (const char *name : {"/system/etc/public.libraries.txt", "/apex/com.android.art/javalib/core-oj.jar"}) {
            CHECK(::close(::open((host + name).c_str(), O_CREAT | O_WRONLY, 0644)) == 0);
        }
        apkrt::vfs::mount("/system", host + "/system");
        apkrt::vfs::mount("/apex", host + "/apex");
    }
};

TEST(the_systems_partitions_answer_erofs_as_on_android_15) {
    constexpr std::int64_t erofs = 30;
    constexpr std::uint64_t w_ok = 2, r_ok = 4, utime_omit = (1UL << 30) - 2;
    System system;
    const struct {
        std::int64_t seconds, nanoseconds;
    } times[2] = {{1'000'000'000, 0}, {1'000'000'000, 0}},
      omitted[2] = {{0, static_cast<std::int64_t>(utime_omit)}, {0, static_cast<std::int64_t>(utime_omit)}};
    for (const auto &[folder, file] : {std::pair<std::string, std::string>{"/system/etc", system.file},
                                       std::pair<std::string, std::string>{"/apex/com.android.art/javalib",
                                                                           system.apex_file}}) {
        CHECK(open_guest(at_fdcwd, file.c_str(), o_wronly) == -erofs);
        CHECK(open_guest(at_fdcwd, file.c_str(), o_rdonly | o_trunc) == -erofs);
        CHECK(open_guest(at_fdcwd, (folder + "/new").c_str(), o_creat | o_wronly, 0600) == -erofs);
        CHECK(open_guest(at_fdcwd, file.c_str(), o_creat | o_excl | o_wronly, 0600) == -eexist);
        CHECK(open_guest(at_fdcwd, folder.c_str(), o_wronly) == -eisdir);
        CHECK(open_guest(at_fdcwd, (folder + "/missing/new").c_str(), o_creat | o_wronly, 0600) == -enoent);
        const auto created = open_guest(at_fdcwd, file.c_str(), o_creat | o_rdonly, 0600);  // reads only: no write
        CHECK(created >= 0);
        call(nr_close, {unsigned_of(created)});
        CHECK(call(nr_faccessat, {at_fdcwd, at(file), w_ok, 0}) == -erofs);
        CHECK(call(nr_faccessat, {at_fdcwd, at(file), r_ok, 0}) == 0);
        CHECK(call(nr_fchmodat, {at_fdcwd, at(file), 0644}) == -erofs);
        CHECK(call(nr_fchmodat, {at_fdcwd, at(folder + "/missing"), 0644}) == -enoent);
        CHECK(call(nr_fchownat, {at_fdcwd, at(file), 10100, 10100, 0}) == -erofs);
        CHECK(call(nr_utimensat, {at_fdcwd, at(file), address(times), 0}) == -erofs);
        CHECK(call(nr_utimensat, {at_fdcwd, at(file), address(omitted), 0}) == 0);  // nothing to change
        CHECK(call(nr_truncate, {at(file), 0}) == -erofs);
        CHECK(call(nr_truncate, {at(folder), 0}) == -eisdir);
        CHECK(call(nr_mkdirat, {at_fdcwd, at(folder + "/new"), 0700}) == -erofs);
        CHECK(call(nr_mkdirat, {at_fdcwd, at(folder), 0700}) == -eexist);
        CHECK(call(nr_mkdirat, {at_fdcwd, at(folder + "/missing/new"), 0700}) == -enoent);
        CHECK(call(nr_symlinkat, {at(file), at_fdcwd, at(folder + "/symlink")}) == -erofs);
        CHECK(call(nr_symlinkat, {at(file), at_fdcwd, at(file)}) == -eexist);
        CHECK(call(nr_linkat, {at_fdcwd, at(file), at_fdcwd, at(folder + "/link"), 0}) == -erofs);
        CHECK(call(nr_linkat, {at_fdcwd, at(file), at_fdcwd, at(system.data + "/link"), 0}) == -exdev);
        CHECK(call(nr_linkat, {at_fdcwd, at(folder + "/missing"), at_fdcwd, at(folder + "/link"), 0}) == -enoent);
        CHECK(call(nr_renameat2, {at_fdcwd, at(folder + "/missing"), at_fdcwd, at(folder + "/moved"), 0}) == -erofs);
        CHECK(call(nr_renameat2, {at_fdcwd, at(file), at_fdcwd, at(system.data + "/moved"), 0}) == -exdev);
        CHECK(call(nr_unlinkat, {at_fdcwd, at(folder + "/missing"), 0}) == -erofs);
        CHECK(call(nr_unlinkat, {at_fdcwd, at(file), 0}) == -erofs);
        CHECK(call(nr_unlinkat, {at_fdcwd, at(folder), at_removedir}) == -erofs);
        // Through a descriptor as well.
        const auto fd = open_guest(at_fdcwd, file.c_str(), o_rdonly);
        CHECK(fd >= 0);
        CHECK(call(nr_fchmod, {unsigned_of(fd), 0644}) == -erofs);
        CHECK(call(nr_fchown, {unsigned_of(fd), 10100, 10100}) == -erofs);
        CHECK(call(nr_utimensat, {unsigned_of(fd), 0, 0, 0}) == -erofs);
        call(nr_close, {unsigned_of(fd)});
        // statfs says so, as Linux's ST_RDONLY.
        struct {
            std::int64_t type, block_size, blocks, free, available, files, free_files;
            std::int32_t fsid[2];
            std::int64_t name_length, fragment_size, flags, spare[4];
        } linux_statfs{};
        CHECK(call(nr_statfs, {at(folder), address(&linux_statfs)}) == 0 && (linux_statfs.flags & 1) != 0);
        constexpr std::uint64_t nr_fstatfs = 44;
        linux_statfs.flags = 0;
        const auto opened = open_guest(at_fdcwd, folder.c_str(), o_rdonly | o_directory);
        CHECK(call(nr_fstatfs, {unsigned_of(opened), address(&linux_statfs)}) == 0 && (linux_statfs.flags & 1) != 0);
        call(nr_close, {unsigned_of(opened)});
    }
    // Between two partitions, as fs/namei.c orders it: link takes the new folder's mount before EXDEV (filename_create),
    // rename checks EXDEV first (do_renameat2).
    CHECK(call(nr_linkat, {at_fdcwd, at(system.file), at_fdcwd, at("/apex/com.android.art/javalib/link"), 0}) == -erofs);
    CHECK(call(nr_renameat2, {at_fdcwd, at(system.file), at_fdcwd, at("/apex/com.android.art/javalib/moved"), 0}) ==
          -exdev);
    // /apex itself is not an APEX: root's folder refuses with EACCES. /data stays writable.
    CHECK(call(nr_mkdirat, {at_fdcwd, at("/apex/new"), 0700}) == -eacces);
    struct {
        std::int64_t type, block_size, blocks, free, available, files, free_files;
        std::int32_t fsid[2];
        std::int64_t name_length, fragment_size, flags, spare[4];
    } data_statfs{};
    CHECK(call(nr_statfs, {at(system.data), address(&data_statfs)}) == 0 && (data_statfs.flags & 1) == 0);
    // Nothing came into the partitions, and their files are as they were.
    for (const char *name : {"system/etc/new", "system/etc/symlink", "system/etc/link", "system/etc/moved",
                             "apex/com.android.art/javalib/new", "apex/new", "data/link", "data/moved"}) {
        CHECK(!system.has(name));
    }
    struct stat status;
    CHECK(::stat((system.host + "/system/etc/public.libraries.txt").c_str(), &status) == 0 &&
          (status.st_mode & 07777) == 0644 && status.st_mtime != 1'000'000'000);
}

TEST(relative_paths_start_at_the_working_directory_or_a_directory_fd) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "b.txt", o_creat | o_rdwr, 0600);
    CHECK(fd >= 0 && write_text(fd, "b") == 1);
    CHECK(scratch.contents("b.txt") == "b");
    CHECK(call(nr_mkdirat, {at_fdcwd, address("sub"), 0755}) == 0);
    const auto directory = open_guest(at_fdcwd, "./sub/../sub", o_rdonly | o_directory);
    CHECK(directory >= 0);
    const auto inner = open_guest(unsigned_of(directory), "c.txt", o_creat | o_wronly, 0600);
    CHECK(inner >= 0 && write_text(inner, "c") == 1);
    CHECK(scratch.contents("sub/c.txt") == "c");
    for (const auto open_fd : {fd, directory, inner}) call(nr_close, {unsigned_of(open_fd)});
}

TEST(open_reports_linux_errors) {
    Scratch scratch;
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "file", o_creat | o_wronly, 0600))});
    call(nr_mkdirat, {at_fdcwd, address("directory"), 0700});
    CHECK(open_guest(at_fdcwd, "missing", o_rdonly) == -enoent);
    CHECK(open_guest(at_fdcwd, "file", o_creat | o_excl | o_wronly, 0600) == -eexist);
    CHECK(open_guest(at_fdcwd, "file", o_rdonly | o_directory) == -enotdir);
    CHECK(open_guest(at_fdcwd, "file/below", o_rdonly) == -enotdir);
    CHECK(open_guest(at_fdcwd, "directory", o_wronly) == -eisdir);
    CHECK(open_guest(at_fdcwd, "/data/local/elsewhere", o_rdonly) == -enoent);
    CHECK(call(nr_mkdirat, {at_fdcwd, address("directory"), 0700}) == -eexist);
}

TEST(open_translates_the_linux_flags) {
    Scratch scratch;
    const auto first = open_guest(at_fdcwd, "log", o_creat | o_wronly, 0600);
    CHECK(write_text(first, "ab") == 2);
    const auto appending = open_guest(at_fdcwd, "log", o_wronly | o_append | o_cloexec);
    CHECK(write_text(first, "") == 0);
    CHECK(call(nr_lseek, {unsigned_of(appending), 0, 0}) == 0);
    CHECK(write_text(appending, "cd") == 2);  // at the end all the same
    CHECK(scratch.contents("log") == "abcd");
    CHECK(call(nr_fcntl, {unsigned_of(appending), 1 /* F_GETFD */}) == 1);
    CHECK(call(nr_fcntl, {unsigned_of(first), 1 /* F_GETFD */}) == 0);
    CHECK(call(nr_fcntl, {unsigned_of(appending), 3 /* F_GETFL */}) == static_cast<std::int64_t>(o_wronly | o_append));
    const auto truncating = open_guest(at_fdcwd, "log", o_wronly | o_trunc);
    CHECK(scratch.contents("log").empty());
    for (const auto fd : {first, appending, truncating}) call(nr_close, {unsigned_of(fd)});
}

TEST(offsets_and_vectors_work_like_linux) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "digits", o_creat | o_rdwr, 0600);
    CHECK(write_text(fd, "0123456789") == 10);
    CHECK(call(nr_lseek, {unsigned_of(fd), 3, 0 /* SEEK_SET */}) == 3);
    CHECK(read_text(fd, 2) == "34");
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 1 /* SEEK_CUR */}) == 5);
    CHECK(call(nr_lseek, {unsigned_of(fd), unsigned_of(-1), 2 /* SEEK_END */}) == 9);
    CHECK(call(nr_lseek, {unsigned_of(fd), unsigned_of(-20), 1}) == -einval);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 3 /* SEEK_DATA */}) == 0);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 4 /* SEEK_HOLE */}) == 10);

    char three[3];
    CHECK(call(nr_pread64, {unsigned_of(fd), address(three), 3, 7}) == 3 && std::memcmp(three, "789", 3) == 0);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 1}) == 10);  // pread leaves the offset where SEEK_HOLE put it
    CHECK(call(nr_pwrite64, {unsigned_of(fd), address("xy"), 2, 0}) == 2);
    CHECK(scratch.contents("digits") == "xy23456789");

    char first[4] = {}, second[6] = {};
    const iovec parts[] = {{first, sizeof first}, {second, sizeof second}};
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 0}) == 0);
    CHECK(call(nr_readv, {unsigned_of(fd), address(parts), 2}) == 10);
    CHECK(std::memcmp(first, "xy23", 4) == 0 && std::memcmp(second, "456789", 6) == 0);
    const iovec out[] = {{const_cast<char *>("A"), 1}, {const_cast<char *>("BC"), 2}};
    CHECK(call(nr_writev, {unsigned_of(fd), address(out), 2}) == 3);
    CHECK(scratch.contents("digits") == "xy23456789ABC");

    int fds[2];
    guest_pipe(fds);
    CHECK(call(nr_lseek, {static_cast<std::uint64_t>(fds[0]), 0, 0}) == -espipe);
    call(nr_close, {static_cast<std::uint64_t>(fds[0])});
    call(nr_close, {static_cast<std::uint64_t>(fds[1])});
    call(nr_close, {unsigned_of(fd)});
}

TEST(size_changes_and_syncs_show_in_stat) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "sized", o_creat | o_rdwr, 0644);
    CHECK(call(nr_ftruncate, {unsigned_of(fd), 100}) == 0);
    CHECK(call(nr_fsync, {unsigned_of(fd)}) == 0);
    CHECK(call(nr_fdatasync, {unsigned_of(fd)}) == 0);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address("sized"), address(&status), 0}) == 0);
    CHECK(status.size == 100 && (status.mode & 0170777) == 0100644);
    CHECK(call(nr_newfstatat, {at_fdcwd, address("/data/local/tmp"), address(&status), 0}) == 0);
    CHECK((status.mode & 0170000) == 0040000);  // S_IFDIR
    CHECK(call(nr_newfstatat, {at_fdcwd, address("nothing"), address(&status), 0}) == -enoent);
    call(nr_close, {unsigned_of(fd)});
}

struct DirectoryEntry {
    std::string name;
    unsigned type;
};

/// The entries getdents64 fills into `buffer`: struct linux_dirent64, 8-byte aligned records.
std::vector<DirectoryEntry> entries_of(const char *buffer, std::int64_t size) {
    std::vector<DirectoryEntry> entries;
    for (std::int64_t offset = 0; offset < size;) {
        std::uint16_t length;
        std::memcpy(&length, buffer + offset + 16, sizeof length);
        CHECK(length % 8 == 0 && length >= 24);
        entries.push_back({buffer + offset + 19, static_cast<unsigned char>(buffer[offset + 18])});
        offset += length;
    }
    return entries;
}

TEST(directories_list_their_entries) {
    Scratch scratch;
    call(nr_mkdirat, {at_fdcwd, address("d"), 0755});
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "d/x", o_creat | o_wronly, 0600))});
    call(nr_mkdirat, {at_fdcwd, address("d/y"), 0755});
    const auto fd = open_guest(at_fdcwd, "d", o_rdonly | o_directory | o_cloexec);
    CHECK(fd >= 0);

    alignas(8) char buffer[4096];
    const auto size = call(nr_getdents64, {unsigned_of(fd), address(buffer), sizeof buffer});
    CHECK(size > 0);
    std::set<std::string> names;
    for (const auto &entry : entries_of(buffer, size)) {
        names.insert(entry.name);
        if (entry.name == "x") CHECK(entry.type == 8);  // DT_REG
        if (entry.name == "y" || entry.name == "." || entry.name == "..") CHECK(entry.type == 4);  // DT_DIR
    }
    CHECK((names == std::set<std::string>{".", "..", "x", "y"}));
    CHECK(call(nr_getdents64, {unsigned_of(fd), address(buffer), sizeof buffer}) == 0);  // the end

    // Back to the start, then one entry at a time: a buffer for one record gets one record.
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 0}) == 0);
    std::set<std::string> again;
    for (int round = 0; round < 10; ++round) {
        const auto one = call(nr_getdents64, {unsigned_of(fd), address(buffer), 24});
        if (one <= 0) break;
        const auto list = entries_of(buffer, one);
        CHECK(list.size() == 1);
        again.insert(list.front().name);
    }
    CHECK(again == names);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 0}) == 0);
    CHECK(call(nr_getdents64, {unsigned_of(fd), address(buffer), 8}) == -einval);  // too small for any record
    CHECK(call(nr_getdents64, {unsigned_of(open_guest(at_fdcwd, "d/x", o_rdonly)), address(buffer), 4096}) == -enotdir);
    call(nr_close, {unsigned_of(fd)});
}

TEST(unlink_rmdir_and_rename_answer_like_linux) {
    Scratch scratch;
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "one", o_creat | o_wronly, 0600))});
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "two", o_creat | o_wronly, 0600))});
    call(nr_mkdirat, {at_fdcwd, address("full"), 0755});
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "full/inside", o_creat | o_wronly, 0600))});
    call(nr_mkdirat, {at_fdcwd, address("empty"), 0755});

    CHECK(call(nr_unlinkat, {at_fdcwd, address("empty"), 0}) == -eisdir);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("full"), at_removedir}) == -enotempty);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("one"), at_removedir}) == -enotdir);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("empty"), at_removedir}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("missing"), 0}) == -enoent);

    CHECK(call(nr_renameat2, {at_fdcwd, address("one"), at_fdcwd, address("three"), 0}) == 0);
    CHECK(open_guest(at_fdcwd, "one", o_rdonly) == -enoent);
    CHECK(call(nr_renameat2, {at_fdcwd, address("three"), at_fdcwd, address("two"), 1 /* RENAME_NOREPLACE */}) == -eexist);
    CHECK(call(nr_renameat2, {at_fdcwd, address("three"), at_fdcwd, address("two"), 0}) == 0);  // replaces
    CHECK(call(nr_unlinkat, {at_fdcwd, address("two"), 0}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("three"), 0}) == -enoent);
}

TEST(the_working_directory_follows_chdir) {
    Scratch scratch;
    char buffer[256];
    CHECK(call(nr_getcwd, {address(buffer), sizeof buffer}) == 16);  // with the terminating NUL
    CHECK(std::string(buffer) == "/data/local/tmp");
    CHECK(call(nr_getcwd, {address(buffer), 10}) == -erange);
    call(nr_mkdirat, {at_fdcwd, address("sub"), 0755});
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "plain", o_creat | o_wronly, 0600))});

    CHECK(call(nr_chdir, {address("sub")}) == 0);
    CHECK(call(nr_getcwd, {address(buffer), sizeof buffer}) > 0 && std::string(buffer) == "/data/local/tmp/sub");
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "here", o_creat | o_wronly, 0600))});
    CHECK(scratch.contents("sub/here").empty() && ::access((scratch.host + "/sub/here").c_str(), F_OK) == 0);
    CHECK(call(nr_chdir, {address("..")}) == 0);
    CHECK(call(nr_getcwd, {address(buffer), sizeof buffer}) > 0 && std::string(buffer) == "/data/local/tmp");
    CHECK(call(nr_chdir, {address("plain")}) == -enotdir);
    CHECK(call(nr_chdir, {address("nowhere")}) == -enoent);

    const auto fd = open_guest(at_fdcwd, "sub", o_rdonly | o_directory);
    CHECK(call(nr_fchdir, {unsigned_of(fd)}) == 0);
    CHECK(call(nr_getcwd, {address(buffer), sizeof buffer}) > 0 && std::string(buffer) == "/data/local/tmp/sub");
    call(nr_close, {unsigned_of(fd)});
}

TEST(descriptors_duplicate_and_pipes_connect) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "dup", o_creat | o_rdwr, 0600);
    const auto copy = call(nr_dup, {unsigned_of(fd)});
    CHECK(copy >= 0 && copy != fd);
    CHECK(write_text(copy, "via copy") == 8);
    CHECK(call(nr_dup3, {unsigned_of(fd), 60, o_cloexec}) == 60);
    CHECK(call(nr_fcntl, {60, 1 /* F_GETFD */}) == 1);
    CHECK(call(nr_dup3, {unsigned_of(fd), unsigned_of(fd), 0}) == -einval);
    for (const auto open_fd : {fd, copy, std::int64_t{60}}) call(nr_close, {unsigned_of(open_fd)});

    int fds[2] = {-1, -1};
    CHECK(call(nr_pipe2, {address(fds), o_cloexec | o_nonblock}) == 0);
    CHECK(call(nr_fcntl, {static_cast<std::uint64_t>(fds[0]), 1 /* F_GETFD */}) == 1);
    CHECK((call(nr_fcntl, {static_cast<std::uint64_t>(fds[1]), 3 /* F_GETFL */}) & static_cast<std::int64_t>(o_nonblock)) != 0);
    CHECK(read_text(fds[0], 4) == "error 11");  // EAGAIN on an empty non-blocking pipe
    CHECK(write_text(fds[1], "abc") == 3);
    int available = -1;
    CHECK(call(nr_ioctl, {static_cast<std::uint64_t>(fds[0]), 0x541b /* FIONREAD */, address(&available)}) == 0);
    CHECK(available == 3);
    CHECK(read_text(fds[0], 4) == "abc");
    CHECK(call(nr_ioctl, {static_cast<std::uint64_t>(fds[0]), 0x5401 /* TCGETS */, address(buffer_for_termios())}) ==
          -enotty);
    call(nr_close, {static_cast<std::uint64_t>(fds[0])});
    call(nr_close, {static_cast<std::uint64_t>(fds[1])});
}

TEST(symlinks_keep_their_text_and_access_checks_modes) {
    Scratch scratch;
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "target", o_creat | o_wronly, 0400))});
    CHECK(call(nr_symlinkat, {address("target"), at_fdcwd, address("link")}) == 0);
    char text[64] = {};
    CHECK(call(nr_readlinkat, {at_fdcwd, address("link"), address(text), sizeof text}) == 6);
    CHECK(std::string(text) == "target");
    CHECK(call(nr_readlinkat, {at_fdcwd, address("target"), address(text), sizeof text}) == -einval);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address("link"), address(&status), at_symlink_nofollow}) == 0);
    CHECK((status.mode & 0170000) == 0120000);  // S_IFLNK
    CHECK(call(nr_newfstatat, {at_fdcwd, address("link"), address(&status), 0}) == 0);
    CHECK((status.mode & 0170000) == 0100000);  // followed to the file
    CHECK(call(nr_faccessat, {at_fdcwd, address("link"), 0 /* F_OK */}) == 0);
    CHECK(call(nr_faccessat, {at_fdcwd, address("target"), 4 /* R_OK */}) == 0);
    if (::geteuid() != 0) CHECK(call(nr_faccessat, {at_fdcwd, address("target"), 2 /* W_OK */}) == -eacces);
}

TEST(file_times_follow_utimensat) {
    Scratch scratch;
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "timed", o_creat | o_wronly, 0600))});
    const struct {
        std::int64_t seconds, nanoseconds;
    } times[2] = {{1'000'000'000, 5000}, {1'100'000'000, 7000}};
    CHECK(call(nr_utimensat, {at_fdcwd, address("timed"), address(times), 0}) == 0);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address("timed"), address(&status), 0}) == 0);
    CHECK(status.atime == 1'000'000'000 && status.atime_nsec == 5000);
    CHECK(status.mtime == 1'100'000'000 && status.mtime_nsec == 7000);
    // UTIME_OMIT leaves a time alone, UTIME_NOW sets it to now (Linux values).
    const struct {
        std::int64_t seconds, nanoseconds;
    } special[2] = {{0, (1 << 30) - 2}, {0, (1 << 30) - 1}};
    CHECK(call(nr_utimensat, {at_fdcwd, address("timed"), address(special), 0}) == 0);
    CHECK(call(nr_newfstatat, {at_fdcwd, address("timed"), address(&status), 0}) == 0);
    CHECK(status.atime == 1'000'000'000 && status.mtime > 1'700'000'000);
}

TEST(umask_masks_the_mode_of_new_files) {
    Scratch scratch;
    constexpr std::uint64_t nr_umask = 166;
    const auto previous = call(nr_umask, {077});
    CHECK(previous >= 0 && previous <= 0777);
    CHECK(call(nr_umask, {0022 | 0170000}) == 077);  // only the permission bits count
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "masked", o_creat | o_wronly, 0666))});
    struct stat host;
    CHECK(::stat((scratch.host + "/masked").c_str(), &host) == 0 && (host.st_mode & 0777) == 0644);
    call(nr_umask, {static_cast<std::uint64_t>(previous)});
}

TEST(statfs_describes_the_file_system) {
    Scratch scratch;
    struct {
        std::int64_t type, block_size, blocks, free, available, files, free_files;
        std::int32_t fsid[2];
        std::int64_t name_length, fragment_size, flags, spare[4];
    } linux_statfs{};
    static_assert(sizeof linux_statfs == 120);
    CHECK(call(nr_statfs, {address("/data/local/tmp"), address(&linux_statfs)}) == 0);
    CHECK(linux_statfs.block_size > 0 && linux_statfs.blocks > 0 && linux_statfs.name_length == 255);
    CHECK(call(nr_statfs, {address("/nowhere"), address(&linux_statfs)}) == -enoent);
}

TEST(clock_gettime_maps_the_linux_clocks) {
    struct {
        std::int64_t seconds, nanoseconds;
    } linux_time{};
    const auto nanoseconds = [&] { return linux_time.seconds * 1'000'000'000 + linux_time.nanoseconds; };
    const auto host = [](clockid_t clock) { return static_cast<std::int64_t>(clock_gettime_nsec_np(clock)); };

    // F10: Linux CLOCK_MONOTONIC stops during sleep like Darwin's CLOCK_UPTIME_RAW, CLOCK_BOOTTIME goes on
    // like Darwin's CLOCK_MONOTONIC.
    const struct {
        std::uint64_t linux_clock;
        clockid_t darwin_clock;
    } pairs[] = {{0, CLOCK_REALTIME}, {1, CLOCK_UPTIME_RAW}, {4, CLOCK_UPTIME_RAW}, {5, CLOCK_REALTIME},
                 {6, CLOCK_UPTIME_RAW}, {7, CLOCK_MONOTONIC}};
    for (const auto &pair : pairs) {
        const auto before = host(pair.darwin_clock);
        CHECK(call(nr_clock_gettime, {pair.linux_clock, reinterpret_cast<std::uint64_t>(&linux_time)}) == 0);
        const auto after = host(pair.darwin_clock);
        if (!(before <= nanoseconds() && nanoseconds() <= after)) {
            std::fprintf(stderr, "Linux clock %llu is not Darwin clock %d\n",
                         static_cast<unsigned long long>(pair.linux_clock), pair.darwin_clock);
            CHECK(false);
        }
        CHECK(linux_time.nanoseconds >= 0 && linux_time.nanoseconds < 1'000'000'000);
    }
    CHECK(call(nr_clock_gettime, {2, reinterpret_cast<std::uint64_t>(&linux_time)}) == 0);  // process CPU time
    CHECK(nanoseconds() > 0);
    CHECK(call(nr_clock_gettime, {10, reinterpret_cast<std::uint64_t>(&linux_time)}) == -einval);  // no clock 10
}

TEST(gettimeofday_fills_the_linux_timeval) {
    constexpr std::uint64_t nr_gettimeofday = 169;
    struct {
        std::int64_t seconds, microseconds;  // both 64-bit on Linux; Darwin's tv_usec is 32-bit
    } value{-1, -1};
    struct {
        std::int32_t minutes_west, dst_time;
    } zone{7, 7};
    CHECK(call(nr_gettimeofday, {address(&value), address(&zone)}) == 0);
    const auto now = static_cast<std::int64_t>(::time(nullptr));
    CHECK(value.seconds >= now - 1 && value.seconds <= now + 1);
    CHECK(value.microseconds >= 0 && value.microseconds < 1'000'000);
    CHECK(zone.minutes_west == 0 && zone.dst_time == 0);
    CHECK(call(nr_gettimeofday, {0, 0}) == 0);
}

std::int64_t monotonic_nanoseconds() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

TEST(nanosleep_sleeps_at_least_as_long_as_asked) {
    constexpr std::uint64_t nr_nanosleep = 101;
    const struct {
        std::int64_t seconds, nanoseconds;
    } ten_ms{0, 10'000'000}, invalid{0, 1'000'000'000};
    const auto before = monotonic_nanoseconds();
    CHECK(call(nr_nanosleep, {address(&ten_ms), 0}) == 0);
    CHECK(monotonic_nanoseconds() - before >= 10'000'000);
    CHECK(call(nr_nanosleep, {address(&invalid), 0}) == -einval);
}

TEST(clock_nanosleep_sleeps_relative_or_until_a_time) {
    constexpr std::uint64_t nr_clock_nanosleep = 115, clock_monotonic = 1, timer_abstime = 1;
    struct Time {
        std::int64_t seconds, nanoseconds;
    };
    const Time ten_ms{0, 10'000'000};
    auto before = monotonic_nanoseconds();
    CHECK(call(nr_clock_nanosleep, {clock_monotonic, 0, address(&ten_ms), 0}) == 0);
    CHECK(monotonic_nanoseconds() - before >= 10'000'000);

    Time now{};
    CHECK(call(nr_clock_gettime, {clock_monotonic, address(&now)}) == 0);
    Time until{now.seconds, now.nanoseconds + 10'000'000};
    if (until.nanoseconds >= 1'000'000'000) {
        until.seconds += 1;
        until.nanoseconds -= 1'000'000'000;
    }
    before = monotonic_nanoseconds();
    CHECK(call(nr_clock_nanosleep, {clock_monotonic, timer_abstime, address(&until), 0}) == 0);
    CHECK(monotonic_nanoseconds() - before >= 9'000'000);  // the clock read above came a little earlier
    CHECK(call(nr_clock_nanosleep, {clock_monotonic, timer_abstime, address(&now), 0}) == 0);  // in the past
    CHECK(call(nr_clock_nanosleep, {1234, 0, address(&ten_ms), 0}) == -einval);
}

TEST(sched_getaffinity_reports_every_active_cpu) {
    int cpus = 0;
    std::size_t size = sizeof cpus;
    CHECK(::sysctlbyname("hw.activecpu", &cpus, &size, nullptr, 0) == 0);
    std::uint64_t mask[2] = {~0ull, ~0ull};
    CHECK(call(nr_sched_getaffinity, {0, sizeof mask, reinterpret_cast<std::uint64_t>(mask)}) == 8);
    CHECK(mask[0] == (cpus >= 64 ? ~0ull : (1ull << cpus) - 1));
    CHECK(mask[1] == ~0ull);  // beyond the 8 bytes Linux reports, nothing is touched
    CHECK(call(nr_sched_getaffinity, {0, 4, reinterpret_cast<std::uint64_t>(mask)}) == -einval);
}

TEST(futex_waits_and_wakes_like_linux) {
    constexpr std::uint64_t futex_wait = 0, futex_wake = 1, futex_private = 128;
    alignas(4) static std::atomic<std::uint32_t> word{7};
    const auto address = reinterpret_cast<std::uint64_t>(&word);

    CHECK(call(nr_futex, {address, futex_wake | futex_private, 1}) == 0);  // nobody waits
    CHECK(call(nr_futex, {address, futex_wait | futex_private, 8, 0}) == -eagain);  // value differs

    struct {
        std::int64_t seconds, nanoseconds;
    } timeout{0, 20'000'000};
    CHECK(call(nr_futex, {address, futex_wait | futex_private, 7, reinterpret_cast<std::uint64_t>(&timeout)}) ==
          -etimedout);

    // One waiter on another thread, woken from here.
    static std::atomic<std::int64_t> waited{-1};
    pthread_t thread;
    CHECK(::pthread_create(
              &thread, nullptr,
              [](void *argument) -> void * {
                  waited = call(nr_futex, {reinterpret_cast<std::uint64_t>(argument), 0 | 128, 7, 0});
                  return nullptr;
              },
              &word) == 0);
    std::int64_t woken = 0;
    for (int attempt = 0; attempt < 2000 && woken == 0; ++attempt) {
        ::usleep(1000);
        woken = call(nr_futex, {address, futex_wake | futex_private, 1});
    }
    CHECK(woken == 1);
    CHECK(::pthread_join(thread, nullptr) == 0);
    CHECK(waited == 0);
}

constexpr std::uint64_t futex_wait = 0, futex_wake = 1, futex_requeue = 3, futex_private = 128;
constexpr std::uint64_t everyone = 0x7fff'ffff;

/// Moves waiters of `from` to `to` with FUTEX_REQUEUE, one per call, until `count` have moved: so they
/// all wait, on `to`, when it returns. False when they do not come within a second.
bool requeue_when_waiting(std::uint64_t from, std::uint64_t to, int count) {
    int moved = 0;
    for (int attempt = 0; attempt < 1000 && moved < count; ++attempt) {
        const auto result = call(nr_futex, {from, futex_requeue | futex_private, 0, 1, to});
        if (result < 0) return false;
        moved += static_cast<int>(result);
        if (moved < count) ::usleep(1000);
    }
    return moved == count;
}

TEST(futex_requeue_moves_waiters_to_another_word_like_linux) {
    // ART's condition variables move their waiters onto the mutex's word (FUTEX_REQUEUE_PRIVATE) in
    // Signal and Broadcast, so that the mutex's unlock wakes them (ADR-0033, decision 8). As Linux's
    // futex_requeue, it wakes `value` waiters, moves up to the count in the timeout argument, and returns
    // how many it woke and moved together.
    alignas(4) static std::atomic<std::uint32_t> condition{0}, mutex{0};
    const auto from = reinterpret_cast<std::uint64_t>(&condition);
    const auto to = reinterpret_cast<std::uint64_t>(&mutex);
    CHECK(call(nr_futex, {from, futex_requeue | futex_private, 1, 1, to}) == 0);  // nobody waits

    static std::atomic<std::int64_t> results[2] = {-1, -1};
    pthread_t threads[2];
    for (int index = 0; index < 2; ++index) {
        CHECK(::pthread_create(
                  &threads[index], nullptr,
                  [](void *result) -> void * {
                      *static_cast<std::atomic<std::int64_t> *>(result) = call(
                          nr_futex, {reinterpret_cast<std::uint64_t>(&condition), futex_wait | futex_private, 0, 0});
                      return nullptr;
                  },
                  &results[index]) == 0);
    }
    // Both wait on the mutex's word now; a wake of the condition's word finds nobody.
    CHECK(requeue_when_waiting(from, to, 2));
    CHECK(call(nr_futex, {from, futex_wake | futex_private, everyone}) == 0);
    // One call moves both, as ART's Broadcast moves every waiter of a condition variable at once (ADR-0033 c2: a
    // requeue that moved only one would show in no other test, as later wakes end the waits it left).
    CHECK(call(nr_futex, {to, futex_requeue | futex_private, 0, everyone, from}) == 2);
    CHECK(call(nr_futex, {from, futex_requeue | futex_private, 0, everyone, to}) == 2);
    // Back again: one woken and one moved is 2.
    CHECK(call(nr_futex, {to, futex_requeue | futex_private, 1, everyone, from}) == 2);
    CHECK(call(nr_futex, {to, futex_wake | futex_private, everyone}) == 0);
    CHECK(call(nr_futex, {from, futex_wake | futex_private, everyone}) == 1);
    // Whatever failed above, no thread waits on: one that comes late finds another value.
    condition = 1;
    mutex = 1;
    call(nr_futex, {from, futex_wake | futex_private, everyone});
    call(nr_futex, {to, futex_wake | futex_private, everyone});
    for (const auto thread : threads) CHECK(::pthread_join(thread, nullptr) == 0);
    CHECK(results[0] == 0 && results[1] == 0);

    // The second word must be aligned, and both counts are ints that may not be negative.
    CHECK(call(nr_futex, {from, futex_requeue | futex_private, 0, 1, to + 1}) == -einval);
    CHECK(call(nr_futex, {from, futex_requeue | futex_private, 0xffff'ffff, 1, to}) == -einval);
    CHECK(call(nr_futex, {from, futex_requeue | futex_private, 0, 0x8000'0000, to}) == -einval);
}

TEST(a_requeued_futex_wait_that_times_out_leaves_the_new_word_s_queue) {
    // A waiter whose wait ends by itself after a requeue, by its time as here or by a signal, takes itself
    // off the queue it is in by then: the new word's.
    alignas(4) static std::atomic<std::uint32_t> condition{0}, mutex{0};
    const auto from = reinterpret_cast<std::uint64_t>(&condition);
    const auto to = reinterpret_cast<std::uint64_t>(&mutex);
    static std::atomic<std::int64_t> result{-1};
    pthread_t thread;
    CHECK(::pthread_create(
              &thread, nullptr,
              [](void *) -> void * {
                  const struct {
                      std::int64_t seconds, nanoseconds;
                  } timeout{1, 0};
                  result = call(nr_futex, {reinterpret_cast<std::uint64_t>(&condition), futex_wait | futex_private, 0,
                                           reinterpret_cast<std::uint64_t>(&timeout)});
                  return nullptr;
              },
              nullptr) == 0);
    CHECK(requeue_when_waiting(from, to, 1));
    CHECK(::pthread_join(thread, nullptr) == 0);
    CHECK(result == -etimedout);
    CHECK(call(nr_futex, {to, futex_wake | futex_private, everyone}) == 0);
    CHECK(call(nr_futex, {from, futex_wake | futex_private, everyone}) == 0);
}

TEST(prctl_names_anonymous_memory_like_an_android_kernel) {
    // PR_SET_VMA, PR_SET_VMA_ANON_NAME: Android's kernels have CONFIG_ANON_VMA_NAME, and bionic names
    // every thread stack with it. Failing it would leave EINVAL in errno of every new thread.
    const auto memory = map_anonymous_pages(1);
    CHECK(call(nr_prctl, {0x5356'4d41, 0, memory, page, reinterpret_cast<std::uint64_t>("stack")}) == 0);
    CHECK(call(nr_prctl, {0x5356'4d41, 1, memory, page, 0}) == -einval);  // no other PR_SET_VMA options
    call(nr_munmap, {memory, page});
}

TEST(prctl_offers_no_tagged_address_abi) {
    // PR_SET_TAGGED_ADDR_CTRL: with it, bionic would tag heap pointers in the top byte.
    CHECK(call(nr_prctl, {55, 1, 0, 0, 0}) == -einval);
    CHECK(call(nr_prctl, {56, 0, 0, 0, 0}) == -einval);
}

// --- Resource limits (ADR-0035) ---------------------------------------------------------------------

constexpr std::uint64_t nr_getrlimit = 163, nr_setrlimit = 164, nr_prlimit64 = 261;
constexpr std::uint64_t rlimit_stack = 3, rlimit_nofile = 7, rlimit_as = 9;
constexpr std::uint64_t rlim_infinity = ~std::uint64_t{0};
constexpr std::uint64_t mib = 1 << 20;
constexpr std::int64_t enosys = 38;

/// Runs `body` in a child process and returns whether its checks passed there, as descriptors_test.cpp
/// does: for changes the process cannot take back. It runs on a thread of its own there, with a signal
/// state of its own.
template <typename Body>
bool in_child(Body body) {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        apk::testing::failures = 0;  // the child's checks alone, not the parent's failures from before the fork
        std::thread(body).join();
        std::fflush(nullptr);
        ::_exit(apk::testing::failures == 0 ? 0 : 1);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

TEST(the_stack_limit_is_androids_and_changes_by_linuxs_rules) {
    // bionic's pthread_getattr_np takes the main thread's stack size from RLIMIT_STACK, and ART asks
    // pthread_getattr_np for every thread it attaches. A hard limit never rises again, so in a child.
    CHECK(in_child([] {
        struct Limits {
            std::uint64_t soft, hard;
        } limits{};
        CHECK(call(nr_getrlimit, {rlimit_stack, address(&limits)}) == 0);
        CHECK(limits.soft == 8 * mib && limits.hard == rlim_infinity);
        limits = {};
        CHECK(call(nr_prlimit64, {0, rlimit_stack, 0, address(&limits)}) == 0);
        CHECK(limits.soft == 8 * mib && limits.hard == rlim_infinity);

        // A lower soft limit is read back; prlimit64 hands out the limits from before the change.
        const Limits lower{1 * mib, rlim_infinity};
        limits = {};
        CHECK(call(nr_prlimit64, {unsigned_of(::getpid()), rlimit_stack, address(&lower), address(&limits)}) == 0);
        CHECK(limits.soft == 8 * mib && limits.hard == rlim_infinity);
        CHECK(call(nr_getrlimit, {rlimit_stack, address(&limits)}) == 0);
        CHECK(limits.soft == 1 * mib && limits.hard == rlim_infinity);

        // The soft limit stays at or below the hard one, and the hard one can fall but not rise.
        const Limits inverted{16 * mib, 8 * mib};
        CHECK(call(nr_setrlimit, {rlimit_stack, address(&inverted)}) == -einval);
        const Limits equal{16 * mib, 16 * mib};
        CHECK(call(nr_setrlimit, {rlimit_stack, address(&equal)}) == 0);
        const Limits higher{16 * mib, 32 * mib};
        CHECK(call(nr_setrlimit, {rlimit_stack, address(&higher)}) == -eperm);
        const Limits both{64 * mib, 32 * mib};  // Linux checks the soft limit first
        CHECK(call(nr_setrlimit, {rlimit_stack, address(&both)}) == -einval);
        // A change that fails hands out nothing.
        const Limits unlimited{rlim_infinity, rlim_infinity};
        limits = {};
        CHECK(call(nr_prlimit64, {0, rlimit_stack, address(&unlimited), address(&limits)}) == -eperm);
        CHECK(limits.soft == 0 && limits.hard == 0);
        CHECK(call(nr_getrlimit, {rlimit_stack, address(&limits)}) == 0);
        CHECK(limits.soft == 16 * mib && limits.hard == 16 * mib);

        // RLIMIT_NOFILE stays the descriptor table's, and other resources are not supported yet.
        CHECK(call(nr_getrlimit, {rlimit_nofile, address(&limits)}) == 0);
        CHECK(limits.soft == 32768 && limits.hard == 32768);
        CHECK(call(nr_getrlimit, {rlimit_as, address(&limits)}) == -enosys);
    }));
}

// --- Sockets (ADR-0035) -----------------------------------------------------------------------------

constexpr std::uint64_t nr_socket = 198;
constexpr std::uint64_t af_unix = 1, sock_stream = 1, sock_dgram = 2, sock_nonblock = 04000,
                        sock_cloexec = 02000000;
constexpr std::int64_t eafnosupport = 97;

/// What the runtime writes to the process's stderr while `body` runs.
template <typename Body>
std::string stderr_of(Body body) {
    char path[] = "/tmp/apkrt-stderr-XXXXXX";
    const int file = ::mkstemp(path);
    const int saved = ::dup(STDERR_FILENO);
    ::dup2(file, STDERR_FILENO);
    body();
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    std::string text(4096, '\0');
    const auto length = ::pread(file, text.data(), text.size(), 0);
    ::close(file);
    ::unlink(path);
    text.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
    return text;
}

TEST(bionics_log_socket_finds_no_logd_quietly) {
    // bionic's async_safe_log opens logd's socket before every message and writes to stderr when its connect fails.
    // There is no logd here: the socket is made, and the connect to /dev/socket/logdw answers ENOENT, both without a
    // report, which would reach the process's stderr past the guest's own (ADR-0054, which replaces ADR-0035's refusal
    // of the socket). In a child, since a report comes once per process.
    CHECK(in_child([] {
        std::int64_t fd = 0, connected = 0;
        struct {
            std::uint16_t family = af_unix;
            char path[108] = "/dev/socket/logdw";
        } logd;
        CHECK(stderr_of([&] {
                  fd = call(nr_socket, {af_unix, sock_dgram | sock_cloexec | sock_nonblock, 0});
                  connected = call(203, {unsigned_of(fd), address(&logd), 2 + sizeof "/dev/socket/logdw"});
              }).empty());
        CHECK(fd >= 0 && connected == -enoent);
        call(nr_close, {unsigned_of(fd)});
        // Linux takes the three as int: the upper halves of the registers do not count.
        fd = call(nr_socket, {af_unix | 0xdead'0000'0000, sock_dgram | sock_cloexec | sock_nonblock | 0xbeef'0000'0000,
                              0x1'0000'0000});
        CHECK(fd >= 0);
        call(nr_close, {unsigned_of(fd)});
        CHECK(call(nr_socket, {12345, sock_stream, 0}) == -eafnosupport);
    }));
}

// --- What ART's start asks of the kernel (ADR-0033, decision 8) -------------------------------------

constexpr std::uint64_t nr_eventfd2 = 19, nr_epoll_create1 = 20, nr_capget = 90, nr_capset = 91,
                        nr_sched_setaffinity = 122, nr_setpriority = 140, nr_uname = 160, nr_userfaultfd = 282,
                        nr_membarrier = 283;

TEST(userfaultfd_and_membarrier_are_refused_quietly) {
    // ART asks for both at every start: userfaultfd for its CMC collector, which our build leaves out (decision
    // 5), and membarrier for its fault handler, which then only warns. Both are refused as by a kernel built
    // without them, and without the report, which would reach the process's stderr past the guest's own: ART's
    // run-tests compare stderr (ADR-0033 test 3). In a child, since a report comes once per process.
    CHECK(in_child([] {
        constexpr std::uint64_t o_cloexec = 02000000, uffd_user_mode_only = 1, membarrier_cmd_query = 0,
                                membarrier_cmd_register_private_expedited = 16;
        std::int64_t result = 0;
        CHECK(stderr_of([&] { result = call(nr_userfaultfd, {o_cloexec | uffd_user_mode_only}); }).empty());
        CHECK(result == -enosys);
        CHECK(stderr_of([&] { result = call(nr_membarrier, {membarrier_cmd_query, 0, 0}); }).empty());
        CHECK(result == -enosys);
        CHECK(stderr_of([&] {
                  result = call(nr_membarrier, {membarrier_cmd_register_private_expedited, 0, 0});
              }).empty());
        CHECK(result == -enosys);
    }));
}

TEST(getpriority_answers_the_nice_value_of_every_guest_thread) {
    // ART's thread dumps ask for it (ADR-0033 test 3). setpriority keeps nothing, so every guest thread runs at the
    // nice value 0, which the kernel answers as 20 - nice, so that no answer is negative.
    constexpr std::uint64_t nr_getpriority = 141;
    CHECK(call(nr_getpriority, {0, 0}) == 20);  // PRIO_PROCESS, the calling thread
    CHECK(call(nr_setpriority, {0, 0, 10}) == 0);
    CHECK(call(nr_getpriority, {0, unsigned_of(::getpid())}) == 20);
    CHECK(call(nr_getpriority, {3, 0}) == -einval);  // which is PRIO_PROCESS, PRIO_PGRP or PRIO_USER
}

TEST(getsockopt_finds_no_socket) {
    // libcore's FileDescriptor.isSocket asks for SO_DOMAIN, for example when a file is closed (ADR-0033 test 3). The
    // guest has no sockets: an open descriptor is none, as Linux answers for a file, and a closed one is bad.
    constexpr std::uint64_t nr_getsockopt = 209, sol_socket = 1, so_domain = 39;
    constexpr std::int64_t enotsock = 88;
    const auto fd = static_cast<std::uint64_t>(call(nr_eventfd2, {0, 0}));
    std::int32_t domain = 0;
    std::uint32_t length = sizeof domain;
    CHECK(call(nr_getsockopt, {fd, sol_socket, so_domain, address(&domain), address(&length)}) == -enotsock);
    CHECK(call(nr_close, {fd}) == 0);
    CHECK(call(nr_getsockopt, {fd, sol_socket, so_domain, address(&domain), address(&length)}) == -ebadf);
}

TEST(process_vm_readv_reads_the_process_s_own_memory_without_faulting) {
    // libunwindstack reads memory through it, so that an unwinder never faults on a bad address: ART's thread dumps
    // and the call stacks of crashes (ADR-0033 test 3). The guest can name only its own process. A transfer ends at
    // the first remote element that cannot be read whole, and an element is never split.
    constexpr std::uint64_t nr_process_vm_readv = 270;
    struct Iovec {
        std::uint64_t base, length;
    };
    const auto self = unsigned_of(::getpid());
    static const char source[] = "unwind me";
    char target[16] = {};
    Iovec local[2] = {{address(target), 3}, {address(target + 3), sizeof target - 3}};
    Iovec remote[2] = {{address(source), 6}, {address(source + 7), 2}};
    CHECK(call(nr_process_vm_readv, {self, address(local), 2, address(remote), 2, 0}) == 8);
    CHECK(std::memcmp(target, "unwindme", 8) == 0);
    remote[1] = {0x1000, 8};  // never mapped: what came before is read
    CHECK(call(nr_process_vm_readv, {self, address(local), 2, address(remote), 2, 0}) == 6);
    CHECK(call(nr_process_vm_readv, {self, address(local), 2, address(remote + 1), 1, 0}) == -efault);
    Iovec read_only = {address(source), 4};  // a destination that cannot be written
    CHECK(call(nr_process_vm_readv, {self, address(&read_only), 1, address(remote), 1, 0}) == -efault);
    CHECK(call(nr_process_vm_readv, {0, 0, 0, 0, 0, 0}) == -esrch);  // no process of the guest's
    CHECK(call(nr_process_vm_readv, {self, address(local), 2, address(remote), 1, 1}) == -einval);  // flags
}

TEST(uname_answers_as_android_15_s_kernel_in_the_emulator) {
    // The kernel of the SDK's Android 15 image (kernel-ranchu of android-35): a release of 5.13 or newer,
    // which ART's read-barrier build checks and apps read. Its release ends in -4k, for its pages; the
    // guest's are 16 KB, as getpagesize says, and so the release ends in -16k.
    struct {
        char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
    } names;
    std::memset(&names, 'x', sizeof names);
    CHECK(call(nr_uname, {address(&names)}) == 0);
    const auto text = [](const char (&field)[65]) { return std::string(field, ::strnlen(field, sizeof field)); };
    CHECK(text(names.sysname) == "Linux");
    CHECK(text(names.nodename) == "localhost");
    CHECK(text(names.release) == "6.6.30-android15-8-gdd9c02ccfe27-ab11987101-16k");
    CHECK(text(names.version) == "#1 SMP PREEMPT Tue Jun 18 20:50:32 UTC 2024");
    CHECK(text(names.machine) == "aarch64");
    CHECK(text(names.domainname) == "(none)");
}

TEST(priorities_affinity_and_capabilities_succeed_without_effect) {
    // Unity sets thread priorities and cores; libcap asks for capabilities. Accepted and kept nowhere:
    // nothing on the host changes, and an app has no capabilities to get or to set.
    CHECK(call(nr_setpriority, {0, 0, 10}) == 0);  // PRIO_PROCESS, this process, nice 10
    CHECK(call(nr_setpriority, {0, unsigned_of(::getpid()), 0}) == 0);
    CHECK(call(nr_setpriority, {3, 0, 0}) == -einval);  // which is PRIO_PROCESS, PRIO_PGRP or PRIO_USER
    std::uint64_t mask = 1;                              // CPU 0
    CHECK(call(nr_sched_setaffinity, {0, sizeof mask, address(&mask)}) == 0);
    mask = 0;
    CHECK(call(nr_sched_setaffinity, {0, sizeof mask, address(&mask)}) == -einval);  // no CPU at all
    std::uint64_t all = 0;
    CHECK(call(nr_sched_getaffinity, {0, sizeof all, address(&all)}) == 8 && (all & 1) != 0 && all != 1);

    constexpr std::uint32_t version_3 = 0x2008'0522;  // _LINUX_CAPABILITY_VERSION_3: two sets of 32 bits
    struct {
        std::uint32_t version;
        std::int32_t pid;
    } header{version_3, 0};
    struct {
        std::uint32_t effective, permitted, inheritable;
    } sets[2];
    std::memset(sets, 0xff, sizeof sets);
    CHECK(call(nr_capget, {address(&header), address(sets)}) == 0);
    CHECK(std::all_of(reinterpret_cast<const std::uint8_t *>(sets), reinterpret_cast<const std::uint8_t *>(sets + 2),
                      [](std::uint8_t byte) { return byte == 0; }));
    CHECK(call(nr_capget, {address(&header), 0}) == 0);  // libcap's cap_init asks for the version this way
    header.version = 0x1234'5678;
    CHECK(call(nr_capget, {address(&header), address(sets)}) == -einval);
    CHECK(header.version == version_3);  // the kernel's version, for the next try
    CHECK(call(nr_capset, {address(&header), address(sets)}) == 0);  // nothing, which is what there is
    sets[0].effective = 1;                                            // CAP_CHOWN
    CHECK(call(nr_capset, {address(&header), address(sets)}) == -eperm);
}

/// The whole guest file `path`, read through the guest's syscalls; "error <n>" when it does not open.
std::string read_guest_file(const char *path) {
    const auto fd = open_guest(at_fdcwd, path, o_rdonly | o_cloexec);
    if (fd < 0) return "error " + std::to_string(-fd);
    std::string text;
    for (std::string part; !(part = read_text(fd, 1000)).empty() && !part.starts_with("error ");) text += part;
    call(nr_close, {unsigned_of(fd)});
    return text;
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> lines;
    for (std::size_t start = 0, end; start < text.size(); start = end + 1) {
        end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(start, end - start));
    }
    return lines;
}

int cpu_count(const char *name) {
    int cpus = 0;
    std::size_t size = sizeof cpus;
    return ::sysctlbyname(name, &cpus, &size, nullptr, 0) == 0 ? cpus : -1;
}

TEST(the_cpu_files_count_the_device_s_cpus) {
    // bionic's sysconf(_SC_NPROCESSORS_CONF) and (_SC_NPROCESSORS_ONLN) read them, and count 1 without
    // them. "online" agrees with sched_getaffinity.
    CHECK(read_guest_file("/sys/devices/system/cpu/possible") == std::format("0-{}\n", cpu_count("hw.ncpu") - 1));
    CHECK(read_guest_file("/sys/devices/system/cpu/online") == std::format("0-{}\n", cpu_count("hw.activecpu") - 1));
    CHECK(read_guest_file("/sys/devices/system/cpu/present") == "error 2");  // not yet
    CHECK(open_guest(at_fdcwd, "/sys/devices/system/cpu/online", o_wronly) == -eacces);
}

TEST(linkerconfig_gives_libnativeloader_the_apexes_libraries) {
    // libnativeloader links an app's class loader namespace to an APEX's namespace for the public libraries this file
    // gives it, and to the system's for the rest (ADR-0033 e2b3). linkerconfig writes it on Android 15, the runtime
    // here, from its namespace table: the libraries of /system/etc/public.libraries.txt that com_android_art serves,
    // ICU's among them (ADR-0040 decision 2), and Conscrypt's JNI library.
    constexpr const char *path = "/linkerconfig/apex.libraries.config.txt";
    Scratch scratch;
    CHECK(read_guest_file(path) == "jni com_android_conscrypt libjavacrypto.so\n");  // no public list
    CHECK(::mkdir((scratch.host + "/system").c_str(), 0755) == 0);
    CHECK(::mkdir((scratch.host + "/system/etc").c_str(), 0755) == 0);
    if (FILE *file = std::fopen((scratch.host + "/system/etc/public.libraries.txt").c_str(), "w")) {
        std::fputs("# a comment\nlibc.so\nlibicu.so\nlibnativehelper.so 64\nlibz.so\n", file);
        std::fclose(file);
    }
    apkrt::vfs::mount("/system", scratch.host + "/system");
    CHECK(read_guest_file(path) ==
          "public com_android_art libnativehelper.so:libicu.so\njni com_android_conscrypt libjavacrypto.so\n");
    CHECK(open_guest(at_fdcwd, path, o_wronly) == -eacces);
}

TEST(proc_cpuinfo_names_the_features_the_guest_is_told_as_linux_writes_them) {
    // ADR-0047 c): one block for each online CPU, in the format of Linux's c_show (arch/arm64/kernel/cpuinfo.c): the
    // Features line names the bits of AT_HWCAP and AT_HWCAP2, BogoMIPS comes from the counter's frequency as Linux
    // derives it with Android's HZ of 250, and the MIDR fields are those Android 15 shows on Apple's hypervisor.
    std::uint64_t frequency = 0;
    std::size_t size = sizeof frequency;
    CHECK(::sysctlbyname("hw.tbfrequency", &frequency, &size, nullptr, 0) == 0);
    const std::uint64_t jiffy = frequency / 250;  // loops_per_jiffy
    const std::string features = apkrt::hwcap::features(apkrt::hwcap::guest());
    std::string wanted;
    for (int cpu = 0; cpu < cpu_count("hw.activecpu"); ++cpu) {
        wanted += std::format("processor\t: {}\nBogoMIPS\t: {}.{:02}\nFeatures\t: {}\nCPU implementer\t: 0x61\n"
                              "CPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x000\nCPU revision\t: 0\n\n",
                              cpu, jiffy / 2000, jiffy / 20 % 100, features);
    }
    CHECK(read_guest_file("/proc/cpuinfo") == wanted);
    CHECK(wanted.find("BogoMIPS\t: 48.00\n") != std::string::npos);  // the 24 MHz counter of Apple's CPUs
    CHECK(open_guest(at_fdcwd, "/proc/cpuinfo", o_wronly) == -eacces);
}

/// A main stack for the files of /proc, as program.cpp gives one: 8 MiB.
struct MainStack {
    std::uint64_t low = 0, high = 0, start = 0;

    MainStack() {
        constexpr std::size_t size = 8 << 20;
        void *const memory = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        low = reinterpret_cast<std::uint64_t>(memory);
        high = low + size;
        start = high - 4096;
        apkrt::proc::set_main_stack(low, high, start);
        apkrt::proc::set_main_thread(::pthread_self(), "a-name-longer-than-fifteen");
    }
    ~MainStack() { ::munmap(reinterpret_cast<void *>(low), high - low); }
    MainStack(const MainStack &) = delete;
    MainStack &operator=(const MainStack &) = delete;
};

TEST(proc_self_stat_reads_as_linux_writes_it) {
    // bionic's pthread_getattr_np takes the main thread's stack from field 28 (startstack) and
    // /proc/self/maps; it aborts without them. The line has Linux's 52 fields (fs/proc/array.c).
    const MainStack stack;
    ::pthread_setname_np("");  // a thread that has not named itself has the program's name
    const std::string text = read_guest_file("/proc/self/stat");
    CHECK(text.ends_with('\n') && lines_of(text).size() == 1);
    const auto comm_begin = text.find(" ("), comm_end = text.rfind(") ");
    CHECK(comm_begin != std::string::npos && comm_end != std::string::npos);
    CHECK(text.substr(0, comm_begin) == std::to_string(::getpid()));
    // TASK_COMM_LEN: 15 characters and the NUL.
    CHECK(text.substr(comm_begin + 2, comm_end - comm_begin - 2) == "a-name-longer-t");
    std::vector<std::string> fields{std::to_string(::getpid()), "comm"};
    std::istringstream rest(text.substr(comm_end + 2));
    for (std::string field; rest >> field;) fields.push_back(field);
    CHECK(fields.size() == 52);
    CHECK(fields.size() >= 52 && fields[2] == "R" && fields[3] == std::to_string(::getppid()));
    CHECK(fields.size() >= 52 && fields[27] == std::to_string(stack.start));
    CHECK(fields.size() >= 52 && fields[24] == "18446744073709551615");  // rsslim: RLIM_INFINITY
    // The process's own number can stand for self, and the name follows the main thread's.
    ::pthread_setname_np("renamed");
    const std::string again = read_guest_file(std::format("/proc/{}/stat", ::getpid()).c_str());
    CHECK(again.find(" (renamed) R ") != std::string::npos);
    CHECK(read_guest_file("/proc/1/stat") == "error 2");  // other processes are not there
}

// ADR-0039 decision 3: /proc/self/cmdline reads the arguments where the program's start put them, as they are
// now, as Linux 6.6's get_mm_cmdline does. So app_process can rename the process as AndroidRuntime::setArgv0
// does, over the whole block; and a program that writes past the arguments' last NUL into the environment
// (setproctitle) shows its first string.
TEST(proc_self_cmdline_reads_the_arguments_as_they_are_now) {
    const MainStack stack;
    // As Linux's exec lays the start out: the strings of argv, then those of envp, at the top of the stack.
    const std::string arguments = std::string("/system/bin/app_process64\0--nice-name=io.example\0--x\0", 53);
    const std::string environment = std::string("A=1\0B=2\0", 8);
    char *const strings = reinterpret_cast<char *>(stack.high) - 128;
    std::memcpy(strings, (arguments + environment).data(), arguments.size() + environment.size());
    auto *const words = reinterpret_cast<std::uint64_t *>(stack.start);
    const std::size_t argument_starts[] = {0, 26, 49}, environment_starts[] = {53, 57};
    words[0] = 3;  // argc
    for (std::size_t index = 0; index < 3; ++index) words[1 + index] = address(strings + argument_starts[index]);
    words[4] = 0;
    for (std::size_t index = 0; index < 2; ++index) words[5 + index] = address(strings + environment_starts[index]);
    words[7] = 0;
    apkrt::proc::set_main_stack(stack.low, stack.high, stack.start);
    CHECK(read_guest_file("/proc/self/cmdline") == arguments);
    CHECK(read_guest_file(std::format("/proc/{}/cmdline", ::getpid()).c_str()) == arguments);

    // setArgv0: the block cleared, the name at its start; the NULs up to the old end stay part of the file.
    std::memset(strings, 0, arguments.size());
    std::memcpy(strings, "io.example", 10);
    CHECK(read_guest_file("/proc/self/cmdline") == "io.example" + std::string(arguments.size() - 10, '\0'));

    // setproctitle: the last byte of the arguments is no NUL now, so the file is the first string with its NUL,
    // which may reach into the environment, but not past its end.
    const std::string title = "a title of 56 characters, past the arguments' last byte";
    CHECK(title.size() > arguments.size() && title.size() < arguments.size() + environment.size());
    std::memcpy(strings, title.c_str(), title.size() + 1);
    CHECK(read_guest_file("/proc/self/cmdline") == title + std::string(1, '\0'));
    const std::string longer(arguments.size() + environment.size() + 5, 't');
    std::memcpy(strings, longer.c_str(), longer.size() + 1);
    CHECK(read_guest_file("/proc/self/cmdline") == longer.substr(0, arguments.size() + environment.size()));
}

TEST(proc_self_maps_shows_the_main_stack_and_no_host_path) {
    // Every mapping in Linux's format (show_map_vma), byte for byte: the name, where there is one, after
    // spaces up to column 73. The main stack is [stack]; whatever only the host maps (its libraries, its
    // own code) is anonymous, so that no host path reaches the guest. The guest's own objects come in the
    // fixture kernel-test, which has some.
    const MainStack stack;
    const auto lines = lines_of(read_guest_file("/proc/self/maps"));
    CHECK(lines.size() > 10);
    const std::regex format("([0-9a-f]{8,})-([0-9a-f]{8,}) [r-][w-][x-][ps] ([0-9a-f]{8,}) [0-9a-f]{2,}:[0-9a-f]{2,} "
                            "[0-9]+ ");
    std::uint64_t previous_end = 0;
    int stacks = 0;
    bool code_found = false;
    const auto code = reinterpret_cast<std::uint64_t>(&read_guest_file);
    for (const auto &line : lines) {
        std::smatch match;
        const bool prefix = std::regex_search(line, match, format, std::regex_constants::match_continuous);
        CHECK(prefix);
        if (!prefix) continue;
        const auto begin = std::stoull(match[1].str(), nullptr, 16), end = std::stoull(match[2].str(), nullptr, 16);
        CHECK(begin < end && begin >= previous_end);
        previous_end = end;
        const auto prefix_length = static_cast<std::size_t>(match.length());
        const std::string name = line.substr(prefix_length);
        if (name.empty()) {
            if (begin <= code && code < end) code_found = true;
            continue;
        }
        CHECK(line.size() > 73 && line[72] == ' ' && line[73] != ' ');
        CHECK(prefix_length + name.find_first_not_of(' ') == 73);
        CHECK(line.substr(73) == "[stack]");
        if (line.substr(73) == "[stack]") {
            ++stacks;
            CHECK(begin == stack.low && end == stack.high);
        }
    }
    CHECK(stacks == 1);
    CHECK(code_found);  // the test's own code is in an anonymous mapping
}

// ART's run-test 071-dexfile-map-clean (ADR-0033 c1) reads /proc/self/smaps: a file the guest maps is named in maps
// and smaps by its guest path, with its offset, and smaps follows each line with Linux 6.6's sizes, as Android 15's
// emulator writes them. A page read through a private mapping of a written-back file stays clean and one written
// there is dirty; one never touched counts while the file's page is cached, as Linux's fault-around maps it. An unmap
// forgets the name, also where the host maps next, and mremap moves it with the pages it moves, at their offsets.
TEST(proc_self_smaps_names_a_files_mapping_and_counts_its_dirty_pages) {
    const Scratch scratch;
    const auto file = open_guest(at_fdcwd, "./mapped", o_rdwr | o_creat | o_cloexec, 0600);
    CHECK(file >= 0);
    CHECK(write_text(file, std::string(4 * page, 'm')) == static_cast<long>(4 * page));
    CHECK(call(nr_fsync, {unsigned_of(file)}) == 0);  // written back, as the run-test's sync does
    const auto mapped = static_cast<std::uint64_t>(
        call(nr_mmap, {0, 3 * page, prot_read | prot_write, map_private, unsigned_of(file), page}));
    CHECK(mapped % page == 0);
    CHECK(*reinterpret_cast<volatile char *>(mapped) == 'm');        // read: clean
    *reinterpret_cast<volatile char *>(mapped + page) = 'w';         // written: a private copy, dirty
    const auto header = std::format("{:08x}-{:08x} rw-p {:08x} ", mapped, mapped + 3 * page, page);
    const auto find = [&](const std::vector<std::string> &lines, const std::string &start) {
        return std::find_if(lines.begin(), lines.end(), [&](const std::string &line) { return line.starts_with(start); });
    };
    const auto maps = lines_of(read_guest_file("/proc/self/maps"));
    const auto line = find(maps, header);
    CHECK(line != maps.end() && line->ends_with(" /data/local/tmp/mapped") && line->size() > 73 &&
          line->substr(73) == "/data/local/tmp/mapped");
    const auto smaps = lines_of(read_guest_file("/proc/self/smaps"));
    auto at = find(smaps, header);
    CHECK(at != smaps.end());
    const char *const names[] = {"Size", "KernelPageSize", "MMUPageSize", "Rss", "Pss", "Pss_Dirty", "Shared_Clean",
                                 "Shared_Dirty", "Private_Clean", "Private_Dirty", "Referenced", "Anonymous", "KSM",
                                 "LazyFree", "AnonHugePages", "ShmemPmdMapped", "FilePmdMapped", "Shared_Hugetlb",
                                 "Private_Hugetlb", "Swap", "SwapPss", "Locked"};
    std::map<std::string, std::uint64_t> sizes;
    for (const char *name : names) {
        if (at == smaps.end() || ++at == smaps.end()) break;
        // The name, then the number in kB, right-aligned to column 24, as SEQ_PUT_DEC writes it.
        CHECK(at->starts_with(std::string(name) + ":") && at->size() == 27 && at->ends_with(" kB") &&
              (*at)[23] != ' ');
        sizes[name] = std::stoull(at->substr(std::string(name).size() + 1, 23 - std::string(name).size()));
    }
    CHECK(at != smaps.end() && ++at != smaps.end() && *at == "THPeligible:           0");
    CHECK(at != smaps.end() && ++at != smaps.end() && *at == "VmFlags: rd wr mr mw me ac ");
    const std::uint64_t kb = page / 1024;
    CHECK(sizes["Size"] == 3 * kb && sizes["KernelPageSize"] == kb && sizes["MMUPageSize"] == kb);
    CHECK(sizes["Rss"] >= 2 * kb && sizes["Rss"] <= 3 * kb && sizes["Pss"] == sizes["Rss"]);
    CHECK(sizes["Private_Clean"] == sizes["Rss"] - kb && sizes["Private_Dirty"] == kb && sizes["Pss_Dirty"] == kb);
    CHECK(sizes["Shared_Clean"] == 0 && sizes["Shared_Dirty"] == 0 && sizes["Swap"] == 0);
    const auto named = [&](std::uint64_t begin, std::uint64_t end, std::uint64_t offset) {
        const auto lines = lines_of(read_guest_file("/proc/self/maps"));
        const auto found = find(lines, std::format("{:08x}-{:08x} rw-p {:08x} ", begin, end, offset));
        return found != lines.end() && found->ends_with(" /data/local/tmp/mapped");
    };
    // mremap moves the name with the pages: all three, then the last one alone, at its own offset.
    const auto target = static_cast<std::uint64_t>(
        call(nr_mmap, {0, 3 * page, prot_read, map_private | map_anonymous, ~std::uint64_t{0}, 0}));
    const auto last = static_cast<std::uint64_t>(
        call(nr_mmap, {0, page, prot_read, map_private | map_anonymous, ~std::uint64_t{0}, 0}));
    CHECK(call(nr_mremap, {mapped, 3 * page, 3 * page, mremap_maymove | mremap_fixed, target}) ==
          static_cast<long>(target));
    CHECK(read_guest_file("/proc/self/maps").find(header) == std::string::npos);
    CHECK(named(target, target + 3 * page, page));
    CHECK(call(nr_mremap, {target + 2 * page, page, page, mremap_maymove | mremap_fixed, last}) ==
          static_cast<long>(last));
    CHECK(named(target, target + 2 * page, page) && named(last, last + page, 3 * page));
    // An unmap forgets it: of the first page, the rest keeps the name at the file's next offset; then all of it, so
    // that what the host maps there next, as the runtime's own memory, is no file of the guest's.
    CHECK(call(nr_munmap, {target, page}) == 0);
    CHECK(named(target + page, target + 2 * page, 2 * page));
    CHECK(call(nr_munmap, {last, page}) == 0);
    CHECK(call(nr_munmap, {target + page, page}) == 0);
    void *const host = ::mmap(reinterpret_cast<void *>(target), 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                              -1, 0);  // a hint, which Darwin keeps while the range is free
    CHECK(host == reinterpret_cast<void *>(target));
    CHECK(read_guest_file("/proc/self/maps").find("/data/local/tmp/mapped") == std::string::npos);
    ::munmap(host, 2 * page);
    call(nr_close, {unsigned_of(file)});
}

// A region the host shares with another task, here a child after fork, holds shared pages in smaps, as Linux counts a
// page that more than one process maps. Its line says "p" like every line, so its VmFlags are a private mapping's.
TEST(proc_self_smaps_counts_the_pages_another_task_shares) {
    const Scratch scratch;
    auto *const region = static_cast<char *>(
        ::mmap(nullptr, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    CHECK(region != MAP_FAILED);
    CHECK(::mprotect(region, page, PROT_NONE) == 0 && ::mprotect(region + 3 * page, page, PROT_NONE) == 0);
    region[page] = 1;
    region[2 * page] = 1;
    int ends[2];
    CHECK(::pipe(ends) == 0);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(ends[1]);
        char byte;
        ::_exit(static_cast<int>(::read(ends[0], &byte, 1)));  // shares the region until the parent closes the pipe
    }
    CHECK(child > 0);
    const auto begin = reinterpret_cast<std::uint64_t>(region + page);
    const auto smaps = lines_of(read_guest_file("/proc/self/smaps"));
    ::close(ends[1]);
    ::close(ends[0]);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ::munmap(region, 4 * page);
    const auto at = std::find_if(smaps.begin(), smaps.end(), [&](const std::string &line) {
        return line.starts_with(std::format("{:08x}-{:08x} rw-p ", begin, begin + 2 * page));
    });
    CHECK(at != smaps.end());
    std::map<std::string, std::uint64_t> sizes;
    std::string flags;
    for (auto line = at == smaps.end() ? at : std::next(at); line != smaps.end(); ++line) {
        if (line->starts_with("VmFlags:")) {
            flags = *line;
            break;
        }
        if (const auto colon = line->find(':'); colon != std::string::npos && line->ends_with(" kB")) {
            sizes[line->substr(0, colon)] = std::stoull(line->substr(colon + 1));
        }
    }
    const std::uint64_t kb = page / 1024;
    CHECK(sizes["Rss"] == 2 * kb && sizes["Shared_Dirty"] == 2 * kb);
    CHECK(sizes["Shared_Clean"] == 0 && sizes["Private_Clean"] == 0 && sizes["Private_Dirty"] == 0);
    CHECK(flags == "VmFlags: rd wr mr mw me ac ");
}

TEST(androids_links_show_as_links_and_name_their_targets) {
    // getCanonicalPath of an app's files folder is /data/data/<package>/files on Android 15 (Formate's pad-test
    // probe): bionic's realpath reads /proc/self/fd of an O_PATH descriptor, which names the file after the link.
    const Scratch scratch;
    CHECK(::mkdir((scratch.host + "/app").c_str(), 0700) == 0);
    apkrt::vfs::mount("/data/user/0/io.example", scratch.host + "/app");  // as GuestStart mounts it
    const auto file = open_guest(at_fdcwd, "/data/user/0/io.example/a", o_creat | o_wronly, 0600);
    CHECK(file >= 0);
    char text[64] = {};
    const auto fd_path = std::format("/proc/self/fd/{}", file);
    const auto length = call(nr_readlinkat, {at_fdcwd, address(fd_path.c_str()), address(text), sizeof text});
    CHECK(length > 0 && std::string(text, static_cast<std::size_t>(length)) == "/data/data/io.example/a");
    call(nr_close, {unsigned_of(file)});
    const auto target = [&](const char *link) {
        const auto got = call(nr_readlinkat, {at_fdcwd, address(link), address(text), sizeof text});
        return got < 0 ? std::string("error") : std::string(text, static_cast<std::size_t>(got));
    };
    CHECK(target("/data/user/0") == "/data/data");
    CHECK(target("/sdcard") == "/storage/self/primary");
    CHECK(target("/storage/self/primary") == "/storage/emulated/0");
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address("/sdcard"), address(&status), at_symlink_nofollow}) == 0);
    CHECK((status.mode & 0170000) == 0120000 && status.size == 21);
    CHECK(::mkdir((scratch.host + "/external").c_str(), 0700) == 0);
    apkrt::vfs::mount("/storage/emulated/0", scratch.host + "/external");
    CHECK(call(nr_newfstatat, {at_fdcwd, address("/sdcard"), address(&status), 0}) == 0);
    CHECK((status.mode & 0170000) == 0040000);  // stat follows both links to the folder
    CHECK(call(nr_readlinkat, {at_fdcwd, address("/sdcard"), address(text), 4}) == 4 && std::string(text, 4) == "/sto");
    // readlinkat's size is an int: the bits above it are not part of it.
    CHECK(call(nr_readlinkat, {at_fdcwd, address("/sdcard"), address(text), 0x1'0000'0004}) == 4);
    // A trailing "/" or "." follows the link, which lstat and readlink then do not see (EINVAL: a folder).
    for (const char *folder : {"/sdcard/", "/sdcard/."}) {
        CHECK(call(nr_newfstatat, {at_fdcwd, address(folder), address(&status), at_symlink_nofollow}) == 0);
        CHECK((status.mode & 0170000) == 0040000);
        CHECK(call(nr_readlinkat, {at_fdcwd, address(folder), address(text), sizeof text}) == -einval);
    }
}

TEST(a_path_of_path_max_bytes_is_too_long) {
    // getname: PATH_MAX (4096) bytes with the NUL; one byte fewer is a path, here of folders that are not there.
    constexpr std::int64_t enametoolong = 36;
    const Scratch scratch;
    std::string path;
    while (path.size() < 4095) path += "/x";
    path.resize(4095);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address(path.c_str()), address(&status), 0}) == -enoent);
    path += 'x';
    CHECK(call(nr_newfstatat, {at_fdcwd, address(path.c_str()), address(&status), 0}) == -enametoolong);
    CHECK(open_guest(at_fdcwd, path.c_str(), o_rdonly) == -enametoolong);
    char text[16];
    CHECK(call(nr_readlinkat, {at_fdcwd, address(path.c_str()), address(text), sizeof text}) == -enametoolong);
    std::string proc = "/proc/self/stat";  // a file the runtime writes itself, then slashes past PATH_MAX
    proc.resize(4096, '/');
    CHECK(open_guest(at_fdcwd, proc.c_str(), o_rdonly) == -enametoolong);
}

// --- Memory (the compatibility plan's gap 1: ActivityManager.getMemoryInfo, sysconf, engines) -----------------------

/// Android 15's /proc/meminfo on the emulator (kernel 6.6, arm64): these lines in this order, each name with its colon
/// padded to 16 characters, the value right-aligned in 8 or more, then " kB" [verified: cat on the emulator,
/// 2026-10-07].
constexpr const char *meminfo_names[] = {
    "MemTotal", "MemFree", "MemAvailable", "Buffers", "Cached", "SwapCached", "Active", "Inactive", "Active(anon)",
    "Inactive(anon)", "Active(file)", "Inactive(file)", "Unevictable", "Mlocked", "SwapTotal", "SwapFree", "Dirty",
    "Writeback", "AnonPages", "Mapped", "Shmem", "KReclaimable", "Slab", "SReclaimable", "SUnreclaim", "KernelStack",
    "ShadowCallStack", "PageTables", "SecPageTables", "NFS_Unstable", "Bounce", "WritebackTmp", "CommitLimit",
    "Committed_AS", "VmallocTotal", "VmallocUsed", "VmallocChunk", "Percpu", "AnonHugePages", "ShmemHugePages",
    "ShmemPmdMapped", "FileHugePages", "FilePmdMapped", "CmaTotal", "CmaFree"};

std::uint64_t host_memory() {
    std::uint64_t bytes = 0;
    std::size_t size = sizeof bytes;
    return ::sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
}

TEST(proc_meminfo_has_android_15s_lines_with_the_hosts_memory) {
    // ActivityManager's MemInfoReader takes MemTotal, MemFree and Cached (availMem is free plus cached), Unity and
    // others MemAvailable: the host's memory, with free <= available <= total.
    const auto lines = lines_of(read_guest_file("/proc/meminfo"));
    CHECK(lines.size() == std::size(meminfo_names));
    std::map<std::string, std::uint64_t> values;
    for (std::size_t index = 0; index < lines.size() && index < std::size(meminfo_names); ++index) {
        const std::string &line = lines[index];
        const std::string name = std::string(meminfo_names[index]) + ":";
        // The value's field: spaces, then digits, at least 8 characters from column 16 to " kB".
        const std::string field = line.size() > 19 ? line.substr(16, line.size() - 19) : std::string();
        const auto digits = field.find_first_not_of(' ');
        CHECK(line.starts_with(name) && line.find_first_not_of(' ', name.size()) >= 16 && line.ends_with(" kB") &&
              field.size() >= 8 && digits != std::string::npos &&
              field.find_first_not_of("0123456789", digits) == std::string::npos);
        values[meminfo_names[index]] = std::strtoull(line.c_str() + 16, nullptr, 10);
    }
    CHECK(values["MemTotal"] == host_memory() / 1024);
    CHECK(values["MemFree"] > 0 && values["MemFree"] <= values["MemAvailable"]);
    CHECK(values["MemAvailable"] <= values["MemTotal"]);
    CHECK(values["Cached"] == values["MemAvailable"] - values["MemFree"]);
    CHECK(values["Buffers"] == 0 && values["SwapTotal"] == 0 && values["SwapFree"] == 0);
    CHECK(values["VmallocTotal"] == 257687552);  // arm64's with 48-bit addresses
    CHECK(open_guest(at_fdcwd, "/proc/meminfo", o_wronly) == -eacces);  // 0444, also for root
}

TEST(sysinfo_gives_linuxs_struct_with_the_memory_of_proc_meminfo) {
    // bionic's sysconf(_SC_PHYS_PAGES) and (_SC_AVPHYS_PAGES) read sysinfo. On 64 bits Linux gives bytes: mem_unit 1.
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
    constexpr std::uint64_t nr_sysinfo = 179;
    LinuxSysinfo info;
    std::memset(&info, 0xff, sizeof info);
    CHECK(call(nr_sysinfo, {address(&info)}) == 0);
    CHECK(info.mem_unit == 1 && info.totalram == host_memory());
    CHECK(info.freeram > 0 && info.freeram <= info.totalram);
    CHECK(info.uptime > 0 && info.procs >= 1);
    CHECK(info.sharedram == 0 && info.bufferram == 0 && info.totalswap == 0 && info.freeswap == 0);
    CHECK(info.totalhigh == 0 && info.freehigh == 0);
    CHECK(call(nr_sysinfo, {0}) == -efault);
}

TEST(proc_self_fd_links_name_what_the_descriptors_refer_to) {
    // bionic's realpath opens the path and asks readlink of /proc/self/fd/<n>; the guest sees its own
    // paths there, and for what has none Linux's names.
    const Scratch scratch;
    const auto file = open_guest(at_fdcwd, "./named", o_rdwr | o_creat | o_cloexec, 0600);
    CHECK(file >= 0);
    const auto link = [](const std::string &path, std::size_t size = 256) {
        std::string buffer(size, '\0');
        const auto length = call(nr_readlinkat, {at_fdcwd, address(path.c_str()), address(buffer.data()), size});
        return length < 0 ? "error " + std::to_string(-length) : buffer.substr(0, static_cast<std::size_t>(length));
    };
    CHECK(link(std::format("/proc/self/fd/{}", file)) == "/data/local/tmp/named");
    CHECK(link(std::format("/proc/{}/fd/{}", ::getpid(), file)) == "/data/local/tmp/named");
    CHECK(link(std::format("/proc/self/fd/{}", file), 9) == "/data/loc");  // cut, as readlink does
    int ends[2];
    guest_pipe(ends);
    CHECK(std::regex_match(link(std::format("/proc/self/fd/{}", ends[0])), std::regex("pipe:\\[[0-9]+\\]")));
    const auto counter = call(nr_eventfd2, {0, 0});
    CHECK(link(std::format("/proc/self/fd/{}", counter)) == "anon_inode:[eventfd]");
    const auto epoll = call(nr_epoll_create1, {0});
    CHECK(link(std::format("/proc/self/fd/{}", epoll)) == "anon_inode:[eventpoll]");
    const auto stat = open_guest(at_fdcwd, "/proc/self/stat", o_rdonly);
    CHECK(link(std::format("/proc/self/fd/{}", stat)) == std::format("/proc/{}/stat", ::getpid()));
    CHECK(link("/proc/self/fd/999") == "error 2");
    for (const auto fd : {file, std::int64_t{ends[0]}, std::int64_t{ends[1]}, counter, epoll, stat}) {
        call(nr_close, {unsigned_of(fd)});
    }
}

// --- Sockets (ADR-0054 step a) ------------------------------------------------------------------------------
// fixtures/suite/socket-test compares the rest with Android 15. Names in the file system are an app's, which the
// shell that runs the fixtures on Android may not make (SELinux), so they are checked here.

constexpr std::uint64_t nr_socketpair = 199, nr_bind = 200, nr_listen = 201, nr_connect = 203, nr_getsockname = 204,
                        nr_getpeername = 205, nr_sendto = 206, nr_recvfrom = 207, nr_accept4 = 242;
constexpr std::int64_t eaddrinuse = 98, econnrefused = 111;

/// Linux's struct sockaddr_un: a 16-bit family, then 108 bytes of path.
struct LinuxUnixName {
    std::uint16_t family = af_unix;
    char path[108] = {};
};

/// A name in the file system and its length as bind takes it, the path's NUL included.
std::pair<LinuxUnixName, std::uint64_t> unix_name(std::string_view path) {
    LinuxUnixName name;
    std::memcpy(name.path, path.data(), path.size());
    return {name, 2 + path.size() + 1};
}

/// getsockname or getpeername of `fd` as bytes.
std::string name_of(std::uint64_t number, std::int64_t fd) {
    LinuxUnixName name;
    std::uint32_t length = sizeof name;
    if (call(number, {unsigned_of(fd), address(&name), address(&length)}) != 0) return "error";
    return std::string(reinterpret_cast<const char *>(&name), length);
}

std::string bytes_of(const std::pair<LinuxUnixName, std::uint64_t> &name) {
    return std::string(reinterpret_cast<const char *>(&name.first), name.second);
}

/// A server bound to `path` and listening, a client connected to it, and the server's side of the connection;
/// checks each step, also the names.
void unix_round(const std::string &path, const std::string &host) {
    const auto name = unix_name(path);
    const auto server = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(server >= 0 && call(nr_bind, {unsigned_of(server), address(&name.first), name.second}) == 0);
    struct stat status{};
    CHECK(::stat(host.c_str(), &status) == 0 && S_ISSOCK(status.st_mode));  // the socket is where the guest put it
    struct LinuxStat {
        std::uint64_t device, inode;
        std::uint32_t mode, links;
        char rest[112];
    } guest{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address(path.c_str()), address(&guest), 0}) == 0 &&
          (guest.mode & 0170000) == 0140000);
    const auto other = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_bind, {unsigned_of(other), address(&name.first), name.second}) == -eaddrinuse);
    call(nr_close, {unsigned_of(other)});
    CHECK(call(nr_listen, {unsigned_of(server), 4}) == 0);
    CHECK(name_of(nr_getsockname, server) == bytes_of(name));
    const auto client = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_connect, {unsigned_of(client), address(&name.first), name.second}) == 0);
    CHECK(name_of(nr_getpeername, client) == bytes_of(name));
    CHECK(name_of(nr_getsockname, client) == std::string("\1\0", 2));  // unnamed: the family alone
    LinuxUnixName peer;
    std::uint32_t peer_length = sizeof peer;
    const auto accepted = call(nr_accept4, {unsigned_of(server), address(&peer), address(&peer_length), 0});
    CHECK(accepted >= 0 && peer_length == 2);
    CHECK(name_of(nr_getsockname, accepted) == bytes_of(name));  // an accepted socket has its listener's name
    CHECK(write_text(client, "unix") == 4 && read_text(accepted, 16) == "unix");
    for (const auto fd : {accepted, client, server}) call(nr_close, {unsigned_of(fd)});
    const auto late = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_connect, {unsigned_of(late), address(&name.first), name.second}) == -econnrefused);
    call(nr_close, {unsigned_of(late)});
    CHECK(call(nr_unlinkat, {at_fdcwd, address(path.c_str()), 0}) == 0);
}

TEST(an_af_unix_name_in_the_file_system_is_the_guests_path) {
    const Scratch scratch;
    unix_round("/data/local/tmp/server.sock", scratch.host + "/server.sock");
    unix_round("relative.sock", scratch.host + "/relative.sock");  // the working directory's, as bound
}

TEST(an_af_unix_name_longer_than_darwins_works) {
    // 107 bytes, the most Linux's sun_path holds with its NUL; on the host far more than Darwin's 104.
    const Scratch scratch;
    const std::string folder = "/data/local/tmp/a-folder-with-a-long-name-for-a-socket";
    CHECK(call(nr_mkdirat, {at_fdcwd, address(folder.c_str()), 0700}) == 0);
    std::string path = folder + "/";
    path += std::string(107 - path.size() - 5, 'n') + ".sock";
    CHECK(path.size() == 107);
    const std::string host = scratch.host + path.substr(std::string("/data/local/tmp").size());
    CHECK(host.size() > 104);
    unix_round(path, host);
}

TEST(af_unix_names_in_the_file_system_answer_as_linux) {
    const Scratch scratch;
    const auto fd = call(nr_socket, {af_unix, sock_stream, 0});
    const auto missing = unix_name("/data/local/tmp/no-folder/x.sock");
    CHECK(call(nr_bind, {unsigned_of(fd), address(&missing.first), missing.second}) == -enoent);
    CHECK(call(nr_connect, {unsigned_of(fd), address(&missing.first), missing.second}) == -enoent);
    const auto file = open_guest(at_fdcwd, "/data/local/tmp/plain", o_creat | o_wronly, 0600);
    call(nr_close, {unsigned_of(file)});
    const auto plain = unix_name("/data/local/tmp/plain");
    CHECK(call(nr_connect, {unsigned_of(fd), address(&plain.first), plain.second}) == -econnrefused);
    CHECK(call(nr_bind, {unsigned_of(fd), address(&plain.first), plain.second}) == -eaddrinuse);
    const auto first = unix_name("/data/local/tmp/first.sock");
    CHECK(call(nr_bind, {unsigned_of(fd), address(&first.first), first.second}) == 0);
    const auto second = unix_name("/data/local/tmp/second.sock");
    CHECK(call(nr_bind, {unsigned_of(fd), address(&second.first), second.second}) == -einval);  // bound already
    call(nr_close, {unsigned_of(fd)});
}

TEST(a_datagram_names_its_sender_by_the_guests_path) {
    const Scratch scratch;
    const auto a = unix_name("/data/local/tmp/a.sock"), b = unix_name("b.sock");
    const auto sender = call(nr_socket, {af_unix, sock_dgram, 0}), receiver = call(nr_socket, {af_unix, sock_dgram, 0});
    CHECK(call(nr_bind, {unsigned_of(sender), address(&a.first), a.second}) == 0);
    CHECK(call(nr_bind, {unsigned_of(receiver), address(&b.first), b.second}) == 0);
    CHECK(call(nr_sendto, {unsigned_of(sender), address("dgram"), 5, 0, address(&b.first), b.second}) == 5);
    char buffer[16];
    LinuxUnixName from;
    std::uint32_t length = sizeof from;
    CHECK(call(nr_recvfrom, {unsigned_of(receiver), address(buffer), sizeof buffer, 0, address(&from), address(&length)}) ==
          5);
    CHECK(std::string(reinterpret_cast<const char *>(&from), length) == bytes_of(a));
    call(nr_close, {unsigned_of(sender)});
    call(nr_close, {unsigned_of(receiver)});
}

TEST(a_connection_closed_before_its_accept_is_accepted) {
    // Linux keeps the connection with its data: accept gives it, then read the data and the end. (Darwin refuses
    // setsockopt on a socket that can neither send nor receive any more, which such a connection is at its accept.)
    const Scratch scratch;
    const auto name = unix_name("/data/local/tmp/early.sock");
    const auto server = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_bind, {unsigned_of(server), address(&name.first), name.second}) == 0);
    CHECK(call(nr_listen, {unsigned_of(server), 4}) == 0);
    const auto client = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_connect, {unsigned_of(client), address(&name.first), name.second}) == 0);
    CHECK(call(nr_write, {unsigned_of(client), address("early"), 5}) == 5);
    call(nr_close, {unsigned_of(client)});
    const auto accepted = call(nr_accept4, {unsigned_of(server), 0, 0, 0});
    CHECK(accepted >= 0);
    char buffer[8];
    CHECK(call(nr_read, {unsigned_of(accepted), address(buffer), sizeof buffer}) == 5);
    CHECK(call(nr_read, {unsigned_of(accepted), address(buffer), sizeof buffer}) == 0);
    call(nr_close, {unsigned_of(accepted)});
    call(nr_close, {unsigned_of(server)});
}

TEST(the_guests_dnsproxyd_is_the_runtimes_resolver) {
    // ADR-0054 step b: libnetd_client's dns_open_proxy connects to /dev/socket/dnsproxyd, which a runtime thread
    // serves with DnsProxyListener's bytes; getpeername names it. Nothing else is there to bind.
    const Scratch scratch;
    CHECK(::mkdir((scratch.host + "/etc").c_str(), 0755) == 0);
    std::FILE *hosts = std::fopen((scratch.host + "/etc/hosts").c_str(), "w");
    CHECK(hosts != nullptr && std::fputs("127.0.0.1 localhost\n", hosts) >= 0 && std::fclose(hosts) == 0);
    apkrt::vfs::mount("/system", scratch.host);
    const auto name = unix_name("/dev/socket/dnsproxyd");
    const auto client = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_connect, {unsigned_of(client), address(&name.first), name.second}) == 0);
    CHECK(name_of(nr_getpeername, client) == bytes_of(name));
    const std::string command("getaddrinfo localhost ^ 0 0 1 0 0", 34);  // with its NUL
    CHECK(call(nr_write, {unsigned_of(client), address(command.data()), command.size()}) ==
          static_cast<std::int64_t>(command.size()));
    std::string reply;
    char buffer[64];
    while (reply.size() < 52) {  // "222", one entry of 16 bytes of address, the end
        const auto got = call(nr_read, {unsigned_of(client), address(buffer), sizeof buffer});
        if (got <= 0) break;
        reply.append(buffer, static_cast<std::size_t>(got));
    }
    CHECK(reply.size() == 52 && reply.starts_with(std::string("222", 4)) &&
          reply.substr(32, 4) == std::string("\x7f\0\0\x01", 4));
    call(nr_close, {unsigned_of(client)});
    const auto walked = unix_name("/dev//socket/./dnsproxyd");  // the same socket by the path walk
    const auto again = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_connect, {unsigned_of(again), address(&walked.first), walked.second}) == 0);
    call(nr_close, {unsigned_of(again)});
    const auto other = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_bind, {unsigned_of(other), address(&name.first), name.second}) == -enoent);
    call(nr_close, {unsigned_of(other)});
}


TEST(an_af_unix_datagram_may_be_as_long_as_on_linux) {
    // Linux takes a datagram of up to its default send buffer less 32 bytes (212960), Darwin one of 2048.
    const Scratch scratch;
    constexpr std::uint64_t msg_dontwait = 0x40;  // so that a datagram that did not go fails instead of waiting
    const std::vector<char> sent(200'000, 'd');
    std::vector<char> received(sent.size() + 1);
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_dgram, 0, address(ends)}) == 0);
    CHECK(call(nr_sendto, {unsigned_of(ends[0]), address(sent.data()), sent.size(), 0, 0, 0}) == 200'000);
    CHECK(call(nr_recvfrom, {unsigned_of(ends[1]), address(received.data()), received.size(), msg_dontwait, 0, 0}) ==
          200'000);
    call(nr_close, {unsigned_of(ends[0])});
    call(nr_close, {unsigned_of(ends[1])});
    const auto b = unix_name("b.sock");
    const auto sender = call(nr_socket, {af_unix, sock_dgram, 0}), receiver = call(nr_socket, {af_unix, sock_dgram, 0});
    CHECK(call(nr_bind, {unsigned_of(receiver), address(&b.first), b.second}) == 0);
    CHECK(call(nr_sendto, {unsigned_of(sender), address(sent.data()), sent.size(), 0, address(&b.first), b.second}) ==
          200'000);
    CHECK(call(nr_recvfrom, {unsigned_of(receiver), address(received.data()), received.size(), msg_dontwait, 0, 0}) ==
          200'000);
    call(nr_close, {unsigned_of(sender)});
    call(nr_close, {unsigned_of(receiver)});
}

TEST(af_unix_datagrams_are_limited_by_the_senders_buffer_alone) {
    // unix_dgram_sendmsg: EMSGSIZE above the sender's sk_sndbuf less 32 bytes, sk_sndbuf being 229376 at first and
    // twice what SO_SNDBUF set, at most twice 262144 (Android 15, socket-test); the receiver's SO_RCVBUF does not count.
    const Scratch scratch;
    constexpr std::uint64_t nr_setsockopt = 208, sol_socket = 1, so_sndbuf = 7, so_rcvbuf = 8, msg_dontwait = 0x40;
    constexpr std::int64_t emsgsize = 90;
    const std::vector<char> sent(524'257, 'd');
    std::vector<char> received(sent.size() + 1);
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_dgram, 0, address(ends)}) == 0);
    const auto passes = [&](std::uint64_t length) {
        const auto result = call(nr_sendto, {unsigned_of(ends[0]), address(sent.data()), length, 0, 0, 0});
        if (result < 0) return result;
        return call(nr_recvfrom, {unsigned_of(ends[1]), address(received.data()), received.size(), msg_dontwait, 0, 0});
    };
    CHECK(passes(229'344) == 229'344);
    CHECK(passes(229'345) == -emsgsize);
    const std::int32_t small = 1024, eight_k = 8192, most = 1 << 30;
    CHECK(call(nr_setsockopt, {unsigned_of(ends[1]), sol_socket, so_rcvbuf, address(&small), sizeof small}) == 0);
    CHECK(call(nr_setsockopt, {unsigned_of(ends[0]), sol_socket, so_sndbuf, address(&eight_k), sizeof eight_k}) == 0);
    CHECK(passes(16'352) == 16'352);
    CHECK(passes(16'353) == -emsgsize);
    CHECK(call(nr_setsockopt, {unsigned_of(ends[0]), sol_socket, so_sndbuf, address(&most), sizeof most}) == 0);
    CHECK(passes(524'256) == 524'256);
    CHECK(passes(524'257) == -emsgsize);
    call(nr_close, {unsigned_of(ends[0])});
    call(nr_close, {unsigned_of(ends[1])});
}

// SOCK_SEQPACKET socketpairs (ADR-0054 step a2): socket-test compares the most with Android 15; these are the cases
// it does not show, from Linux 6.6's af_unix.c and datagram.c.
constexpr std::uint64_t sock_seqpacket = 5, msg_dontwait = 0x40;
constexpr std::int64_t econnreset = 104, epipe = 32;

TEST(a_seqpacket_reset_comes_before_the_messages_left) {
    // A close that left messages unread sets the peer's sk_err, which __skb_try_recv_datagram answers before the
    // queue, and unix_seqpacket_sendmsg before a send: ECONNRESET once, then the messages, then the end.
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_seqpacket, 0, address(ends)}) == 0);
    CHECK(write_text(ends[0], "left") == 4);
    CHECK(write_text(ends[1], "unread") == 6);
    call(nr_close, {unsigned_of(ends[0])});
    char buffer[8];
    CHECK(call(nr_recvfrom, {unsigned_of(ends[1]), address(buffer), sizeof buffer, msg_dontwait, 0, 0}) == -econnreset);
    CHECK(read_text(ends[1], 8) == "left");
    CHECK(call(nr_recvfrom, {unsigned_of(ends[1]), address(buffer), sizeof buffer, msg_dontwait, 0, 0}) == 0);
    call(nr_close, {unsigned_of(ends[1])});
    CHECK(call(nr_socketpair, {af_unix, sock_seqpacket, 0, address(ends)}) == 0);
    CHECK(write_text(ends[1], "unread") == 6);
    call(nr_close, {unsigned_of(ends[0])});
    CHECK(call(nr_sendto, {unsigned_of(ends[1]), address("x"), 1, 0x4000 /* MSG_NOSIGNAL */, 0, 0}) == -econnreset);
    CHECK(call(nr_sendto, {unsigned_of(ends[1]), address("x"), 1, 0x4000, 0, 0}) == -epipe);
    call(nr_close, {unsigned_of(ends[1])});
}

TEST(a_seqpacket_peers_close_ends_a_receive_that_waits) {
    // Darwin wakes the waiting end when its datagram peer closes; Linux's end of the messages follows.
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_seqpacket, 0, address(ends)}) == 0);
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        call(nr_close, {unsigned_of(ends[0])});
    });
    char buffer[8];
    CHECK(call(nr_recvfrom, {unsigned_of(ends[1]), address(buffer), sizeof buffer, 0, 0, 0}) == 0);
    closer.join();
    call(nr_close, {unsigned_of(ends[1])});
}

TEST(a_seqpacket_shut_rd_ends_its_receives_and_the_peers_sends) {
    // unix_shutdown gives the peer SEND_SHUTDOWN for this end's RCV_SHUTDOWN: its sends get EPIPE, and this end's
    // receives the end once nothing is left.
    constexpr std::uint64_t nr_shutdown = 210, shut_rd = 0;
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_seqpacket, 0, address(ends)}) == 0);
    CHECK(write_text(ends[1], "kept") == 4);
    CHECK(call(nr_shutdown, {unsigned_of(ends[0]), shut_rd}) == 0);
    CHECK(read_text(ends[0], 8) == "kept");
    char buffer[8];
    CHECK(call(nr_recvfrom, {unsigned_of(ends[0]), address(buffer), sizeof buffer, msg_dontwait, 0, 0}) == 0);
    CHECK(call(nr_sendto, {unsigned_of(ends[1]), address("x"), 1, 0x4000 /* MSG_NOSIGNAL */, 0, 0}) == -epipe);
    CHECK(write_text(ends[0], "back") == 4);  // the other direction stays open
    CHECK(read_text(ends[1], 8) == "back");
    call(nr_close, {unsigned_of(ends[0])});
    call(nr_close, {unsigned_of(ends[1])});
}

TEST(a_seqpacket_send_ignores_a_name) {
    // unix_seqpacket_sendmsg drops msg_name: the message goes to the peer.
    const Scratch scratch;
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_seqpacket, 0, address(ends)}) == 0);
    const auto nowhere = unix_name("/data/local/tmp/nowhere.sock");
    CHECK(call(nr_sendto, {unsigned_of(ends[0]), address("named"), 5, 0, address(&nowhere.first), nowhere.second}) == 5);
    char buffer[8];
    CHECK(call(nr_recvfrom, {unsigned_of(ends[1]), address(buffer), sizeof buffer, msg_dontwait, 0, 0}) == 5);
    call(nr_close, {unsigned_of(ends[0])});
    call(nr_close, {unsigned_of(ends[1])});
}

/// How long a receive on `waiting` waits until `end` ends it, done 200 ms in by another thread; a close of `rescue`
/// 2 s in ends it otherwise, so that a receive that `end` does not wake fails rather than hangs.
template <typename End>
std::chrono::milliseconds seqpacket_wait(std::int64_t waiting, std::int64_t rescue, End end) {
    std::thread other([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        end();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        call(nr_close, {unsigned_of(rescue)});
    });
    char buffer[8];
    const auto started = std::chrono::steady_clock::now();
    CHECK(call(nr_recvfrom, {unsigned_of(waiting), address(buffer), sizeof buffer, 0, 0, 0}) == 0);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    other.join();
    return waited;
}

TEST(a_seqpacket_shutdown_ends_a_receive_that_waits) {
    // Linux wakes the receive at once, for the other's SHUT_WR as for this end's own SHUT_RD; Darwin wakes nothing
    // for a datagram, the runtime's mark does.
    constexpr std::uint64_t nr_shutdown = 210, shut_rd = 0, shut_wr = 1;
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_seqpacket, 0, address(ends)}) == 0);
    CHECK(seqpacket_wait(ends[1], ends[0], [&] { call(nr_shutdown, {unsigned_of(ends[0]), shut_wr}); }) <
          std::chrono::milliseconds(1500));
    call(nr_close, {unsigned_of(ends[1])});
    CHECK(call(nr_socketpair, {af_unix, sock_seqpacket, 0, address(ends)}) == 0);
    CHECK(seqpacket_wait(ends[0], ends[1], [&] { call(nr_shutdown, {unsigned_of(ends[0]), shut_rd}); }) <
          std::chrono::milliseconds(1500));
    call(nr_close, {unsigned_of(ends[0])});
}

TEST(two_receives_on_an_ended_seqpacket_never_get_the_mark) {
    // The runtime's mark stays queued behind the messages of an end that receives no more: two threads that receive
    // at once get the message and the end, never the mark (unix_dgram_recvmsg receives under u->iolock).
    constexpr std::uint64_t nr_shutdown = 210, shut_wr = 1;
    int wrong = 0;
    for (int round = 0; round < 300; ++round) {
        std::int32_t ends[2] = {-1, -1};
        CHECK(call(nr_socketpair, {af_unix, sock_seqpacket, 0, address(ends)}) == 0);
        CHECK(write_text(ends[0], "data!") == 5);
        CHECK(call(nr_shutdown, {unsigned_of(ends[0]), shut_wr}) == 0);
        std::atomic<int> ready{0};
        std::int64_t results[2] = {-1, -1};
        const auto receive = [&](int which) {
            char buffer[32];
            ++ready;
            while (ready.load() < 2) {
            }
            results[which] = call(nr_recvfrom, {unsigned_of(ends[1]), address(buffer), sizeof buffer, msg_dontwait, 0, 0});
        };
        std::thread other(receive, 1);
        receive(0);
        other.join();
        wrong += !((results[0] == 5 && results[1] == 0) || (results[0] == 0 && results[1] == 5));
        call(nr_close, {unsigned_of(ends[0])});
        call(nr_close, {unsigned_of(ends[1])});
    }
    CHECK(wrong == 0);
}

TEST(getsockopt_without_a_buffer_is_efault) {
    // sk_getsockopt copies the value to the guest's buffer: none is EFAULT, as setsockopt's.
    constexpr std::uint64_t nr_getsockopt = 209, sol_socket = 1, so_type = 3;
    CHECK(in_child([] {
        const auto fd = call(nr_socket, {af_unix, sock_stream, 0});
        std::uint32_t length = 4;
        CHECK(call(nr_getsockopt, {unsigned_of(fd), sol_socket, so_type, 0, address(&length)}) == -efault);
        call(nr_close, {unsigned_of(fd)});
    }));
}

/// Linux's struct msghdr and struct iovec on arm64.
struct TestMsghdr {
    std::uint64_t name = 0;
    std::uint32_t name_length = 0, pad = 0;
    std::uint64_t vectors = 0, vector_count = 0, control = 0, control_length = 0;
    std::int32_t flags = 0, pad2 = 0;
};
struct TestIovec {
    std::uint64_t base, length;
};
constexpr std::uint64_t nr_sendmsg = 211, nr_recvmsg = 212;

/// Sends a byte on `fd` with `fds` in SCM_RIGHTS.
std::int64_t send_rights(std::int64_t fd, std::initializer_list<std::int32_t> fds) {
    char data = 'r';
    TestIovec vector{address(&data), 1};
    alignas(8) unsigned char control[64] = {};
    const std::uint64_t length = 16 + fds.size() * sizeof(std::int32_t);
    const std::int32_t sol_socket = 1, scm_rights = 1;
    std::memcpy(control, &length, sizeof length);
    std::memcpy(control + 8, &sol_socket, sizeof sol_socket);
    std::memcpy(control + 12, &scm_rights, sizeof scm_rights);
    std::memcpy(control + 16, fds.begin(), fds.size() * sizeof(std::int32_t));
    TestMsghdr message;
    message.vectors = address(&vector);
    message.vector_count = 1;
    message.control = address(control);
    message.control_length = (length + 7) & ~std::uint64_t{7};
    return call(nr_sendmsg, {unsigned_of(fd), address(&message), 0});
}

/// A byte received on `fd` with `flags`: the call's result, the control's length, the flags and the descriptors.
struct ReceivedRights {
    std::int64_t result;
    std::uint64_t control_length;
    std::int32_t flags;
    std::vector<std::int32_t> fds;
};

ReceivedRights receive_rights(std::int64_t fd, std::uint64_t flags = 0) {
    char data = 0;
    TestIovec vector{address(&data), 1};
    alignas(8) unsigned char control[64] = {};
    TestMsghdr message;
    message.vectors = address(&vector);
    message.vector_count = 1;
    message.control = address(control);
    message.control_length = sizeof control;
    ReceivedRights out{call(nr_recvmsg, {unsigned_of(fd), address(&message), flags}), message.control_length,
                       message.flags, {}};
    std::uint64_t length = 0;
    if (out.control_length >= 16) std::memcpy(&length, control, sizeof length);
    for (std::uint64_t at = 16; at + sizeof(std::int32_t) <= length && at + sizeof(std::int32_t) <= sizeof control;
         at += sizeof(std::int32_t)) {
        std::int32_t got;
        std::memcpy(&got, control + at, sizeof got);
        out.fds.push_back(got);
    }
    return out;
}

TEST(a_peek_leaves_the_descriptors_of_scm_rights_in_flight) {
    // Darwin's MSG_PEEK gives a message's descriptors as zeros: the runtime passes none on a peek, and the receive
    // after it gets them. (Linux gives copies on a peek, unix_peek_fds: a departure, ADR-0054 step a.)
    CHECK(in_child([] {
        constexpr std::uint64_t msg_peek = 2;
        std::int32_t ends[2] = {-1, -1};
        CHECK(call(nr_socketpair, {af_unix, sock_stream, 0, address(ends)}) == 0);
        int pipe_ends[2];
        guest_pipe(pipe_ends);
        CHECK(write_text(pipe_ends[1], "peek") == 4);
        CHECK(send_rights(ends[0], {pipe_ends[0], pipe_ends[1]}) == 1);
        call(nr_close, {unsigned_of(pipe_ends[0])});
        call(nr_close, {unsigned_of(pipe_ends[1])});
        const auto peeked = receive_rights(ends[1], msg_peek);
        CHECK(peeked.result == 1 && peeked.control_length == 0 && peeked.fds.empty());
        CHECK(::fcntl(0, F_GETFD) != -1);  // the host's descriptor 0 is nobody's to take
        const auto received = receive_rights(ends[1]);
        CHECK(received.result == 1 && received.fds.size() == 2);
        if (received.fds.size() == 2) {
            CHECK(read_text(received.fds[0], 8) == "peek");
            for (const auto fd : received.fds) call(nr_close, {unsigned_of(fd)});
        }
        call(nr_close, {unsigned_of(ends[0])});
        call(nr_close, {unsigned_of(ends[1])});
        CHECK(::fcntl(0, F_GETFD) != -1);
    }));
}

TEST(descriptors_in_flight_arrive_as_sent_also_two_of_one_file) {
    // Two open descriptions of one file, each sent on a socketpair of its own and received in the other order:
    // each arrives as the one sent, with its own offset, as Linux passes the open file itself.
    const Scratch scratch;
    if (FILE *file = std::fopen((scratch.host + "/flight").c_str(), "wb")) {
        std::fputs("0123456789", file);
        std::fclose(file);
    }
    const auto first = open_guest(at_fdcwd, "flight", o_rdonly), second = open_guest(at_fdcwd, "flight", o_rdonly);
    CHECK(call(nr_lseek, {unsigned_of(second), 5, 0 /* SEEK_SET */}) == 5);
    std::int32_t a[2] = {-1, -1}, b[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_stream, 0, address(a)}) == 0);
    CHECK(call(nr_socketpair, {af_unix, sock_stream, 0, address(b)}) == 0);
    CHECK(send_rights(a[0], {static_cast<std::int32_t>(first)}) == 1);
    CHECK(send_rights(b[0], {static_cast<std::int32_t>(second)}) == 1);
    const auto from_b = receive_rights(b[1]), from_a = receive_rights(a[1]);
    CHECK(from_b.fds.size() == 1 && from_a.fds.size() == 1);
    if (from_b.fds.size() == 1 && from_a.fds.size() == 1) {
        CHECK(read_text(from_b.fds[0], 1) == "5");
        CHECK(read_text(from_a.fds[0], 1) == "0");
        call(nr_close, {unsigned_of(from_b.fds[0])});
        call(nr_close, {unsigned_of(from_a.fds[0])});
    }
    for (const auto fd : {first, second, std::int64_t{a[0]}, std::int64_t{a[1]}, std::int64_t{b[0]}, std::int64_t{b[1]}}) {
        call(nr_close, {unsigned_of(fd)});
    }
}

TEST(a_bind_goes_through_no_link_that_the_guest_plants) {
    // The runtime binds a socket under a name of its own before it gives it the guest's: a link that the guest
    // plants there, as at every name of the form it might guess, must not take the socket out of its tree.
    const Scratch scratch;
    char pattern[] = "/tmp/apkrt-outside-XXXXXX";
    const std::string outside = ::mkdtemp(pattern);
    for (int number = 0; number < 512; ++number) {
        const std::string planted = scratch.host + "/.apkrt-" + std::to_string(::getpid()) + "-" + std::to_string(number);
        static_cast<void>(::symlink((outside + "/escaped-" + std::to_string(number)).c_str(), planted.c_str()));
    }
    static_cast<void>(::symlink((outside + "/escaped-zeros").c_str(), (scratch.host + "/.apkrt-" + std::string(32, '0')).c_str()));
    const auto name = unix_name("/data/local/tmp/planted.sock");
    const auto fd = call(nr_socket, {af_unix, sock_stream, 0});
    CHECK(call(nr_bind, {unsigned_of(fd), address(&name.first), name.second}) == 0);
    struct stat status{};
    CHECK(::lstat((scratch.host + "/planted.sock").c_str(), &status) == 0 && S_ISSOCK(status.st_mode));
    int escaped = 0;
    if (DIR *directory = ::opendir(outside.c_str())) {
        while (const dirent *entry = ::readdir(directory)) escaped += entry->d_name[0] != '.';
        ::closedir(directory);
    }
    CHECK(escaped == 0);
    call(nr_close, {unsigned_of(fd)});
    static_cast<void>(std::system(("rm -rf '" + outside + "'").c_str()));
}

TEST(so_rcvtimeo_ends_a_read_of_a_socket_too) {
    // sock_read_iter receives as recvmsg does, so the timeout ends a read with EAGAIN.
    constexpr std::uint64_t nr_setsockopt = 208, sol_socket = 1, so_rcvtimeo = 20;
    const struct {
        std::int64_t seconds, microseconds;
    } timeout{0, 150'000};
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_stream, 0, address(ends)}) == 0);
    CHECK(call(nr_setsockopt, {unsigned_of(ends[0]), sol_socket, so_rcvtimeo, address(&timeout), sizeof timeout}) == 0);
    std::thread late([&] {  // ends a read that waits on regardless, so that it fails instead of hanging
        std::this_thread::sleep_for(std::chrono::seconds(1));
        write_text(ends[1], "late");
    });
    char buffer[8];
    const auto started = std::chrono::steady_clock::now();
    CHECK(call(nr_read, {unsigned_of(ends[0]), address(buffer), sizeof buffer}) == -eagain);
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(140));
    late.join();
    call(nr_close, {unsigned_of(ends[0])});
    call(nr_close, {unsigned_of(ends[1])});
}

TEST(an_interest_in_epollrdhup_alone_sees_the_peers_shutdown) {
    // ep_poll reports what unix_poll sets within the interest: EPOLLRDHUP once the peer sends no more.
    constexpr std::uint64_t nr_epoll_ctl = 21, nr_epoll_pwait = 22, nr_shutdown = 210, epoll_ctl_add = 1, shut_wr = 1;
    constexpr std::uint32_t epollrdhup = 0x2000;
    struct LinuxEpollEvent {
        std::uint32_t events;
        std::uint64_t data;  // aligned: Linux packs the struct only on x86-64
    };
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_socketpair, {af_unix, sock_stream, 0, address(ends)}) == 0);
    const auto epoll = call(nr_epoll_create1, {0});
    LinuxEpollEvent interest{epollrdhup, 7};
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_add, unsigned_of(ends[0]), address(&interest)}) == 0);
    LinuxEpollEvent ready[2] = {};
    CHECK(call(nr_epoll_pwait, {unsigned_of(epoll), address(ready), 2, 0, 0, 8}) == 0);
    CHECK(call(nr_shutdown, {unsigned_of(ends[1]), shut_wr}) == 0);
    CHECK(call(nr_epoll_pwait, {unsigned_of(epoll), address(ready), 2, 1000, 0, 8}) == 1);
    CHECK(ready[0].events == epollrdhup && ready[0].data == 7);
    for (const auto fd : {epoll, std::int64_t{ends[0]}, std::int64_t{ends[1]}}) call(nr_close, {unsigned_of(fd)});
}

TEST(a_bind_of_the_family_alone_gives_an_abstract_name_of_five_hex_digits) {
    // unix_autobind: the family, a NUL and "%05x", 8 bytes.
    const Scratch scratch;
    const auto fd = call(nr_socket, {af_unix, sock_dgram, 0});
    const std::uint16_t family = af_unix;
    CHECK(call(nr_bind, {unsigned_of(fd), address(&family), sizeof family}) == 0);
    const auto name = name_of(nr_getsockname, fd);
    CHECK(name.size() == 8 && name[2] == '\0' && std::all_of(name.begin() + 3, name.end(), [](char digit) {
              return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
          }));
    call(nr_close, {unsigned_of(fd)});
}

TEST(so_acceptconn_tells_a_listening_socket) {
    const Scratch scratch;
    constexpr std::uint64_t nr_getsockopt = 209, sol_socket = 1, so_acceptconn = 30;
    const auto name = unix_name("listening.sock");
    const auto fd = call(nr_socket, {af_unix, sock_stream, 0});
    const auto accepting = [&] {
        std::int32_t value = -1;
        std::uint32_t length = sizeof value;
        CHECK(call(nr_getsockopt, {unsigned_of(fd), sol_socket, so_acceptconn, address(&value), address(&length)}) == 0);
        return value;
    };
    CHECK(call(nr_bind, {unsigned_of(fd), address(&name.first), name.second}) == 0);
    CHECK(accepting() == 0);
    CHECK(call(nr_listen, {unsigned_of(fd), 4}) == 0);
    CHECK(accepting() == 1);
    call(nr_close, {unsigned_of(fd)});
}

TEST(msg_trunc_tells_the_length_of_a_datagram_that_came_during_the_wait) {
    // Darwin's loopback hands a UDP datagram over after sendto returned, so a recv right after it waits; MSG_TRUNC
    // then tells the length of the datagram that came, not of none.
    constexpr std::uint64_t af_inet = 2, msg_trunc = 0x20;
    struct LinuxInetName {
        std::uint16_t family = af_inet;
        std::uint16_t port = 0;
        std::uint8_t address[4] = {127, 0, 0, 1};
        std::uint8_t zero[8] = {};
    } receiver_name;
    const auto receiver = call(nr_socket, {af_inet, sock_dgram, 0}), sender = call(nr_socket, {af_inet, sock_dgram, 0});
    CHECK(call(nr_bind, {unsigned_of(receiver), address(&receiver_name), sizeof receiver_name}) == 0);
    std::uint32_t length = sizeof receiver_name;
    CHECK(call(nr_getsockname, {unsigned_of(receiver), address(&receiver_name), address(&length)}) == 0);
    const std::string sent(100, 'u');
    for (int round = 0; round < 20; ++round) {
        CHECK(call(nr_sendto, {unsigned_of(sender), address(sent.data()), sent.size(), 0, address(&receiver_name),
                               sizeof receiver_name}) == 100);
        char buffer[10];
        CHECK(call(nr_recvfrom, {unsigned_of(receiver), address(buffer), sizeof buffer, msg_trunc, 0, 0}) == 100);
    }
    call(nr_close, {unsigned_of(sender)});
    call(nr_close, {unsigned_of(receiver)});
}

TEST(an_abstract_name_goes_with_its_socket) {
    const Scratch scratch;
    LinuxUnixName name;
    std::memcpy(name.path, "\0abstract-test", 14);
    const std::uint64_t length = 2 + 14;
    for (int round = 0; round < 2; ++round) {  // the name is free again once its socket is closed
        const auto fd = call(nr_socket, {af_unix, sock_stream, 0});
        CHECK(call(nr_bind, {unsigned_of(fd), address(&name), length}) == 0);
        CHECK(name_of(nr_getsockname, fd) == std::string(reinterpret_cast<const char *>(&name), length));
        call(nr_close, {unsigned_of(fd)});
    }
    const auto pair_check = [&] {
        std::int32_t ends[2] = {-1, -1};
        CHECK(call(nr_socketpair, {af_unix, sock_stream, 0, address(ends)}) == 0);
        CHECK(write_text(ends[0], "pair") == 4 && read_text(ends[1], 8) == "pair");
        call(nr_close, {unsigned_of(ends[0])});
        call(nr_close, {unsigned_of(ends[1])});
    };
    pair_check();
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
