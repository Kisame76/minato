#ifndef APKRT_AUDIO_DECODE_H
#define APKRT_AUDIO_DECODE_H

// The host's audio decoding (the SoundPool plan's step 3), APKRT_NR_AUDIO_DECODE: AudioToolbox's ExtAudioFile on the
// guest's file bytes, for the formats Android's SoundPool decodes that the guest does not decode itself.

#include <cstdint>

namespace apkrt::audio_decode {

/// APKRT_NR_AUDIO_DECODE: `argument` the guest's struct apkrt_audio_decode (guest_interface.h).
long call(std::uint64_t argument);

}  // namespace apkrt::audio_decode

#endif  // APKRT_AUDIO_DECODE_H
