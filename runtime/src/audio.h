// The audio output streams (ADR-0036, decisions 5 and 6): each stream's ring of interleaved float32 frames and its
// state live in host memory, where the host's render block reads them (apkrt.h) and the guest reaches them through
// the private syscalls of guest_interface.h.
#pragma once

#include <cstdint>

namespace apkrt::audio {

/// The private syscalls audio_open to audio_join (guest_interface.h).
long open(std::uint64_t config);
long close(std::uint64_t handle);
long control(std::uint64_t handle, std::uint64_t command, std::int64_t argument);
long write(std::uint64_t handle, std::uint64_t frames_address, std::int64_t frames, std::int64_t deadline);
long status(std::uint64_t handle, std::uint64_t address);
long wait(std::uint64_t handle, std::uint32_t sequence, std::int64_t deadline);
long join(std::uint64_t handle, std::uint64_t join);
/// audio_tap: the last `frames` frames (1 to 2048) of what the started output streams played, mixed to mono at 48 kHz
/// and clipped, oldest first, for the Visualizer; the frames before a stream's first one are zeros. -EFAULT, -EINVAL.
long tap(std::uint64_t buffer_address, std::int64_t frames);
/// audio_effect: one kind of effect of an audio session (the real audio effects), played by the render block.
long effect(std::uint64_t address);
/// audio_read (ADR-0064).
long read(std::uint64_t handle, std::uint64_t buffer_address, std::int64_t frames, std::int64_t deadline);

/// The microphone grant was revoked: every open input stream is DISCONNECTED (its reads answer -EPIPE) and the host's
/// input closes, so that the capture ends with the grant. The guest's close then only frees the stream.
void revoke_input();

}  // namespace apkrt::audio
