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

/* A frame as the consumer acquires it: its slot, its buffer (the slot's, which the consumer does not release), the
   producer's timestamp (CLOCK_MONOTONIC in nanoseconds, the queue's time unless the producer gave one) and its
   transform (ANATIVEWINDOW_TRANSFORM_*). */
struct minato_BufferQueue_frame {
    int32_t slot;
    struct AHardwareBuffer *buffer;
    int64_t timestamp;
    int32_t transform;
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
   Before the first dequeue; 0, or -EINVAL for a maxAcquired below 1 or above 62. Frames still drop as above. */
int minato_BufferQueue_setConsumer(struct minato_BufferQueue *queue, int32_t maxAcquired, uint64_t usage);
/* The format of the buffers while the producer sets none (BufferQueue's setDefaultBufferFormat), an
   AHARDWAREBUFFER_FORMAT_*; RGBA_8888 until set. And the release of the buffers of every free slot, which the next
   dequeue of each makes again (discardFreeBuffers). */
void minato_BufferQueue_setDefaultFormat(struct minato_BufferQueue *queue, int32_t format);
void minato_BufferQueue_discardFreeBuffers(struct minato_BufferQueue *queue);

/* The producer, for libEGL's window surfaces on a queue window (c2): whether `window` is one; a free slot's buffer of
   the window's size and format, waiting for one (0, -ENODEV abandoned, -EINVAL for any other window); and its queue
   with a timestamp, 0 for the queue's time (0, -ENODEV, -EINVAL for a slot that is not dequeued, as libgui's
   BAD_VALUE); or its cancel, which gives the slot back free without a frame, libgui's cancelBuffer (0, -EINVAL). */
int minato_ANativeWindow_isQueue(struct ANativeWindow *window);
int minato_ANativeWindow_dequeue(struct ANativeWindow *window, struct AHardwareBuffer **buffer, int32_t *slot);
int minato_ANativeWindow_queue(struct ANativeWindow *window, int32_t slot, int64_t timestamp);
int minato_ANativeWindow_cancel(struct ANativeWindow *window, int32_t slot);

#ifdef __cplusplus
}
#endif

#endif
