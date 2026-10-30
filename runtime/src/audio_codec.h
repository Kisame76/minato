#ifndef APKRT_AUDIO_CODEC_H
#define APKRT_AUDIO_CODEC_H

// The guest's audio codecs (ADR-0060 e2f), APKRT_NR_AUDIO_CODEC: MediaCodec's audio decoders, one unit at a time on
// AudioToolbox's AudioConverter, by the runtime's IDs.

#include <cstdint>

namespace apkrt::audio_codec {

/// APKRT_NR_AUDIO_CODEC (guest_interface.h): `op` with its argument, an ID or the address of a struct
/// apkrt_audio_codec.
long call(std::uint64_t op, std::uint64_t argument);

}  // namespace apkrt::audio_codec

#endif  // APKRT_AUDIO_CODEC_H
