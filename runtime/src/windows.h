// The window registry (ADR-0028, decisions 6 and 7): the host's windows with their layers and gates, the
// process's background state, "scheduled" and the windows' buffers (ADR-0043, decision 3). The host's side is
// apkrt.h; the guest's is window_gate, window_size, window_lock and window_post.
#pragma once

#include <cstdint>

namespace apkrt::windows {

/// The private syscall window_gate (guest_interface.h): 0 to go on, 1 when the window is gone, -EINTR
/// when a deliverable signal ends the wait, -EINVAL for an ID or a kind it does not take.
long gate(std::uint64_t id, std::uint64_t kind);

/// The private syscall window_size (guest_interface.h, ADR-0038 c): the window's size as of now into the two int32_t
/// at `address`, both under the registry's lock. 0, -EINVAL for a window that is removed or unknown, -EFAULT for
/// address 0.
long size(std::uint64_t id, std::uint64_t address);

/// The private syscall window_lock (guest_interface.h, ADR-0043 decision 3): the windows' buffers fill the struct at
/// `address`. The registry's checks under its lock, the buffers' call outside it.
long lock_buffer(std::uint64_t id, std::uint64_t address);

/// The private syscall window_post (guest_interface.h, ADR-0043 decision 3): the registry's checks under its lock,
/// the buffers' post outside it, also for a window removed meanwhile.
long post_buffer(std::uint64_t id);

/// ANativeWindow_setFrameRate (ADR-0036, decision 2): keeps `bits`, a float's, as the frame rate of the window
/// `id`, and when they changed calls `report` with them under the registry's lock, so that no report of a
/// window comes once apkrt_window_remove has returned. Nothing for a window that is removed or unknown.
void set_frame_rate(std::uint64_t id, std::uint32_t bits, void (*report)(std::uint32_t bits));

/// The windows' buffers' trim, if they have one, outside the registry's lock: apkrt_session_post of
/// APKRT_EVENT_TRIM_MEMORY at TRIM_MEMORY_UI_HIDDEN or above.
void trim_buffers();

}  // namespace apkrt::windows
