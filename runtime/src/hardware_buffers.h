// The guest's hardware buffers (ADR-0058 a)): the process's AHardwareBuffers by the runtime's IDs, with their
// description, references and locks; the host's buffers (apkrt.h's apkrt_hardware_buffers) hold their memory.
#pragma once

#include <cstdint>

namespace apkrt::hardware_buffers {

/// hardware_buffer (guest_interface.h): `op` with its argument, an ID or the address of a struct
/// apkrt_hardware_buffer.
long call(std::uint64_t op, std::uint64_t argument);

}  // namespace apkrt::hardware_buffers
