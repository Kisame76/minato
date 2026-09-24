// Guest threads (M2 step 5c, ARCHITECTURE.md 4.5): every guest thread is a Darwin pthread that runs
// guest code on the guest's own stack. The program's first thread has the process ID as its thread
// ID, as on Linux; clone creates the others. SYS_exit ends one thread; the process ends when the
// last one does, with the first thread's exit code, or at once with exit_group.
#pragma once

#include "guest_context.h"
#include "signals.h"
#include "syscall_frame.h"

#include <functional>

#include <cstdint>

namespace apkrt::threads {

struct GuestThread {
    // First, so that syscall_entry.S finds in_runtime at a fixed offset from the thread.
    signals::ThreadSignals signals;
    std::int32_t tid = 0;
    std::uint64_t thread_pointer = 0;   // the guest thread pointer it starts with
    std::uint64_t clear_child_tid = 0;  // cleared and woken at exit (CLONE_CHILD_CLEARTID, set_tid_address)
    GuestContext start{};               // where a clone child starts
    apkrt_host_context exit_point{};    // SYS_exit returns here, to the host's stack
    int exit_code = 0;
    // A range the thread unmapped while it still ran on it, its own stack: unmapped once it is off it.
    std::uint64_t deferred_unmap_start = 0;
    std::uint64_t deferred_unmap_length = 0;
    void *host_signal_stack = nullptr;  // where the runtime's signal handlers run for this thread
};
static_assert(offsetof(GuestThread, signals) == 0, "syscall_entry.S");

/// The guest thread the calling host thread is running, or null for a host thread.
GuestThread *current();

/// Runs `enter` as the guest thread `thread` on the calling host thread; `enter` switches to guest
/// code and never returns. Comes back when the guest thread ends with SYS_exit: then the thread's
/// deferred unmap is done, its clear_child_tid word cleared and woken, and the process ends if it
/// was the last guest thread.
void run(GuestThread &thread, void (*enter)(void *), void *argument);

/// A new thread ID; the first thread has the process ID.
std::int32_t new_tid();

/// Makes `thread` findable by its ID (tgkill) and counts it among the living threads.
void add(GuestThread &thread);

/// Undoes add() for a thread that never started.
void forget(GuestThread &thread);

/// Calls `visit` with the living guest thread `tid`, if there is one, while it cannot end.
void find(std::int32_t tid, const std::function<void(GuestThread &)> &visit);

/// Calls `visit` with every living guest thread, while none can end.
void for_each(const std::function<void(GuestThread &)> &visit);

/// Ends the calling guest thread (SYS_exit): back to run(), off the guest's stack. Never returns.
[[noreturn]] void exit_current(int code);

}  // namespace apkrt::threads
