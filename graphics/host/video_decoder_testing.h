/* graphics.video_decoder's way into graphics/host's video decoders (video_decoder.m, ADR-0060 e2b3): VideoToolbox's
   decode call replaced, so that a test makes the answers iOS gives seldom (a session ended in the background, frames
   whose call back never comes, an exception) at will. Only graphics.video_decoder_asan's own build of video_decoder.m
   has it (MINATO_VIDEO_DECODER_TESTING), not the library the app links. */
#pragma once

#include <VideoToolbox/VideoToolbox.h>

typedef OSStatus (*minato_video_decode_function)(VTDecompressionSessionRef session, CMSampleBufferRef sample,
                                                 VTDecodeFrameFlags flags, void *frame_context,
                                                 VTDecodeInfoFlags *info);

#ifdef MINATO_VIDEO_DECODER_TESTING
/* Every later decode goes to `decode`, or to VideoToolbox's VTDecompressionSessionDecodeFrame again for NULL. */
void minato_video_decoder_set_decode_for_testing(minato_video_decode_function decode);
/* The Metal pass of render (ADR-0060 e2c2) from `image`, an NV12 image, into `target`, an 'RGBA' IOSurface, so that a
   test hands it surfaces no hardware buffer of graphics/host's is: 0, or the errno render answers. */
int minato_video_decoder_convert_for_testing(CVPixelBufferRef image, IOSurfaceRef target);
/* The decoders' provider of "scheduled" (apkrt_add_scheduled_provider): 1 once no render is in flight, 0 at
   `deadline` (CLOCK_UPTIME_RAW's nanoseconds) first. */
int minato_video_decoder_scheduled_for_testing(uint64_t deadline);
/* The time the last render into a window presented its drawable at (CLOCK_UPTIME_RAW's nanoseconds), 0 when it
   presented at once (ADR-0060 e2c3). */
int64_t minato_video_decoder_window_time_for_testing(void);
#ifdef __OBJC__
#import <Metal/Metal.h>
/* The texture of the drawable the last render into a window drew, kept until the next one (ADR-0060 e2c3), so that a
   test reads its pixels back from a layer whose framebufferOnly it turned off; nil before the first. */
id<MTLTexture> minato_video_decoder_window_texture_for_testing(void);
#endif
#endif
