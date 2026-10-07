// The files of /proc and /sys that the runtime writes for the guest (ADR-0033, decision 8): what bionic,
// ART and apps read there about the process and the device. Each opens as a regular file that holds what
// Linux would show at that moment; the links of /proc/self/fd name what the guest's descriptors refer to.
// /proc/self/maps and smaps name the main stack, the guest's ELF objects and the files it maps.
// Also /linkerconfig/apex.libraries.config.txt, which linkerconfig writes at boot on Android (ADR-0033 e2b3).
#pragma once

#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace apkrt::proc {

/// The main thread's stack, [low, high), and the stack pointer the program started with:
/// /proc/self/maps names that range [stack], and the startstack field of /proc/self/stat points into
/// it, which is where bionic's pthread_getattr_np looks for the main thread's stack.
void set_main_stack(std::uint64_t low, std::uint64_t high, std::uint64_t start);

/// The program's main thread, whose name /proc/self/stat shows. Until the thread names itself, its
/// name is `program`, the name of the program's file, as Linux's exec sets it.
void set_main_thread(pthread_t thread, std::string_view program);

/// A file the guest maps with sys_mmap through its descriptor `fd`, at [begin, end) from file offset `offset`:
/// /proc/self/maps and smaps name that range by the file's guest path, with its device and inode, as Linux shows
/// a file's mapping, until an unmap, a mapping over it or mremap changes the range.
void map_file(std::uint64_t begin, std::uint64_t end, int fd, std::uint64_t offset);

/// [begin, end) is unmapped, or another mapping replaces it: the names of files there go.
void unmap(std::uint64_t begin, std::uint64_t end);

/// mremap moved [from, from + old_length) to `to` with `new_length`: the names go with the pages they name, the
/// part past `new_length` loses its names, and what lay at the target before is replaced.
void remap(std::uint64_t from, std::uint64_t old_length, std::uint64_t to, std::uint64_t new_length);

/// openat of `path`, an absolute guest path in normal form: for a file the runtime writes, the guest's
/// new descriptor or -errno; nullopt for every other path.
std::optional<long> open(std::string_view path, int linux_flags);

/// readlinkat of `path`, an absolute guest path in normal form: for a link of /proc, the length of
/// the target written to `buffer`, cut at `size` and not terminated, as readlink writes it, or -errno;
/// nullopt for every other path.
std::optional<long> read_link(std::string_view path, char *buffer, std::size_t size);

/// sysinfo(2): Linux's struct sysinfo at `address`, with the memory /proc/meminfo shows. 0, or -EFAULT for NULL.
long sysinfo(std::uint64_t address);

}  // namespace apkrt::proc
