// libmediandk.so's AMediaCodec (ADR-0060 e2b3): the codec core, which the framework's MediaCodec natives run on too.
// Minato's codecs are video decoders on the host's (video_decoder in guest_interface.h): c2.apple.avc.decoder and
// c2.apple.hevc.decoder. Encoders, crypto and the audio codecs (e2f) are not here yet: their calls answer
// AMEDIA_ERROR_UNSUPPORTED, and an encoder or another type cannot be made (NULL), as on a device without it.
// - States as Android's: made, configured (configure), started (start: the host's decoder), and back to made at
//   stop; flush keeps it started. A call in the wrong state answers AMEDIA_ERROR_INVALID_OPERATION.
// - Input: four buffers of max-input-size bytes (the format's, else width x height x 3 / 2, at least 1 MiB); a queued
//   one is copied at once, so its buffer is free again; the units the host does not take yet wait in order and go
//   at the next call, so that queueInputBuffer never fails for a host that is behind.
// - Output: eight buffers. Each call pumps the host: queued units in, decoded frames out into free output buffers,
//   NV12 (COLOR_FormatYUV420SemiPlanar, 21) with the host's rows. INFO_OUTPUT_FORMAT_CHANGED comes before the first
//   frame and before a frame of another size; the output format has the size, stride, slice height, color format and
//   the crop ("crop", a rect, AMEDIAFORMAT_KEY_DISPLAY_CROP). The end of the stream is an output buffer of size 0 with
//   AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM.
// - A wait (dequeue with a timeout) polls the host every 2 ms, as its decoder calls back on its own threads.
// - Asynchronous mode (setAsyncNotifyCallback before configure): a thread of the codec's, from start to stop, pumps
//   and calls the app back: each free input buffer once, the format, each output buffer, outside the codec's lock.
// - Surface output (ADR-0060 e2c): configured with a window of libnativewindow's queue (a SurfaceTexture's, an
//   ImageReader's, an AImageReader's), the host keeps the frames and an output buffer holds a frame's token, with no
//   bytes (getOutputBuffer NULL, as Android's) and the frame's NV12 size as its BufferInfo size; the output format's
//   color format is COLOR_FormatSurface. releaseOutputBuffer with render dequeues a slot of the window, has the host
//   draw the frame into its buffer and queues it at the frame's time (or releaseOutputBufferAtTime's), else the frame
//   goes undrawn; setOutputSurface changes the window to another queue's. Named limit: a host window (SurfaceView's,
//   the activity's) has no slots yet: configure answers AMEDIA_ERROR_UNSUPPORTED (e2c3).
// Bionic's C library and pthreads alone, as the rest of runtime/android: no C++ library.
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/buffer_queue.h"
#include "APKRuntime/guest_interface.h"
#include "APKRuntime/media_codec.h"

