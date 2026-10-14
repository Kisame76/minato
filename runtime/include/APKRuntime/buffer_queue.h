/* libnativewindow's buffer queue (ADR-0058 c)): the queue behind a SurfaceTexture, in place of libgui's BufferQueue,
   for libandroid_runtime's SurfaceTexture, libandroid's ASurfaceTexture and libEGL. Its producer is an ANativeWindow
   (a queue window), which Surface(SurfaceTexture) holds; its consumer is the SurfaceTexture. Guest code; every
   function is in libnativewindow.so, version APKRT_PRIVATE, and reached through dlsym, as the NDK's stub lacks it.
   - Three slots, each an AHardwareBuffer of the window's size and format (ANativeWindow_setBuffersGeometry, else
     the default size of minato_BufferQueue_setDefaultSize and RGBA_8888), GPU_SAMPLED_IMAGE, GPU_FRAMEBUFFER and the
     CPU's, made at a slot's first dequeue and again when the size or the format changes.
   - A queued frame replaces one still queued, which is dropped, as an app's SurfaceTexture fed by an app's producer
     has it on Android 15 (BufferQueue's mQueueBufferCanDrop): `available` comes for the first frame of a run only,
     and the consumer acquires the newest. A dequeue waits only while every slot is busy; the consumer's last acquired
     frame stays its own until the next acquire's release.
   - Abandoned (minato_BufferQueue_abandon, SurfaceTexture.release), the producer's calls answer -ENODEV, NO_INIT,
     libgui's answer, and a waiting dequeue wakes with it.
   The queue lives while the window and the consumer hold it. */
#ifndef APKRUNTIME_BUFFER_QUEUE_H
#define APKRUNTIME_BUFFER_QUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AHardwareBuffer;
struct ANativeWindow;
struct minato_BufferQueue;

/* Every slot's index is below this, BufferQueue's NUM_BUFFER_SLOTS: a consumer or a producer that keeps something per
   slot keeps this many, as an ImageWriter's maxImages and an ImageReader's grow a queue past its three slots. */
#define MINATO_BUFFER_QUEUE_MAX_SLOTS 64

/* A frame as the consumer acquires it: its slot, its buffer (the slot's, which the consumer does not release), the
   producer's timestamp (CLOCK_MONOTONIC in nanoseconds, the queue's time unless the producer gave one), its transform
   (ANATIVEWINDOW_TRANSFORM_*), data space (the window's, or an ImageWriter's per frame; 0 for none) and crop (all 0
   for none). */
struct minato_BufferQueue_frame {
    int32_t slot;
    struct AHardwareBuffer *buffer;
    int64_t timestamp;
    int32_t transform;
    int32_t data_space;
    int32_t crop_left, crop_top, crop_right, crop_bottom;
};

/* What a producer queues a frame with, as minato_BufferQueue_frame has it; timestamp 0 for the queue's time. */
struct minato_BufferQueue_settings {
    int64_t timestamp;
    int32_t transform;
    int32_t data_space;
    int32_t crop_left, crop_top, crop_right, crop_bottom;
};

/* Called after each queued frame on the producer's thread, outside the queue's lock; and once the queue goes, after
   both the window and the consumer let it go, so that `context` lives as long as any call of `available` may run. */
typedef void (*minato_BufferQueue_available)(void *context);
typedef void (*minato_BufferQueue_destroy)(void *context);

/* A new queue with its producer window, which holds one reference for the caller (ANativeWindow_release lets it go),
   and its consumer, which minato_BufferQueue_release lets go; `available` for each frame and `destroy` at its end,
   each with `context`, or NULL. Returns 0, -EINVAL for a NULL `producer` or `consumer`, or -ENOMEM. */
int minato_BufferQueue_create(minato_BufferQueue_available available, minato_BufferQueue_destroy destroy,
                              void *context, struct ANativeWindow **producer, struct minato_BufferQueue **consumer);

/* The consumer: the default size of the buffers while the producer sets none (SurfaceTexture.setDefaultBufferSize);
   the queued frame, the newest, after the release of the consumer's last one (0; -EAGAIN with none queued; -ENODEV
   abandoned); the release of a frame's slot without a new one (releaseTexImage); abandon; and the consumer's end. */
