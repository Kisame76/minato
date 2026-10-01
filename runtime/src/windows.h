// The window registry (ADR-0028, decisions 6 and 7): the host's windows with their layers and gates, the
// process's background state and "scheduled". The host's side is apkrt.h; the guest's is window_gate.
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

/// ANativeWindow_setFrameRate (ADR-0036, decision 2): keeps `bits`, a float's, as the frame rate of the window
/// `id`, and when they changed calls `report` with them under the registry's lock, so that no report of a
/// window comes once apkrt_window_remove has returned. Nothing for a window that is removed or unknown.
void set_frame_rate(std::uint64_t id, std::uint32_t bits, void (*report)(std::uint32_t bits));

}  // namespace apkrt::windows