namespace {

constexpr int kInputs = 4, kOutputs = 8;
constexpr int32_t kColorFormatYuv420SemiPlanar = 21, kColorFormatSurface = 0x7f000789;

// libnativewindow's private functions (APKRT_PRIVATE) for a surface's queue, looked up once (no C++ runtime, so
// pthread_once rather than a function's static).
struct WindowFunctions {
    decltype(&minato_ANativeWindow_isQueue) is_queue = nullptr;
    decltype(&minato_ANativeWindow_dequeue) dequeue = nullptr;
    decltype(&minato_ANativeWindow_queueWith) queue = nullptr;
    decltype(&minato_ANativeWindow_cancel) cancel = nullptr;
};

WindowFunctions found_window_functions;
pthread_once_t window_functions_once = PTHREAD_ONCE_INIT;

void find_window_functions() {
    if (void *library = dlopen("libnativewindow.so", RTLD_NOW)) {
        WindowFunctions &found = found_window_functions;
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
        FIND(is_queue, minato_ANativeWindow_isQueue);
        FIND(dequeue, minato_ANativeWindow_dequeue);
        FIND(queue, minato_ANativeWindow_queueWith);
        FIND(cancel, minato_ANativeWindow_cancel);
#undef FIND
    }
}

/// The functions, or NULL while any is missing.
const WindowFunctions *window_functions() {
    pthread_once(&window_functions_once, find_window_functions);
    const WindowFunctions &found = found_window_functions;
    return found.is_queue != nullptr && found.dequeue != nullptr && found.queue != nullptr && found.cancel != nullptr
               ? &found
               : nullptr;
}

/// Whether `window` is a queue's, which a surface decoder draws into.
bool is_queue_window(ANativeWindow *window) {
    const WindowFunctions *functions = window_functions();
    return functions != nullptr && functions->is_queue(window) != 0;
}

const minato_media_codec_info kCodecs[] = {
    {"c2.apple.avc.decoder", "video/avc", 0, 1},
    {"c2.apple.hevc.decoder", "video/hevc", 0, 1},
};

uint32_t decoder_type(const char *type) {
    if (type == nullptr) return 0;
    if (strcasecmp(type, "video/avc") == 0) return APKRT_VIDEO_DECODER_AVC;
    if (strcasecmp(type, "video/hevc") == 0) return APKRT_VIDEO_DECODER_HEVC;
    return 0;
}

long video_call(apkrt_video_decoder_op op, uintptr_t argument) {
    long result;
    do {
        result = syscall(APKRT_NR_VIDEO_DECODER, op, argument);
    } while (result == -1 && errno == EINTR);
    return result == -1 ? -errno : result;
}

int64_t now_us() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

/// Bytes of the codec's own, grown as needed.
struct Bytes {
    uint8_t *data;
    size_t size;
};

bool resize(Bytes *bytes, size_t size) {
    if (size <= bytes->size && bytes->data != nullptr) {
        bytes->size = size;
        return true;
    }
    auto *grown = static_cast<uint8_t *>(realloc(bytes->data, size > 0 ? size : 1));
    if (grown == nullptr) return false;
    bytes->data = grown, bytes->size = size;
    return true;
}

struct Unit {
    Bytes bytes;
    int64_t time_us;
    uint32_t flags;
};

struct Output {
    Bytes bytes;
    AMediaCodecBufferInfo info;
    uint64_t token;  // a surface decoder's frame, 0 for none
    enum { kFree, kReady, kHeld } state;
};

struct Lock {
    explicit Lock(pthread_mutex_t *mutex) : mutex(mutex) { pthread_mutex_lock(mutex); }
    ~Lock() { pthread_mutex_unlock(mutex); }
    pthread_mutex_t *mutex;
};

}  // namespace

struct AMediaCodec {
    const minato_media_codec_info *info;
    pthread_mutex_t mutex;
    enum { kMade, kConfigured, kStarted } state;
    uint32_t type;
    int32_t width, height;
    Bytes codec_data;
    AMediaFormat *input_format, *output_format;
    uint64_t decoder;  // the runtime's ID while started
    ANativeWindow *window;  // the surface, a queue's, with a reference of the codec's; NULL for ByteBuffer output
    Bytes inputs[kInputs];
    bool input_dequeued[kInputs];
    Unit waiting[kInputs];  // queued units the host has not taken yet, from waiting_head
    int waiting_head, waiting_count;
    Output outputs[kOutputs];
    int ready[kOutputs];  // output buffers in order, from ready_head
    int ready_head, ready_count;
    bool format_changed;  // to report before the next output buffer
    uint32_t frame_width, frame_height, frame_stride, frame_slice;
    bool ended;  // the end of the stream came out
    media_status_t error;
    // Asynchronous mode.
    AMediaCodecOnAsyncNotifyCallback callbacks;
    void *userdata;
    bool asynchronous, thread_running, stopping;
    pthread_t thread;
};

