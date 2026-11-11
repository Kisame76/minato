/* The probe of what iOS lets an app do with Metal in the background (ADR-0029, decision 15; see minato_graphics.h).
   Host code only. Its steps:
   - begin, in the foreground: a command buffer that works, then command buffer A, made and partly encoded;
   - background, after didEnterBackground has returned: more of A (a render pass and a read-back), command buffer B
     made and encoded there (an upload from a staging buffer into a texture and its read-back), neither committed;
     and the counter-case, a command buffer committed there on a queue of its own, with its status and error;
   - end, in the foreground again: A and B committed in that order, their results checked, and the counter-case's queue
     tried once more.
   On the Mac, where nothing restricts the GPU, the steps run one after another and the counter-case completes. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>
#include <string.h>

#include "minato_graphics.h"

enum { kSide = 16, kBytes = kSide * kSide * 4, kFill = 4096 };

static id<MTLDevice> device;
static id<MTLCommandQueue> queue, other_queue;
static id<MTLBuffer> filled, cleared, uploaded, staging, scratch;  // scratch: the counter-case's queue only
static id<MTLTexture> target, upload_target;
static id<MTLCommandBuffer> buffer_a, buffer_b;
static int begun, background_done, encoded_a, encoded_b;

static id<MTLTexture> texture(MTLTextureUsage usage) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:kSide
                                                                                         height:kSide
                                                                                      mipmapped:NO];
    descriptor.usage = usage;
    descriptor.storageMode = MTLStorageModePrivate;
    return [device newTextureWithDescriptor:descriptor];
}

/* A command buffer's end: "completed", or its status and error after at most `seconds`. */
static const char *awaited(id<MTLCommandBuffer> buffer, double seconds, char *text, size_t size) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> ended) {
        (void)ended;
        dispatch_semaphore_signal(done);
    }];
    [buffer commit];
    const dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC));
    const long late = dispatch_semaphore_wait(done, deadline);
    if (late != 0) {
        snprintf(text, size, "no end within %.0f s (status %ld)", seconds, (long)buffer.status);
    } else if (buffer.status == MTLCommandBufferStatusCompleted) {
        snprintf(text, size, "completed");
    } else {
        NSError *error = buffer.error;
        snprintf(text, size, "status %ld, error %s %ld (%s)", (long)buffer.status,
                 error != nil ? error.domain.UTF8String : "none", error != nil ? (long)error.code : 0L,
                 error != nil ? error.localizedDescription.UTF8String : "");
    }
    return text;
}

static int all_bytes(id<MTLBuffer> buffer, size_t length, uint8_t value) {
    const uint8_t *bytes = buffer.contents;
    for (size_t index = 0; index < length; index++) {
        if (bytes[index] != value) {
            return 0;
        }
    }
    return 1;
}

/* Every pixel of a BGRA8 read-back is `b g r a`. */
static int all_pixels(id<MTLBuffer> buffer, const uint8_t pixel[4]) {
    const uint8_t *bytes = buffer.contents;
    for (size_t index = 0; index < kBytes; index += 4) {
        if (memcmp(bytes + index, pixel, 4) != 0) {
            return 0;
        }
    }
    return 1;
}

static void read_back(id<MTLCommandBuffer> buffer, id<MTLTexture> from, id<MTLBuffer> to) {
    id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
    [blit copyFromTexture:from
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(kSide, kSide, 1)
                        toBuffer:to
               destinationOffset:0
          destinationBytesPerRow:kSide * 4
        destinationBytesPerImage:kBytes];
    [blit endEncoding];
}

