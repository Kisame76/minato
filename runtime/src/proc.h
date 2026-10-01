// The files of /proc and /sys that the runtime writes for the guest (ADR-0033, decision 8): what bionic,
// ART and apps read there about the process and the device. Each opens as a regular file that holds what
// Linux would show at that moment; the links of /proc/self/fd name what the guest's descriptors refer to.
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

/// openat of `path`, an absolute guest path in normal form: for a file the runtime writes, the guest's
/// new descriptor or -errno; nullopt for every other path.
std::optional<long> open(std::string_view path, int linux_flags);

/// readlinkat of `path`, an absolute guest path in normal form: for a link of /proc, the length of
/// the target written to `buffer`, cut at `size` and not terminated, as readlink writes it, or -errno;
/// nullopt for every other path.
std::optional<long> read_link(std::string_view path, char *buffer, std::size_t size);

}  // namespace apkrt::proc