namespace {

/// Sends the waiting units to the host while it takes them, with the codec's lock held.
void push_units(AMediaCodec *codec) {
    while (codec->waiting_count > 0 && codec->error == AMEDIA_OK) {
        Unit &unit = codec->waiting[codec->waiting_head];
        apkrt_video_decoder call{};
        call.id = codec->decoder;
        call.data = reinterpret_cast<uintptr_t>(unit.bytes.data);
        call.size = unit.bytes.size;
        call.time_us = unit.time_us;
        call.flags = unit.flags;
        const long queued = video_call(APKRT_VIDEO_DECODER_QUEUE, reinterpret_cast<uintptr_t>(&call));
        if (queued == -EAGAIN) return;
        if (queued != 0) {
            codec->error = AMEDIA_ERROR_UNKNOWN;
            return;
        }
        codec->waiting_head = (codec->waiting_head + 1) % kInputs;
        codec->waiting_count--;
    }
}

/// The output format of a frame of this size and rows, with the codec's lock held.
void set_output_format(AMediaCodec *codec, const apkrt_video_decoder &frame) {
    if (codec->output_format == nullptr) codec->output_format = AMediaFormat_new();
    AMediaFormat *format = codec->output_format;
    if (format == nullptr) return;
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/raw");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, static_cast<int32_t>(frame.width));
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, static_cast<int32_t>(frame.height));
    if (codec->window == nullptr) {
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_STRIDE, static_cast<int32_t>(frame.stride));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, static_cast<int32_t>(frame.slice_height));
    }
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          codec->window != nullptr ? kColorFormatSurface : kColorFormatYuv420SemiPlanar);
    AMediaFormat_setRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, frame.crop_left, frame.crop_top, frame.crop_right,
                         frame.crop_bottom);
    codec->frame_width = frame.width, codec->frame_height = frame.height;
    codec->frame_stride = frame.stride, codec->frame_slice = frame.slice_height;
    codec->format_changed = true;
}

