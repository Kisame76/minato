/* graphics/host's drawing of a video image into the guest's surfaces (ADR-0060 e2c and e2c3), which the video
   decoders (video_decoder.m) and the media players (media_player.m, ADR-0060 e2e1) share: an NV12 image ('420v' or
   '420f', IOSurface-backed) into a hardware buffer of the guest's or into a host window, as video_decoder.m's comment
   has it. Both run on the guest's thread inside its syscall, never throw, and answer -EIO in the background. */
#pragma once

#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdint.h>

#include "APKRuntime/apkrt.h"

/* The image drawn into the target, scaled to its size: 0; -EINVAL for a target of another table, more than one
   layer, a format the host does not draw into or a surface Metal cannot wrap; -EIO. A YUV 420 target's copy takes
   `*transfer`, a VTPixelTransferSession made at the first, which the caller keeps and releases. */
int minato_video_frame_render(CVPixelBufferRef image, const struct apkrt_video_target *target,
                              VTPixelTransferSessionRef *transfer);

/* The image drawn into the host window of ID `window`, presented at `time_ns` (CLOCK_UPTIME_RAW; 0 at once) and
   scaled by `flags` (APKRT_VIDEO_DECODER_CROP or 0): 0; -EINVAL for other flags, a window without a Metal layer or
   an image Metal cannot wrap; -ENODEV for a window that is not there; -EIO. `*cropped_window` is the window the
   caller's last render left cropped, given back stretched at a switch to another and updated. */
int minato_video_frame_render_window(CVPixelBufferRef image, uint64_t window, int64_t time_ns, uint32_t flags,
                                     uint64_t *cropped_window);

/* The window given back stretched, Core Animation's default, when a render left it cropped (the caller's end). */
void minato_video_frame_give_back_gravity(uint64_t window);
