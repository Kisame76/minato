// The guest's video decoders (ADR-0060 e2b3): the process's decoders by the runtime's IDs; the host's decoders
// (apkrt.h's apkrt_video_decoders) decode.
#pragma once

#include <cstdint>

namespace apkrt::video_decoders {

/// video_decoder (guest_interface.h): `op` with its argument, an ID or the address of a struct apkrt_video_decoder.
long call(std::uint64_t op, std::uint64_t argument);

}  // namespace apkrt::video_decoders