/// Takes the host's decoded frames into free output buffers, with the codec's lock held.
void pull_frames(AMediaCodec *codec) {
    while (!codec->ended && codec->error == AMEDIA_OK) {
        int free_output = -1;
        for (int index = 0; index < kOutputs && free_output < 0; index++) {
            if (codec->outputs[index].state == Output::kFree) free_output = index;
        }
        if (free_output < 0) return;
        Output &output = codec->outputs[free_output];
        apkrt_video_decoder call{};
        call.id = codec->decoder;
        if (codec->window == nullptr) {
            call.data = reinterpret_cast<uintptr_t>(output.bytes.data);
            call.size = output.bytes.data != nullptr ? output.bytes.size : 0;
        }
        long got = video_call(APKRT_VIDEO_DECODER_DEQUEUE, reinterpret_cast<uintptr_t>(&call));
        if (got == -ENOSPC && codec->window == nullptr) {
            if (!resize(&output.bytes, call.size)) {
                codec->error = AMEDIA_ERROR_UNKNOWN;
                return;
            }
            call.data = reinterpret_cast<uintptr_t>(output.bytes.data);
            call.size = output.bytes.size;
            got = video_call(APKRT_VIDEO_DECODER_DEQUEUE, reinterpret_cast<uintptr_t>(&call));
        }
        if (got == -EAGAIN) return;
        if (got != 0) {
            codec->error = AMEDIA_ERROR_UNKNOWN;
            return;
        }
        const bool end = (call.flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0;
        if (!end && (call.width != codec->frame_width || call.height != codec->frame_height ||
                     call.stride != codec->frame_stride || call.slice_height != codec->frame_slice)) {
            set_output_format(codec, call);
        }
        output.token = codec->window != nullptr && !end ? call.data : 0;
        // A surface's frame has no bytes here: its size is its NV12's, as a ByteBuffer frame's would be.
        const uint64_t size = output.token != 0 ? uint64_t{call.width} * call.height * 3 / 2 : call.size;
        output.info = AMediaCodecBufferInfo{0, static_cast<int32_t>(size), call.time_us,
                                            end ? static_cast<uint32_t>(AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) : 0u};
        output.state = Output::kReady;
        codec->ready[(codec->ready_head + codec->ready_count) % kOutputs] = free_output;
        codec->ready_count++;
        if (end) codec->ended = true;
    }
}

void pump(AMediaCodec *codec) {
    if (codec->state != AMediaCodec::kStarted) return;
    push_units(codec);
    pull_frames(codec);
    push_units(codec);
}

/// Waits up to `timeout_us` (forever below 0) for `ready`, pumping every 2 ms, with the codec's lock held.
template <typename Ready>
bool wait_for(AMediaCodec *codec, int64_t timeout_us, Ready ready) {
    const int64_t deadline = timeout_us < 0 ? INT64_MAX : now_us() + timeout_us;
    for (;;) {
        pump(codec);
        if (ready()) return true;
        if (codec->state != AMediaCodec::kStarted || codec->error != AMEDIA_OK) return false;
        const int64_t left = deadline - now_us();
        if (left <= 0) return false;
        pthread_mutex_unlock(&codec->mutex);
        usleep(static_cast<useconds_t>(left < 2000 ? left : 2000));
        pthread_mutex_lock(&codec->mutex);
    }
}

/// A free input buffer's index, or -1 while every one is out or the host is four units behind.
int free_input(AMediaCodec *codec) {
    if (codec->waiting_count >= kInputs) return -1;
    for (int index = 0; index < kInputs; index++) {
        if (!codec->input_dequeued[index]) return index;
    }
    return -1;
}

int take_ready(AMediaCodec *codec) {
    const int index = codec->ready[codec->ready_head];
    codec->ready_head = (codec->ready_head + 1) % kOutputs;
    codec->ready_count--;
    codec->outputs[index].state = Output::kHeld;
    return index;
}

/// Drops every unit and frame, with the codec's lock held.
void drop_buffers(AMediaCodec *codec) {
    codec->waiting_head = codec->waiting_count = 0;
    codec->ready_head = codec->ready_count = 0;
    for (auto &output : codec->outputs) output.state = Output::kFree, output.token = 0;  // the runtime ended the tokens
    for (bool &dequeued : codec->input_dequeued) dequeued = false;
    codec->ended = false;
}

/// Ends the asynchronous thread, with the codec's lock held, which it lets go while it waits.
void stop_thread(AMediaCodec *codec) {
    if (!codec->thread_running) return;
    codec->stopping = true;
    codec->thread_running = false;
    if (pthread_equal(codec->thread, pthread_self())) {
        pthread_detach(codec->thread);  // stopped from its own callback
        return;
    }
    pthread_mutex_unlock(&codec->mutex);
    pthread_join(codec->thread, nullptr);
    pthread_mutex_lock(&codec->mutex);
}

void end_decoder(AMediaCodec *codec) {
    if (codec->decoder != 0) video_call(APKRT_VIDEO_DECODER_DESTROY, codec->decoder);
    codec->decoder = 0;
    drop_buffers(codec);
    codec->error = AMEDIA_OK;
}

/// Ends the surface, with the codec's lock held.
void end_window(AMediaCodec *codec) {
    if (codec->window != nullptr) ANativeWindow_release(codec->window);
    codec->window = nullptr;
}

/// A surface decoder's frame of `token` drawn into a slot of the window and queued at `timestamp_ns`, or with
/// `render` false dropped, with the codec's lock held. A slot the host cannot draw into goes back free, and the
/// frame with it.
void release_frame(AMediaCodec *codec, uint64_t token, bool render, int64_t timestamp_ns) {
    apkrt_video_decoder call{};
    call.id = codec->decoder;
    call.data = token;
    const WindowFunctions *functions = window_functions();
    AHardwareBuffer *buffer = nullptr;
    int32_t slot = -1;
    uint64_t buffer_id = 0;
    if (!render || functions == nullptr || functions->dequeue(codec->window, &buffer, &slot) != 0) {
        video_call(APKRT_VIDEO_DECODER_DISCARD, reinterpret_cast<uintptr_t>(&call));
        return;
    }
    if (AHardwareBuffer_getId(buffer, &buffer_id) != 0) buffer_id = 0;
    call.size = buffer_id;
    if (buffer_id != 0 && video_call(APKRT_VIDEO_DECODER_RENDER, reinterpret_cast<uintptr_t>(&call)) == 0) {
        minato_BufferQueue_settings settings{};
        settings.timestamp = timestamp_ns;
        settings.exact_time = 1;  // a frame at time 0 keeps it, as Android's codec queues its time
        functions->queue(codec->window, slot, &settings);
    } else {
        if (buffer_id == 0) video_call(APKRT_VIDEO_DECODER_DISCARD, reinterpret_cast<uintptr_t>(&call));
        functions->cancel(codec->window, slot);
    }
}

/// releaseOutputBuffer and releaseOutputBufferAtTime: the buffer free again, its frame drawn first when `render`, at
/// `timestamp_ns` (below 0: the frame's time).
media_status_t release_output(AMediaCodec *codec, size_t index, bool render, int64_t timestamp_ns) {
    if (codec == nullptr || index >= kOutputs) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    Output &output = codec->outputs[index];
    if (codec->state != AMediaCodec::kStarted || output.state != Output::kHeld) return AMEDIA_ERROR_INVALID_OPERATION;
    if (output.token != 0) {
        release_frame(codec, output.token, render,
                      timestamp_ns >= 0 ? timestamp_ns : output.info.presentationTimeUs * 1000);
        output.token = 0;
    }
    output.state = Output::kFree;
    pump(codec);
    return AMEDIA_OK;
}

/// Asynchronous mode's thread: pumps and calls the app back, outside the lock, until stop.
void *run_callbacks(void *argument) {
    auto *codec = static_cast<AMediaCodec *>(argument);
    pthread_mutex_lock(&codec->mutex);
    while (!codec->stopping) {
        pump(codec);
        bool called = false;
        if (codec->error != AMEDIA_OK) {
            const media_status_t error = codec->error;
            codec->error = AMEDIA_OK;
            pthread_mutex_unlock(&codec->mutex);
            if (codec->callbacks.onAsyncError != nullptr) {
                codec->callbacks.onAsyncError(codec, codec->userdata, error, 0, "decoding failed");
            }
            pthread_mutex_lock(&codec->mutex);
            continue;
        }
        const int input = free_input(codec);
        if (input >= 0) {
            codec->input_dequeued[input] = true;
            pthread_mutex_unlock(&codec->mutex);
            if (codec->callbacks.onAsyncInputAvailable != nullptr) {
                codec->callbacks.onAsyncInputAvailable(codec, codec->userdata, input);
            }
            pthread_mutex_lock(&codec->mutex);
            called = true;
        }
        if (codec->ready_count > 0 && !codec->stopping) {
            if (codec->format_changed && codec->output_format != nullptr) {
                codec->format_changed = false;
                AMediaFormat *format = AMediaFormat_new();
                if (format != nullptr) AMediaFormat_copy(format, codec->output_format);
                pthread_mutex_unlock(&codec->mutex);
                if (format != nullptr && codec->callbacks.onAsyncFormatChanged != nullptr) {
                    codec->callbacks.onAsyncFormatChanged(codec, codec->userdata, format);
                }
                AMediaFormat_delete(format);
                pthread_mutex_lock(&codec->mutex);
            }
            if (codec->ready_count > 0 && !codec->stopping) {
                const int index = take_ready(codec);
                AMediaCodecBufferInfo info = codec->outputs[index].info;
                pthread_mutex_unlock(&codec->mutex);
                if (codec->callbacks.onAsyncOutputAvailable != nullptr) {
                    codec->callbacks.onAsyncOutputAvailable(codec, codec->userdata, index, &info);
                }
                pthread_mutex_lock(&codec->mutex);
            }
            called = true;
        }
        if (!called) {
            pthread_mutex_unlock(&codec->mutex);
            usleep(2000);
            pthread_mutex_lock(&codec->mutex);
        }
    }
    pthread_mutex_unlock(&codec->mutex);
    return nullptr;
}

AMediaCodec *make_codec(const minato_media_codec_info *info) {
    if (info == nullptr || info->encoder) return nullptr;
    auto *codec = static_cast<AMediaCodec *>(calloc(1, sizeof(AMediaCodec)));
    if (codec == nullptr) return nullptr;
    pthread_mutex_init(&codec->mutex, nullptr);
    codec->info = info;
    codec->type = decoder_type(info->type);
    codec->state = AMediaCodec::kMade;
    codec->error = AMEDIA_OK;
    return codec;
}

}  // namespace

