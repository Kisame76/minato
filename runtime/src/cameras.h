// The guest's cameras (ADR-0073): the open cameras by the runtime's IDs; the host's cameras (apkrt.h's apkrt_cameras)
// capture.
#pragma once

#include <cstdint>

namespace apkrt::cameras {

/// camera (guest_interface.h): `op` with the address of its struct apkrt_camera.
long call(std::uint64_t op, std::uint64_t argument);

}  // namespace apkrt::cameras
