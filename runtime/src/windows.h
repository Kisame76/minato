// The window registry (ADR-0028, decisions 6 and 7): the host's windows with their layers and gates, the
// process's background state and "scheduled". The host's side is apkrt.h; the guest's is window_gate.
#pragma once

#include <cstdint>

namespace apkrt::windows {

/// The private syscall window_gate (guest_interface.h): 0 to go on, 1 when the window is gone, -EINTR
/// when a deliverable signal ends the wait, -EINVAL for an ID or a kind it does not take.
long gate(std::uint64_t id, std::uint64_t kind);

}  // namespace apkrt::windows