extern "C" {

size_t minato_media_codec_count(void) {
    return sizeof kCodecs / sizeof *kCodecs;
}

const minato_media_codec_info *minato_media_codec_at(size_t index) {
    return index < minato_media_codec_count() ? &kCodecs[index] : nullptr;
}

AMediaCodec *AMediaCodec_createCodecByName(const char *name) {
    if (name == nullptr) return nullptr;
    for (const auto &info : kCodecs) {
        if (strcmp(info.name, name) == 0) return make_codec(&info);
    }
    return nullptr;
}

AMediaCodec *AMediaCodec_createDecoderByType(const char *mime_type) {
    if (mime_type == nullptr) return nullptr;
    for (const auto &info : kCodecs) {
        if (!info.encoder && strcasecmp(info.type, mime_type) == 0) return make_codec(&info);
    }
    return nullptr;
}

AMediaCodec *AMediaCodec_createEncoderByType(const char *) {
    return nullptr;  // no encoder yet
}

media_status_t AMediaCodec_delete(AMediaCodec *codec) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
    pthread_mutex_lock(&codec->mutex);
    stop_thread(codec);
    end_decoder(codec);
    end_window(codec);
    pthread_mutex_unlock(&codec->mutex);
    AMediaFormat_delete(codec->input_format);
    AMediaFormat_delete(codec->output_format);
    free(codec->codec_data.data);
    for (auto &input : codec->inputs) free(input.data);
    for (auto &unit : codec->waiting) free(unit.bytes.data);
    for (auto &output : codec->outputs) free(output.bytes.data);
    pthread_mutex_destroy(&codec->mutex);
    free(codec);
    return AMEDIA_OK;
}