void minato_BufferQueue_setDefaultSize(struct minato_BufferQueue *queue, int32_t width, int32_t height);
int minato_BufferQueue_acquire(struct minato_BufferQueue *queue, struct minato_BufferQueue_frame *frame);
void minato_BufferQueue_releaseSlot(struct minato_BufferQueue *queue, int32_t slot);
void minato_BufferQueue_abandon(struct minato_BufferQueue *queue);
int minato_BufferQueue_isAbandoned(struct minato_BufferQueue *queue);
void minato_BufferQueue_release(struct minato_BufferQueue *queue);

/* The consumer as an ImageReader's (the graphics breadth's G3a, BufferQueue's maxAcquiredBufferCount): up to
   `maxAcquired` frames stay acquired at once, each until minato_BufferQueue_releaseSlot, where a SurfaceTexture's
   acquire lets its last frame go; one more acquire answers -EBUSY. The queue has maxAcquired + 2 slots, and its
   buffers the consumer's `usage` with the producers' (GPU_FRAMEBUFFER, CPU_WRITE_OFTEN) in place of every usage.
   Before the first dequeue; 0, or -EINVAL for a maxAcquired below 1 or above 62. Frames still drop as above, unless
   an ImageWriter is connected. */
int minato_BufferQueue_setConsumer(struct minato_BufferQueue *queue, int32_t maxAcquired, uint64_t usage);
/* The format of the buffers while the producer sets none (BufferQueue's setDefaultBufferFormat), an
   AHARDWAREBUFFER_FORMAT_* or IMPLEMENTATION_DEFINED (0x22, a PRIVATE reader's: RGBA_8888 in memory, 0x22 to
   ANativeWindow_getFormat); RGBA_8888 until set. And the release of the buffers of every free slot, which the next
   dequeue of each makes again (discardFreeBuffers). */
void minato_BufferQueue_setDefaultFormat(struct minato_BufferQueue *queue, int32_t format);
void minato_BufferQueue_discardFreeBuffers(struct minato_BufferQueue *queue);
/* An ImageReader's acquired frame leaves the queue (detachBuffer, the graphics breadth's G3b): its slot is free and
   without a buffer, whose slot reference the queue releases; the consumer keeps its own. 0, or -EINVAL for a slot
   that is not acquired or a queue that is no ImageReader's. */
int minato_BufferQueue_detach(struct minato_BufferQueue *queue, int32_t slot);

/* The producer, for libEGL's window surfaces on a queue window (c2): whether `window` is one; a free slot's buffer of
   the window's size and format, waiting for one (0, -ENODEV abandoned, -EINVAL for any other window); and its queue
   with a timestamp, 0 for the queue's time (0, -ENODEV, -EINVAL for a slot that is not dequeued, as libgui's
   BAD_VALUE); or its cancel, which gives the slot back free without a frame, libgui's cancelBuffer (0, -EINVAL). */
int minato_ANativeWindow_isQueue(struct ANativeWindow *window);
int minato_ANativeWindow_dequeue(struct ANativeWindow *window, struct AHardwareBuffer **buffer, int32_t *slot);
int minato_ANativeWindow_queue(struct ANativeWindow *window, int32_t slot, int64_t timestamp);
int minato_ANativeWindow_cancel(struct ANativeWindow *window, int32_t slot);

/* An ImageWriter as the producer (the graphics breadth's G3b, libgui's connect as NATIVE_WINDOW_API_CAMERA): once
   connected, no frame drops; the queue has `maxDequeued` slots next to the consumer's maxAcquired (one for a
   SurfaceTexture), so that a dequeue waits once all are out; and `released` runs with `context` after each release
   of a frame by the consumer, outside the queue's lock, never after the disconnect has returned, which gives the
   queue its slot count back. 0, -EBUSY with a producer connected already, -ENODEV abandoned, -EINVAL (also for more
   than MINATO_BUFFER_QUEUE_MAX_SLOTS slots). A connected producer queues with its settings, and queues
   a buffer it attaches, which takes a free slot, waiting for one, and leaves it again at the consumer's release
   (0, -ENODEV, -EINVAL). */
typedef void (*minato_BufferQueue_released)(void *context);
int minato_ANativeWindow_connect(struct ANativeWindow *window, int32_t maxDequeued,
                                 minato_BufferQueue_released released, void *context);
void minato_ANativeWindow_disconnect(struct ANativeWindow *window);
int minato_ANativeWindow_queueWith(struct ANativeWindow *window, int32_t slot,
                                   const struct minato_BufferQueue_settings *settings);
int minato_ANativeWindow_attachAndQueue(struct ANativeWindow *window, struct AHardwareBuffer *buffer,
                                        const struct minato_BufferQueue_settings *settings);

#ifdef __cplusplus
}
#endif

#endif