int minato_background_commit_probe_begin(void) {
    @autoreleasepool {
        device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            printf("background commit probe: no Metal device\n");
            return 1;
        }
        queue = [device newCommandQueue];
        other_queue = [device newCommandQueue];
        filled = [device newBufferWithLength:kFill options:MTLResourceStorageModeShared];
        cleared = [device newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
        uploaded = [device newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
        staging = [device newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
        scratch = [device newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
        target = texture(MTLTextureUsageRenderTarget);
        upload_target = texture(MTLTextureUsageShaderRead);

        char text[256];
        id<MTLCommandBuffer> first = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [first blitCommandEncoder];
        [blit fillBuffer:filled range:NSMakeRange(0, kFill) value:0x11];
        [blit endEncoding];
        awaited(first, 5.0, text, sizeof text);
        const int works = strcmp(text, "completed") == 0 && all_bytes(filled, kFill, 0x11);
        printf("background commit probe: foreground, a command buffer: %s, its result right: %s\n", text,
               works ? "yes" : "no");

        buffer_a = [queue commandBuffer];
        blit = [buffer_a blitCommandEncoder];
        [blit fillBuffer:filled range:NSMakeRange(0, kFill) value:0x5A];
        [blit endEncoding];
        printf("background commit probe: foreground, command buffer A made and its fill encoded, not committed\n");
        fflush(stdout);
        begun = works;
        return works ? 0 : 1;
    }
}

void minato_background_commit_probe_background(double since) {
    @autoreleasepool {
        if (!begun) {
            return;
        }
        // A: a render pass that clears the target to 64 128 192 255 (RGBA), then its read-back.
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(64 / 255.0, 128 / 255.0, 192 / 255.0, 1.0);
        id<MTLRenderCommandEncoder> render = [buffer_a renderCommandEncoderWithDescriptor:pass];
        encoded_a = render != nil;
        [render endEncoding];
        if (encoded_a) {
            read_back(buffer_a, target, cleared);
        }

        // B, made in the background: an upload of the staging buffer into a texture, as ANGLE's texture uploads do,
        // and its read-back.
        uint8_t *bytes = staging.contents;
        for (size_t index = 0; index < kBytes; index += 4) {
            bytes[index] = 10, bytes[index + 1] = 20, bytes[index + 2] = 30, bytes[index + 3] = 40;
        }
        buffer_b = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [buffer_b blitCommandEncoder];
        encoded_b = buffer_b != nil && blit != nil;
        [blit copyFromBuffer:staging
                   sourceOffset:0
              sourceBytesPerRow:kSide * 4
            sourceBytesPerImage:kBytes
                     sourceSize:MTLSizeMake(kSide, kSide, 1)
                      toTexture:upload_target
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        if (encoded_b) {
            read_back(buffer_b, upload_target, uploaded);
        }
        printf("background commit probe: background (%.1f s after its notification), A encoded on: %s, B made and "
               "encoded: %s, neither committed\n", since, encoded_a ? "yes" : "no", encoded_b ? "yes" : "no");

        // The counter-case: a commit in the background, on a queue of its own.
        char text[256];
        id<MTLCommandBuffer> refused = [other_queue commandBuffer];
        blit = [refused blitCommandEncoder];
        [blit fillBuffer:scratch range:NSMakeRange(0, kBytes) value:0x77];
        [blit endEncoding];
        printf("background commit probe: background, a command buffer committed there: %s\n",
               awaited(refused, 3.0, text, sizeof text));
        fflush(stdout);
        background_done = 1;
    }
}

int minato_background_commit_probe_end(void) {
    @autoreleasepool {
        if (!begun || !background_done) {
            printf("background commit probe: the background steps did not run\n");
            return 1;
        }
        char text_a[256], text_b[256], text_other[256];
        // Committed in their order, as a FIFO of deferred commits would be at the foreground.
        awaited(buffer_a, 5.0, text_a, sizeof text_a);
        awaited(buffer_b, 5.0, text_b, sizeof text_b);
        static const uint8_t clear[4] = {192, 128, 64, 255}, upload[4] = {10, 20, 30, 40};
        const int right_a = strcmp(text_a, "completed") == 0 && all_bytes(filled, kFill, 0x5A) &&
                            encoded_a && all_pixels(cleared, clear);
        const int right_b = strcmp(text_b, "completed") == 0 && encoded_b && all_pixels(uploaded, upload);
        printf("background commit probe: foreground again, A committed: %s, its results right: %s; B committed: %s, "
               "its result right: %s\n", text_a, right_a ? "yes" : "no", text_b, right_b ? "yes" : "no");

        id<MTLCommandBuffer> again = [other_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [again blitCommandEncoder];
        [blit fillBuffer:scratch range:NSMakeRange(0, kBytes) value:0x99];
        [blit endEncoding];
        awaited(again, 5.0, text_other, sizeof text_other);
        const int queue_works = strcmp(text_other, "completed") == 0 && all_bytes(scratch, kBytes, 0x99);
        printf("background commit probe: foreground again, the counter-case's queue: %s, its result right: %s\n",
               text_other, queue_works ? "yes" : "no");
        printf("background commit probe: %s\n", right_a && right_b && queue_works ? "passed" : "failed");
        fflush(stdout);
        return right_a && right_b && queue_works ? 0 : 1;
    }
}