media_status_t AMediaCodec_getName(AMediaCodec *codec, char **out_name) {
    if (codec == nullptr || out_name == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out_name = strdup(codec->info->name);
    return *out_name != nullptr ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}

void AMediaCodec_releaseName(AMediaCodec *, char *name) {
    free(name);
}

media_status_t AMediaCodec_configure(AMediaCodec *codec, const AMediaFormat *format, ANativeWindow *surface,
                                     AMediaCrypto *crypto, uint32_t flags) {
    if (codec == nullptr || format == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state != AMediaCodec::kMade) return AMEDIA_ERROR_INVALID_OPERATION;
    if ((surface != nullptr && !is_queue_window(surface)) || crypto != nullptr ||
        (flags & AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != 0) {
        return AMEDIA_ERROR_UNSUPPORTED;  // a host window's surface with e2c3; no crypto, no encoder
    }
    auto *asked = const_cast<AMediaFormat *>(format);
    const char *mime = nullptr;
    int32_t width = 0, height = 0, max_input = 0;
    if (!AMediaFormat_getString(asked, AMEDIAFORMAT_KEY_MIME, &mime) || decoder_type(mime) != codec->type ||
        !AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_WIDTH, &width) ||
        !AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_HEIGHT, &height) || width <= 0 || height <= 0 ||
        width > 8192 || height > 8192) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    codec->codec_data.size = 0;
    const char *const keys[] = {AMEDIAFORMAT_KEY_CSD_0, AMEDIAFORMAT_KEY_CSD_1, AMEDIAFORMAT_KEY_CSD_2};
    for (const char *key : keys) {
        void *data = nullptr;
        size_t size = 0;
        if (!AMediaFormat_getBuffer(asked, key, &data, &size) || size == 0) continue;
        const size_t used = codec->codec_data.data != nullptr ? codec->codec_data.size : 0;
        if (used + size > (64u << 10) || !resize(&codec->codec_data, used + size)) {
            return AMEDIA_ERROR_INVALID_PARAMETER;
        }
        memcpy(codec->codec_data.data + used, data, size);
    }
    const size_t frame = static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
    size_t capacity = AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, &max_input) && max_input > 0
                          ? static_cast<size_t>(max_input)
                          : (frame > (1u << 20) ? frame : (1u << 20));
    if (capacity > (16u << 20)) capacity = 16u << 20;
    for (auto &input : codec->inputs) {
        if (!resize(&input, capacity)) return AMEDIA_ERROR_UNKNOWN;
    }
    if (codec->input_format == nullptr) codec->input_format = AMediaFormat_new();
    if (codec->input_format == nullptr) return AMEDIA_ERROR_UNKNOWN;
    AMediaFormat_copy(codec->input_format, asked);
    codec->width = width, codec->height = height;
    end_window(codec);
    if (surface != nullptr) {
        ANativeWindow_acquire(surface);
        codec->window = surface;
    }
    codec->state = AMediaCodec::kConfigured;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_setAsyncNotifyCallback(AMediaCodec *codec, AMediaCodecOnAsyncNotifyCallback callback,
                                                  void *userdata) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state == AMediaCodec::kStarted) return AMEDIA_ERROR_INVALID_OPERATION;
    codec->callbacks = callback;
    codec->userdata = userdata;
    codec->asynchronous = callback.onAsyncInputAvailable != nullptr || callback.onAsyncOutputAvailable != nullptr;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_start(AMediaCodec *codec) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state != AMediaCodec::kConfigured) return AMEDIA_ERROR_INVALID_OPERATION;
    apkrt_video_decoder call{};
    call.type = codec->type;
    call.width = static_cast<uint32_t>(codec->width);
    call.height = static_cast<uint32_t>(codec->height);
    call.data = reinterpret_cast<uintptr_t>(codec->codec_data.data);
    call.size = codec->codec_data.data != nullptr ? codec->codec_data.size : 0;
    call.flags = codec->window != nullptr ? APKRT_VIDEO_DECODER_SURFACE : 0;
    if (video_call(APKRT_VIDEO_DECODER_CREATE, reinterpret_cast<uintptr_t>(&call)) != 0) return AMEDIA_ERROR_UNKNOWN;
    codec->decoder = call.id;
    codec->frame_width = codec->frame_height = codec->frame_stride = codec->frame_slice = 0;
    codec->format_changed = false;
    codec->state = AMediaCodec::kStarted;
    if (codec->asynchronous) {
        codec->stopping = false;
        codec->thread_running = pthread_create(&codec->thread, nullptr, run_callbacks, codec) == 0;
    }
    return AMEDIA_OK;
}

