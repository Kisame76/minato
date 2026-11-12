#ifndef APKRT_AUDIO_ENCODE_H
#define APKRT_AUDIO_ENCODE_H

// The host's audio encoding (MediaRecorder's audio), APKRT_NR_AUDIO_ENCODE: AudioToolbox's ExtAudioFile encodes the
// guest's PCM frames to AAC-LC in an MPEG-4 file a chunk at a time, handing the file's bytes back as records.

#include <cstdint>

namespace apkrt::audio_encode {

/// APKRT_NR_AUDIO_ENCODE: `op` with its argument (guest_interface.h: an id or the address of an apkrt_audio_encode_*).
long call(std::uint64_t op, std::uint64_t argument);

}  // namespace apkrt::audio_encode

#endif  // APKRT_AUDIO_ENCODE_H
