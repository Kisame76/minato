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
#endif