media_status_t AMediaCodec_stop(AMediaCodec *codec) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    stop_thread(codec);
    end_decoder(codec);
    end_window(codec);  // a surface is configure's, as Android's
    codec->state = AMediaCodec::kMade;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_flush(AMediaCodec *codec) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state != AMediaCodec::kStarted) return AMEDIA_ERROR_INVALID_OPERATION;
    if (video_call(APKRT_VIDEO_DECODER_FLUSH, codec->decoder) != 0) return AMEDIA_ERROR_UNKNOWN;
    drop_buffers(codec);
    return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *codec, int64_t timeout_us) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state != AMediaCodec::kStarted || codec->asynchronous) return AMEDIA_ERROR_INVALID_OPERATION;
    int index = -1;
    wait_for(codec, timeout_us, [&] { return (index = free_input(codec)) >= 0; });
    if (index < 0) {
        return codec->error != AMEDIA_OK ? static_cast<ssize_t>(codec->error) : AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    codec->input_dequeued[index] = true;
    return index;
}

uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *codec, size_t index, size_t *out_size) {
    if (codec == nullptr || index >= kInputs) return nullptr;
    Lock hold(&codec->mutex);
    if (codec->state != AMediaCodec::kStarted || !codec->input_dequeued[index]) return nullptr;
    if (out_size != nullptr) *out_size = codec->inputs[index].size;
    return codec->inputs[index].data;
}

