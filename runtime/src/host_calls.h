// Host code on guest threads (ADR-0030): the private syscall "bind host table" with the providers
// graphics/host sets (decision 3). The entry apkrt_host_call and apkrt_call_guest are in
// syscall_entry.S, the switches of in_runtime in signals.cpp.
#pragma once

#include <cstdint>

namespace apkrt::host_calls {

/// The syscall APKRT_NR_BIND_HOST_TABLE (APKRuntime/guest_interface.h): hands the table to the
/// provider set for `name`, as host code, and puts the entry into slot 0 once the provider filled
/// the others. 0, -ENOENT without a provider, or -EINVAL.
long bind_host_table(std::uint64_t name, std::uint64_t table, std::uint64_t length, std::uint64_t hash);

/// Whether a provider is set for `name` (apkrt_set_host_provider).
bool has_provider(const char *name);

}  // namespace apkrt::host_calls

namespace apkrt::signals {
struct ThreadSignals;
}

/// Called by apkrt_host_call (syscall_entry.S): drops the entries of the calling thread's stack of
/// host calls that a longjmp out of a callback left behind, before a call made at `stack_pointer`.
extern "C" void apkrt_drop_stale_host_calls(apkrt::signals::ThreadSignals *state, std::uint64_t stack_pointer);

/// Called by apkrt_host_call when its stack of host calls is full: reports it and traps.
extern "C" [[noreturn]] void apkrt_host_calls_too_deep(void);