media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *codec, size_t index, off_t offset, size_t size,
                                            uint64_t time, uint32_t flags) {
    if (codec == nullptr || index >= kInputs) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state != AMediaCodec::kStarted || !codec->input_dequeued[index]) return AMEDIA_ERROR_INVALID_OPERATION;
    const Bytes &input = codec->inputs[index];
    if (offset < 0 || static_cast<size_t>(offset) > input.size || size > input.size - static_cast<size_t>(offset)) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (codec->waiting_count >= kInputs) return AMEDIA_ERROR_INVALID_OPERATION;  // free_input keeps this from coming
    Unit &unit = codec->waiting[(codec->waiting_head + codec->waiting_count) % kInputs];
    if (!resize(&unit.bytes, size)) return AMEDIA_ERROR_UNKNOWN;
    if (size > 0) memcpy(unit.bytes.data, input.data + offset, size);
    unit.time_us = static_cast<int64_t>(time);
    unit.flags = (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0 ? APKRT_VIDEO_DECODER_END_OF_STREAM : 0;
    codec->waiting_count++;
    codec->input_dequeued[index] = false;
    pump(codec);
    return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *codec, AMediaCodecBufferInfo *info, int64_t timeout_us) {
    if (codec == nullptr || info == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state != AMediaCodec::kStarted || codec->asynchronous) return AMEDIA_ERROR_INVALID_OPERATION;
    wait_for(codec, timeout_us, [&] { return codec->ready_count > 0; });
    if (codec->ready_count == 0) {
        return codec->error != AMEDIA_OK ? static_cast<ssize_t>(codec->error) : AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    if (codec->format_changed) {
        codec->format_changed = false;
        return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
    }
    const int index = take_ready(codec);
    *info = codec->outputs[index].info;
    return index;
}

uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *codec, size_t index, size_t *out_size) {
    if (codec == nullptr || index >= kOutputs) return nullptr;
    Lock hold(&codec->mutex);
    Output &output = codec->outputs[index];
    if (codec->state != AMediaCodec::kStarted || output.state != Output::kHeld || codec->window != nullptr) {
        return nullptr;  // a surface decoder's buffers have no bytes
    }
    if (out_size != nullptr) *out_size = output.bytes.size;
    return output.bytes.data;
}

media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *codec, size_t index, bool render) {
    return release_output(codec, index, render, -1);
}

media_status_t AMediaCodec_releaseOutputBufferAtTime(AMediaCodec *codec, size_t index, int64_t timestamp_ns) {
    return release_output(codec, index, true, timestamp_ns >= 0 ? timestamp_ns : 0);
}

AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *codec) {
    if (codec == nullptr) return nullptr;
    Lock hold(&codec->mutex);
    AMediaFormat *copy = AMediaFormat_new();
    if (copy != nullptr && codec->output_format != nullptr) AMediaFormat_copy(copy, codec->output_format);
    return copy;
}

AMediaFormat *AMediaCodec_getInputFormat(AMediaCodec *codec) {
    if (codec == nullptr) return nullptr;
    Lock hold(&codec->mutex);
    AMediaFormat *copy = AMediaFormat_new();
    if (copy != nullptr && codec->input_format != nullptr) AMediaFormat_copy(copy, codec->input_format);
    return copy;
}

AMediaFormat *AMediaCodec_getBufferFormat(AMediaCodec *codec, size_t) {
    return AMediaCodec_getOutputFormat(codec);
}

media_status_t AMediaCodec_signalEndOfInputStream(AMediaCodec *) {
    return AMEDIA_ERROR_UNSUPPORTED;  // an input surface's, for encoders
}

media_status_t AMediaCodec_setOutputSurface(AMediaCodec *codec, ANativeWindow *surface) {
    if (codec == nullptr || surface == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->window == nullptr || codec->state == AMediaCodec::kMade) return AMEDIA_ERROR_INVALID_OPERATION;
    if (!is_queue_window(surface)) return AMEDIA_ERROR_UNSUPPORTED;  // a host window's with e2c3
    ANativeWindow_acquire(surface);
    end_window(codec);
    codec->window = surface;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_setParameters(AMediaCodec *codec, const AMediaFormat *) {
    return codec != nullptr ? AMEDIA_OK : AMEDIA_ERROR_INVALID_PARAMETER;  // none a decoder of ours takes
}

}  // extern "C"
