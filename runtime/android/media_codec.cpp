// libmediandk.so's AMediaCodec (ADR-0060 e2b3): the codec core, which the framework's MediaCodec natives run on too.
// Minato's codecs are video decoders on the host's (video_decoder in guest_interface.h): c2.apple.avc.decoder and
// c2.apple.hevc.decoder; and the audio decoders (e2f) under Android's names with their OMX.google aliases, as Android
// 15 lists them: c2.android.aac.decoder, c2.android.mp3.decoder and c2.android.flac.decoder on the host's
// (audio_codec), and c2.android.raw.decoder in the guest. Encoders and crypto are not here yet: their calls answer
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
//   goes undrawn; setOutputSurface changes the window to another queue's. The codec claims the window as its producer
//   from configure to its end (minato_ANativeWindow_claim, as Android's codec connects to its surface): a CPU lock, a
//   writer, an EGL window surface or another codec on it is refused after the claim, so that no other producer holds
//   the slots its render dequeues under the codec's lock.
// - A host window (a SurfaceView's, the activity's; ADR-0060 e2c3) has no slots: the host draws a rendered frame into
//   the window's layer itself (RENDER_WINDOW), presented at once or at releaseOutputBufferAtTime's time, stretched or
//   cropped by setVideoScalingMode's mode. The codec claims it as a queue window, so that a CPU lock, an EGL window
//   surface, a Vulkan surface and another codec are refused on it after the claim, and an EGL window surface or a
//   Vulkan surface made before refuses the claim (AMEDIA_ERROR_UNKNOWN, as Android's connect fails there [verified:
//   media-window-test]). The host does not hold one producer per window: the claim is the guest's. Once the window
//   is gone (the SurfaceView's surface destroyed), the codec releases itself, as Android 15's does [verified:
//   media-window-test]: every call
//   that sets or runs it answers AMEDIA_ERROR_INVALID_OPERATION, its thread joined at the delete.
// - Frames rendered (AMediaCodec_setOnFrameRenderedCallback, MediaCodec's OnFrameRenderedListener): each frame drawn
//   into any surface, reported after its render with its time and the buffer's timestamp, the frame's time in
//   nanoseconds or releaseOutputBufferAtTime's, as Android 15 reports them [verified: media-window-test]; on the
//   thread that released it, outside the codec's lock.
// - Audio (e2f): the AAC decoder decodes each queued unit as an output buffer is free, one unit per buffer at the
//   unit's time, 16-bit PCM interleaved; csd-0's AudioSpecificConfig, or a unit queued with
//   BUFFER_FLAG_CODEC_CONFIG, configures it. Android trims what comes out as CCodec's SkipCutBuffer does [verified:
//   media-codec-audio-test]: the format's encoder-delay frames are skipped from the start, and again after each
//   flush, and encoder-padding frames are held back from buffer to buffer, so that the end of the stream drops them.
//   The output format is C2SoftAacDec's: audio/raw with its rate, channels and channel mask, encoder-delay and
//   encoder-padding as configured, and its fixed DRC and batch keys; configure's has the format's rate and channels
//   with channel mask 0. A format change rides the buffer it comes with: INFO_OUTPUT_FORMAT_CHANGED comes when that
//   buffer would. A flush before the app has seen a decoded format brings configure's again, alone (no buffer with
//   it), and the decoded one after it, as Android 15's resume reports them [verified: media-codec-audio-test and
//   media-codec-decoders-test]. A buffer whose frames the trimming took is given out empty, as Android's are.
// - MP3, FLAC and raw (e2f.2), as Android 15's C2SoftMp3Dec, C2SoftFlacDec and C2SoftRawDec [verified:
//   media-codec-decoders-test]. Their output formats have no channel mask nor AAC's keys; FLAC's and raw's have the PCM
//   encoding, 16-bit or float as configured (raw's also 8-bit, 24-bit packed and 32-bit). The trimming above is theirs
//   too. MP3: a buffer's frames, split by their headers, decode into one output buffer, at the time of the first unit
//   since the start or a flush plus the frames given since; the host's codec is made from the first frame's header, and
//   again from a header of another rate or channels, whose delay the new codec cuts where C2SoftMp3Dec goes on, so that
//   it comes as silence (a named limit: the new codec starts without the bit reservoir); a frame whose side information
//   pvmp3 refuses never goes to the host, and it and a frame the host decodes to nothing give silence, as
//   C2SoftMp3Dec's recoverable errors do; the end of the stream adds the 529 frames of silence that C2SoftMp3Dec adds
//   for the delay it cut from the start. FLAC: csd-0's metadata, or the first unit's, gives STREAMINFO to the host, one
//   frame a unit (after a frame the host cannot decode, none until a flush, as libFLAC under C2SoftFlacDec), its
//   samples shifted to 16 bits or made float as FLACDecoder does. Raw: each unit's bytes come out as they went in, in
//   whole frames. For every audio codec, a unit queued with the end of the stream comes out as its frames and then the
//   end on its own, as CCodec's buffer channel splits it; and a format of another rate or channels than the trimming's
//   (configure's, then the last such change's) trims anew, as CCodecBuffers' updateSkipCutBuffer: the frames held back
//   are dropped, and configure's delay is skipped and its padding held again, each times the new rate over the
//   trimming's, as it scales them.
// Bionic's C library and pthreads alone, as the rest of runtime/android: no C++ library.
#include <android/hardware_buffer.h>
#include <android/log.h>
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
    decltype(&minato_ANativeWindow_claim) claim = nullptr;
    decltype(&minato_ANativeWindow_unclaim) unclaim = nullptr;
    decltype(&minato_ANativeWindow_getInfo) info = nullptr;
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
        FIND(claim, minato_ANativeWindow_claim);
        FIND(unclaim, minato_ANativeWindow_unclaim);
        FIND(info, minato_ANativeWindow_getInfo);
#undef FIND
    }
}

/// The functions, or NULL while any is missing.
const WindowFunctions *window_functions() {
    pthread_once(&window_functions_once, find_window_functions);
    const WindowFunctions &found = found_window_functions;
    return found.is_queue != nullptr && found.dequeue != nullptr && found.queue != nullptr && found.cancel != nullptr &&
                   found.claim != nullptr && found.unclaim != nullptr && found.info != nullptr
               ? &found
               : nullptr;
}

/// Whether a surface decoder draws into `window`: a queue's, with `host` 0, or a host window, with `host` its ID.
bool is_surface(ANativeWindow *window, uint64_t *host) {
    const WindowFunctions *functions = window_functions();
    if (functions == nullptr) return false;
    if (functions->is_queue(window) != 0) {
        *host = 0;
        return true;
    }
    minato_window_info info{};
    if (functions->info(window, &info) != 0 || info.id == 0) return false;
    *host = info.id;
    return true;
}

constexpr char kAacType[] = "audio/mp4a-latm", kAacDecoder[] = "c2.android.aac.decoder";
constexpr char kMp3Type[] = "audio/mpeg", kMp3Decoder[] = "c2.android.mp3.decoder";
constexpr char kFlacType[] = "audio/flac", kFlacDecoder[] = "c2.android.flac.decoder";
constexpr char kRawType[] = "audio/raw", kRawDecoder[] = "c2.android.raw.decoder";

const minato_media_codec_info kCodecs[] = {
    {"c2.apple.avc.decoder", "video/avc", 0, 1, "c2.apple.avc.decoder", 1},
    {"c2.apple.hevc.decoder", "video/hevc", 0, 1, "c2.apple.hevc.decoder", 1},
    {kAacDecoder, kAacType, 0, 0, kAacDecoder, 0},
    {"OMX.google.aac.decoder", kAacType, 0, 0, kAacDecoder, 0},
    {kMp3Decoder, kMp3Type, 0, 0, kMp3Decoder, 0},
    {"OMX.google.mp3.decoder", kMp3Type, 0, 0, kMp3Decoder, 0},
    {kFlacDecoder, kFlacType, 0, 0, kFlacDecoder, 0},
    {"OMX.google.flac.decoder", kFlacType, 0, 0, kFlacDecoder, 0},
    {kRawDecoder, kRawType, 0, 0, kRawDecoder, 0},
    {"OMX.google.raw.decoder", kRawType, 0, 0, kRawDecoder, 0},
};

/// Raw's type of ours, beside the host's audio codec types: its samples are copied in the guest.
constexpr uint32_t kRawCodec = 0x100;
/// android.media.AudioFormat's PCM encodings.
constexpr int32_t kPcm16 = 2, kPcm8 = 3, kPcmFloat = 4, kPcm24Packed = 21, kPcm32 = 22;
/// The frames of C2SoftMp3Dec's decoder delay, which it cuts from the start and adds as silence at the end.
constexpr uint32_t kMp3Delay = 529;

/// A unit's flag of ours: BUFFER_FLAG_CODEC_CONFIG's configuration, not a unit to decode.
constexpr uint32_t kUnitConfig = 1u << 31;

uint32_t decoder_type(const char *type) {
    if (type == nullptr) return 0;
    if (strcasecmp(type, "video/avc") == 0) return APKRT_VIDEO_DECODER_AVC;
    if (strcasecmp(type, "video/hevc") == 0) return APKRT_VIDEO_DECODER_HEVC;
    return 0;
}

/// The audio codec type of a media type (the host's, or kRawCodec), 0 for none.
uint32_t audio_type(const char *type) {
    if (type == nullptr) return 0;
    if (strcasecmp(type, kAacType) == 0) return APKRT_AUDIO_CODEC_AAC;
    if (strcasecmp(type, kMp3Type) == 0) return APKRT_AUDIO_CODEC_MP3;
    if (strcasecmp(type, kFlacType) == 0) return APKRT_AUDIO_CODEC_FLAC;
    return strcasecmp(type, kRawType) == 0 ? kRawCodec : 0;
}

/// A sample's bytes in a PCM encoding.
size_t sample_bytes(int32_t encoding) {
    return encoding == kPcm8 ? 1 : encoding == kPcm24Packed ? 3 : encoding == kPcmFloat || encoding == kPcm32 ? 4 : 2;
}

/// An MP3 frame's size, rate and channels from its header, as C2SoftMp3Dec's parseMp3Header reads them, Layer III
/// only; 0 for another header.
size_t mp3_frame(const uint8_t *header, int32_t *rate, int32_t *channels) {
    if (header[0] != 0xff || (header[1] & 0xe0) != 0xe0) return 0;
    const unsigned version = header[1] >> 3 & 3u, layer = header[1] >> 1 & 3u;
    const unsigned bitrate = header[2] >> 4, rate_index = header[2] >> 2 & 3u, padding = header[2] >> 1 & 1u;
    if (version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 || rate_index == 3) return 0;
    static constexpr unsigned rates[] = {44100, 48000, 32000};
    static constexpr unsigned v1[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static constexpr unsigned v2[] = {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const unsigned hz = rates[rate_index] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    *rate = static_cast<int32_t>(hz);
    *channels = (header[3] >> 6) == 3 ? 1 : 2;
    return (version == 3 ? 144000 * v1[bitrate - 1] : 72000 * v2[bitrate - 1]) / hz + padding;
}

/// Whether pvmp3_get_side_info refuses an MP3 frame of `size` bytes (SIDE_INFO_ERROR): a granule's channel whose
/// window switching has block type 0, or side information past the frame. C2SoftMp3Dec gives the frame as silence.
bool mp3_side_info_refused(const uint8_t *frame, size_t size, int32_t channels) {
    const bool mpeg1 = (frame[1] >> 3 & 3) == 3;
    size_t bit = (frame[1] & 1) == 0 ? 48 : 32;  // the header, and its CRC
    bit += mpeg1 ? 9 + (channels == 1 ? 5 : 3) + 4 * size_t(channels) : 8 + (channels == 1 ? 1 : 2);
    const int granules = mpeg1 ? 2 : 1;
    for (int granule = 0; granule < granules; granule++) {
        for (int channel = 0; channel < channels; channel++) {
            bit += 12 + 9 + 8 + (mpeg1 ? 4 : 9);
            if (bit + 1 + 22 + (mpeg1 ? 3 : 2) > size * 8) return true;
            const bool switching = (frame[bit >> 3] >> (7 - (bit & 7)) & 1) != 0;
            const unsigned block_type = (frame[(bit + 1) >> 3] >> (7 - ((bit + 1) & 7)) & 1) << 1 |
                                        (frame[(bit + 2) >> 3] >> (7 - ((bit + 2) & 7)) & 1);
            if (switching && block_type == 0) return true;
            bit += 1 + 22 + (mpeg1 ? 3 : 2);
        }
    }
    return false;
}

/// STREAMINFO's 34 bytes in FLAC metadata ("fLaC" and its blocks, csd-0's or a config unit's); NULL where there is
/// none.
const uint8_t *flac_info(const uint8_t *data, size_t size) {
    if (data == nullptr || size < 8 || memcmp(data, "fLaC", 4) != 0) return nullptr;
    for (size_t at = 4; at + 4 <= size;) {
        const size_t length = size_t{data[at + 1]} << 16 | size_t{data[at + 2]} << 8 | data[at + 3];
        if ((data[at] & 0x7f) == 0) return length == 34 && at + 4 + length <= size ? data + at + 4 : nullptr;
        if ((data[at] & 0x80) != 0) return nullptr;
        at += 4 + length;
    }
    return nullptr;
}

long host_call(long number, uint64_t op, uintptr_t argument) {
    long result;
    do {
        result = syscall(number, op, argument);
    } while (result == -1 && errno == EINTR);
    return result == -1 ? -errno : result;
}

long video_call(apkrt_video_decoder_op op, uintptr_t argument) {
    return host_call(APKRT_NR_VIDEO_DECODER, op, argument);
}

long audio_call(uint64_t op, uintptr_t argument) {
    return host_call(APKRT_NR_AUDIO_CODEC, op, argument);
}

/// C2SoftAacDec's channel mask of a channel count, as android.media.AudioFormat's CHANNEL_OUT_* bits.
int32_t channel_mask(uint32_t channels) {
    constexpr int32_t fl = 0x4, fr = 0x8, fc = 0x10, lfe = 0x20, bl = 0x40, br = 0x80, sl = 0x800, sr = 0x1000;
    switch (channels) {
        case 1: return fl;
        case 2: return fl | fr;
        case 4: return fl | fc | bl | br;
        case 5: return fl | fc | fr | bl | br;
        case 6: return fl | fc | fr | bl | br | lfe;
        case 7: return fl | fc | fr | bl | br | sl | sr;
        case 8: return fl | fc | fr | bl | br | sl | sr | lfe;
        default: return 0;
    }
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
    AMediaFormat *format;  // an audio decoder's format change that comes with the buffer, NULL for none
    bool decoded;          // the format is the decoder's, not configure's again after a flush
    bool format_only;      // no buffer for the app: the format change alone (configure's again after a flush)
};

/// An audio format's rate, channels and channel mask, which decide whether it changed.
struct AudioShape {
    int32_t rate, channels, mask;
    bool operator==(const AudioShape &other) const {
        return rate == other.rate && channels == other.channels && mask == other.mask;
    }
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
    enum { kMade, kConfigured, kStarted, kReleased } state;  // kReleased: by itself, its window gone (e2c3)
    uint32_t type;  // the host's video decoder type, or for an audio codec its audio codec type
    bool audio;
    int32_t width, height;
    Bytes codec_data;
    AMediaFormat *input_format, *output_format;
    uint64_t decoder;  // the runtime's ID while started (an audio codec's from its configuration on)
    ANativeWindow *window;  // the surface, with a reference of the codec's; NULL for ByteBuffer output
    uint64_t host_window;   // the window's ID when it is a host window (e2c3), else 0
    uint32_t scaling;       // RENDER_WINDOW's flags, by setVideoScalingMode
    AMediaCodecOnFrameRendered rendered;  // AMediaCodec_setOnFrameRenderedCallback's, and its userdata
    void *rendered_userdata;
    bool told_format;       // a buffer the host cannot draw into logged, once per codec
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
    // An audio codec's.
    AudioShape configured, decoded, attached;  // configure's format; the host's; the last one a buffer came with
    bool told_decoded;                // the app was given a decoder's format since configure
    bool input_ended;                 // the end of the stream queued: no more input until a flush
    int32_t delay, padding;           // encoder-delay and encoder-padding frames, -1 for none in the format
    int32_t encoding;                 // the output's PCM encoding
    uint32_t channels, unit_frames;   // the host's, from its configuration (raw's: configure's channels)
    uint32_t skip;                    // frames still to skip
    uint32_t cut;                     // the frames held back: the padding's, scaled after a change of rate
    int32_t cut_rate, cut_channels;   // the trimming's rate and channels: configure's, then a format change's
    Bytes pcm, held;                  // one buffer's frames; the frames held back, the padding's
    bool first, gapless;              // MP3: no frames since the start or a flush; frames since the end's silence
    bool stuck;                       // FLAC: a frame the host could not decode since the start or a flush
    bool announce;                    // FLAC: its configuration's format due with the next buffer, frames or not
    int64_t anchor_us;                // MP3: the time of the first unit since the start or a flush
    uint64_t processed;               // MP3: the frames given since then
    // Asynchronous mode.
    AMediaCodecOnAsyncNotifyCallback callbacks;
    void *userdata;
    bool asynchronous, thread_running, stopping;
    bool thread_joinable;  // a thread made and neither joined nor detached yet
    unsigned generation;   // start's count, which a thread stopped and started again from its callback is not of
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

/// An audio codec's output format of `shape`, its decoder's keys: C2SoftAacDec's, or the others' fewer.
AMediaFormat *audio_format(const AMediaCodec *codec, const AudioShape &shape) {
    AMediaFormat *format = AMediaFormat_new();
    if (format == nullptr) return nullptr;
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/raw");
    if (shape.rate > 0) AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, shape.rate);
    if (shape.channels > 0) AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, shape.channels);
    const bool aac = codec->type == APKRT_AUDIO_CODEC_AAC;
    if (aac) AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_MASK, shape.mask);
    if (codec->delay >= 0) AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_ENCODER_DELAY, codec->delay);
    if (codec->padding >= 0) AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_ENCODER_PADDING, codec->padding);
    if (codec->type == APKRT_AUDIO_CODEC_FLAC || codec->type == kRawCodec) {
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PCM_ENCODING, codec->encoding);
    }
    AMediaFormat_setInt32(format, "buffer-batch-max-output-size", 0);
    AMediaFormat_setInt32(format, "buffer-batch-threshold-output-size", 0);
    if (!aac) return format;
    const struct {
        const char *key;
        int32_t value;
    } fixed[] = {
            {"aac-drc-album-mode", 0},       {"aac-drc-boost-level", 127},
            {"aac-drc-cut-level", 127},      {"aac-drc-effect-type", 3},
            {"aac-drc-output-loudness", -1}, {"aac-encoded-target-level", -1},
            {"aac-max-output-channel_count", 8}, {"aac-target-ref-level", 64},
            {"max-output-channel-count", 8},
    };
    for (const auto &entry : fixed) AMediaFormat_setInt32(format, entry.key, entry.value);
    return format;
}

/// The host's audio codec configured by `config` (AAC's AudioSpecificConfig, FLAC's metadata, MP3's frame header),
/// in place of the one before, with the codec's lock held; false where it refuses it.
bool make_audio(AMediaCodec *codec, const uint8_t *config, size_t size) {
    if (codec->type == APKRT_AUDIO_CODEC_FLAC) {
        config = flac_info(config, size);
        size = 34;
        if (config == nullptr) return false;
    }
    apkrt_audio_codec call{};
    call.type = codec->type;
    call.data = reinterpret_cast<uintptr_t>(config);
    call.size = size;
    if (audio_call(APKRT_AUDIO_CODEC_CREATE, reinterpret_cast<uintptr_t>(&call)) != 0) return false;
    // FLAC's samples come as 32 bits, the others' as 16.
    if (!resize(&codec->pcm, size_t{call.frames} * call.channels * 4)) {
        audio_call(APKRT_AUDIO_CODEC_DESTROY, call.id);
        return false;
    }
    if (codec->decoder != 0) audio_call(APKRT_AUDIO_CODEC_DESTROY, codec->decoder);
    codec->decoder = call.id;
    codec->channels = call.channels;
    codec->unit_frames = static_cast<uint32_t>(call.frames);
    codec->decoded = AudioShape{static_cast<int32_t>(call.rate), static_cast<int32_t>(call.channels),
                                codec->type == APKRT_AUDIO_CODEC_AAC ? channel_mask(call.channels) : 0};
    return true;
}

/// One unit decoded by the host at `at` frames into the codec's pcm, with the codec's lock held: the frames it
/// gave, or -1 for an error.
long decode_unit(AMediaCodec *codec, const uint8_t *data, size_t size, size_t at) {
    const size_t frame_bytes = size_t{codec->channels} * (codec->type == APKRT_AUDIO_CODEC_FLAC ? 4 : 2);
    if (codec->decoder == 0 || !resize(&codec->pcm, (at + codec->unit_frames) * frame_bytes)) return -1;
    apkrt_audio_codec call{};
    call.id = codec->decoder;
    call.data = reinterpret_cast<uintptr_t>(data);
    call.size = size;
    call.out = reinterpret_cast<uintptr_t>(codec->pcm.data + at * frame_bytes);
    call.capacity = codec->unit_frames * frame_bytes;
    if (audio_call(APKRT_AUDIO_CODEC_DECODE, reinterpret_cast<uintptr_t>(&call)) != 0) return -1;
    return static_cast<long>(call.frames);
}

/// `frames` 16-bit frames of silence at `at` frames into the codec's pcm; false without memory.
bool silence(AMediaCodec *codec, size_t at, size_t frames) {
    const size_t frame_bytes = size_t{codec->channels} * 2;
    if (!resize(&codec->pcm, (at + frames) * frame_bytes)) return false;
    memset(codec->pcm.data + at * frame_bytes, 0, frames * frame_bytes);
    return true;
}

/// An MP3 buffer's frames decoded one by one into the codec's pcm, as C2SoftMp3Dec's process: the frames, or -1 for
/// a header it does not take, a buffer without a frame or the host's error. Four bytes or fewer after the last frame
/// are left, as its calculateOutSize leaves them.
long decode_mp3(AMediaCodec *codec, const uint8_t *data, size_t size) {
    size_t out = 0;
    if (size <= 4) return -1;
    for (size_t at = 0; at + 4 < size;) {
        int32_t rate = 0, channels = 0;
        size_t length = mp3_frame(data + at, &rate, &channels);
        if (length == 0) return -1;
        if (length > size - at) length = size - at;
        if (codec->decoder == 0 || rate != codec->decoded.rate || channels != codec->decoded.channels) {
            const bool going = codec->decoder != 0 && !codec->first;
            if (!make_audio(codec, data + at, 4)) return -1;  // C2SoftMp3Dec reconfigures for another rate
            // The new codec cuts its delay, where C2SoftMp3Dec goes on decoding [verified:
            // media-codec-decoders-test].
            if (going && !silence(codec, out, kMp3Delay)) return -1;
            if (going) out += kMp3Delay;
        }
        const bool refused = mp3_side_info_refused(data + at, length, channels);
        const long got = refused ? 0 : decode_unit(codec, data + at, length, out);
        if (got < 0) return -1;
        if (got == 0 && !silence(codec, out, codec->unit_frames)) return -1;  // as C2SoftMp3Dec's recoverable errors
        out += got > 0 ? static_cast<size_t>(got) : codec->unit_frames;
        at += length;
    }
    return static_cast<long>(out);
}

/// FLAC's `frames` 32-bit samples in the codec's pcm as the output's encoding, as FLACDecoder's copyTo16Signed and
/// copyToFloat make them from the top of 32 bits: shifted to 16 bits, or a float of full scale 2^31.
void flac_samples(AMediaCodec *codec, size_t frames) {
    const size_t count = frames * codec->channels;
    const auto *words = reinterpret_cast<const int32_t *>(codec->pcm.data);
    if (codec->encoding == kPcmFloat) {
        auto *floats = reinterpret_cast<float *>(codec->pcm.data);
        for (size_t i = 0; i < count; i++) floats[i] = static_cast<float>(words[i]) * (1.0f / 2147483648.0f);
    } else {
        auto *shorts = reinterpret_cast<int16_t *>(codec->pcm.data);
        for (size_t i = 0; i < count; i++) shorts[i] = static_cast<int16_t>(words[i] >> 16);
    }
}

/// A free output buffer's index, or -1.
int free_output(const AMediaCodec *codec) {
    for (int index = 0; index < kOutputs; index++) {
        if (codec->outputs[index].state == Output::kFree) return index;
    }
    return -1;
}

void make_ready(AMediaCodec *codec, int index) {
    codec->outputs[index].state = Output::kReady;
    codec->ready[(codec->ready_head + codec->ready_count) % kOutputs] = index;
    codec->ready_count++;
}

/// `frames` decoded frames in the codec's pcm, trimmed as SkipCutBuffer trims them, into `output`'s bytes: the frames
/// to skip dropped, the rest after the held ones, and all but the padding's last frames out. False without memory.
bool trim(AMediaCodec *codec, size_t frames, Output &output) {
    const size_t frame_bytes = size_t{codec->channels} * sample_bytes(codec->encoding);
    const size_t skipped = frames < codec->skip ? frames : codec->skip;
    codec->skip -= static_cast<uint32_t>(skipped);
    const size_t kept = (frames - skipped) * frame_bytes, held = codec->held.data != nullptr ? codec->held.size : 0;
    if (!resize(&codec->held, held + kept)) return false;
    if (kept > 0) memcpy(codec->held.data + held, codec->pcm.data + skipped * frame_bytes, kept);
    const size_t cut = size_t{codec->cut} * frame_bytes;
    const size_t out = held + kept > cut ? held + kept - cut : 0;
    if (!resize(&output.bytes, out)) return false;
    if (out > 0) {
        memcpy(output.bytes.data, codec->held.data, out);
        memmove(codec->held.data, codec->held.data + out, held + kept - out);
        codec->held.size = held + kept - out;
    }
    return true;
}

/// A unit's frames into the codec's pcm, as its decoder gives them, with the codec's lock held: the frames, or -1 for
/// an error. Sets `time_us` to the output's time.
long unit_frames(AMediaCodec *codec, const Unit &unit, int64_t *time_us) {
    const bool end = (unit.flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0;
    *time_us = unit.time_us;
    if (codec->type == kRawCodec) {
        const size_t frame_bytes = size_t{codec->channels} * sample_bytes(codec->encoding);
        if (!resize(&codec->pcm, unit.bytes.size)) return -1;
        if (unit.bytes.size > 0) memcpy(codec->pcm.data, unit.bytes.data, unit.bytes.size);
        return static_cast<long>(unit.bytes.size / frame_bytes);
    }
    if (codec->type == APKRT_AUDIO_CODEC_MP3) {
        if (unit.bytes.size == 0 && (!codec->gapless || !end)) return 0;
        const long decoded = unit.bytes.size > 0 ? decode_mp3(codec, unit.bytes.data, unit.bytes.size) : 0;
        if (decoded < 0) return -1;
        size_t frames = static_cast<size_t>(decoded);
        const bool cut = codec->first && unit.bytes.size == 0;  // the silence then is what the start's cut takes
        if (codec->first) {
            codec->first = false;
            codec->gapless = true;
            codec->anchor_us = unit.time_us;
        }
        if (end && !cut) {
            const size_t frame_bytes = size_t{codec->channels} * 2;
            if (!resize(&codec->pcm, (frames + kMp3Delay) * frame_bytes)) return -1;
            memset(codec->pcm.data + frames * frame_bytes, 0, kMp3Delay * frame_bytes);
            frames += kMp3Delay;
            codec->gapless = false;
        }
        if (codec->decoded.rate > 0) {
            *time_us = codec->anchor_us + static_cast<int64_t>(codec->processed * 1000000 /
                                                               static_cast<uint64_t>(codec->decoded.rate));
        }
        codec->processed += frames;
        return static_cast<long>(frames);
    }
    if (unit.bytes.size == 0) return 0;
    if (codec->type == APKRT_AUDIO_CODEC_FLAC && codec->stuck) return 0;
    const long frames = decode_unit(codec, unit.bytes.data, unit.bytes.size, 0);
    if (codec->type != APKRT_AUDIO_CODEC_FLAC) return frames;
    // libFLAC reads on past a frame it cannot decode, finds C2SoftFlacDec's input at its end and decodes nothing more
    // until a flush [verified: media-codec-decoders-test].
    if (frames == 0) codec->stuck = true;
    if (frames > 0) flac_samples(codec, static_cast<size_t>(frames));
    return frames;
}

/// The trimming made anew for the decoder's format where its rate or channels are not the trimming's, as
/// updateSkipCutBuffer: configure's delay and padding times the new rate over the trimming's, the frames held back
/// dropped. Without a delay or padding in the format there is no trimming to make anew.
void retrim(AMediaCodec *codec) {
    if (codec->delay <= 0 && codec->padding <= 0) return;
    const AudioShape &now = codec->decoded;
    if (now.rate == codec->cut_rate && now.channels == codec->cut_channels) return;
    int64_t delay = codec->delay > 0 ? codec->delay : 0, padding = codec->padding > 0 ? codec->padding : 0;
    if (now.rate != codec->cut_rate && codec->cut_rate > 0) {
        delay = delay * now.rate / codec->cut_rate;
        padding = padding * now.rate / codec->cut_rate;
    }
    codec->cut_rate = now.rate;
    codec->cut_channels = now.channels;
    codec->skip = static_cast<uint32_t>(delay);
    codec->cut = static_cast<uint32_t>(padding);
    codec->held.size = 0;
}

/// Decodes the waiting units into free output buffers, one each, with the codec's lock held.
void push_audio(AMediaCodec *codec) {
    while (codec->waiting_count > 0 && codec->error == AMEDIA_OK && !codec->ended) {
        Unit &unit = codec->waiting[codec->waiting_head];
        // AAC and FLAC take a configuration; FLAC's first unit is one, flagged or not, as C2SoftFlacDec forces it.
        // MP3's and raw's decoders take a flagged unit as any other.
        const bool takes = codec->type == APKRT_AUDIO_CODEC_AAC || codec->type == APKRT_AUDIO_CODEC_FLAC;
        const bool flac_config = codec->type == APKRT_AUDIO_CODEC_FLAC && codec->decoder == 0 && unit.bytes.size > 0;
        if ((takes && (unit.flags & kUnitConfig) != 0) || flac_config) {
            // The configuration again (an app's, or after a flush) leaves a configured codec as it is.
            if (codec->decoder == 0) {
                if (!make_audio(codec, unit.bytes.data, unit.bytes.size)) {
                    codec->error = AMEDIA_ERROR_UNKNOWN;
                    return;
                }
                // C2SoftFlacDec's STREAMINFO is a configuration update of its own, which the next buffer brings
                // even without frames [verified: media-codec-decoders-test].
                codec->announce = codec->type == APKRT_AUDIO_CODEC_FLAC;
            }
            if ((unit.flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0) {
                unit.bytes.size = 0;  // the end on its own after it, as CCodec's buffer channel queues it
                unit.flags &= ~kUnitConfig;
                continue;
            }
        } else {
            const int index = free_output(codec);
            if (index < 0) return;
            Output &output = codec->outputs[index];
            // A unit with the end of the stream is its bytes, then the end on its own at its time, as CCodec's
            // buffer channel queues them.
            const uint32_t flags_then = unit.flags;
            const bool split = (unit.flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0 && unit.bytes.size > 0;
            if (split) unit.flags &= ~uint32_t{APKRT_VIDEO_DECODER_END_OF_STREAM};
            int64_t time_us = 0;
            const long frames = unit_frames(codec, unit, &time_us);
            const bool changed = (frames > 0 || codec->announce) && !(codec->decoded == codec->attached);
            if (changed) retrim(codec);
            if (frames < 0 || !trim(codec, static_cast<size_t>(frames), output)) {
                codec->error = AMEDIA_ERROR_UNKNOWN;
                return;
            }
            const bool end = (unit.flags & APKRT_VIDEO_DECODER_END_OF_STREAM) != 0;
            if (changed) {
                output.format = audio_format(codec, codec->decoded);
                output.decoded = true;
                codec->attached = codec->decoded;
            }
            codec->announce = false;
            const uint32_t flags = end ? static_cast<uint32_t>(AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) : 0u;
            output.info = AMediaCodecBufferInfo{0, static_cast<int32_t>(output.bytes.size), time_us, flags};
            make_ready(codec, index);
            if (end) codec->ended = true;  // the padding still held is never given out
            if (split) {
                unit.bytes.size = 0;
                unit.flags = flags_then;
                continue;  // the end, as the next unit
            }
        }
        codec->waiting_head = (codec->waiting_head + 1) % kInputs;
        codec->waiting_count--;
    }
}

/// Whether the next ready buffer comes with a format change; it is then the output format, given to the app.
bool take_format(AMediaCodec *codec) {
    if (codec->ready_count == 0) return false;
    Output &next = codec->outputs[codec->ready[codec->ready_head]];
    if (next.format == nullptr) return false;
    AMediaFormat_delete(codec->output_format);
    codec->output_format = next.format;
    next.format = nullptr;
    codec->told_decoded = codec->told_decoded || next.decoded;
    if (next.format_only) {  // the change comes alone, as Android's after a flush
        next.format_only = false;
        next.state = Output::kFree;
        codec->ready_head = (codec->ready_head + 1) % kOutputs;
        codec->ready_count--;
    }
    return true;
}

void pump(AMediaCodec *codec) {
    if (codec->state != AMediaCodec::kStarted) return;
    if (codec->audio) {
        push_audio(codec);
        return;
    }
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
    if (codec->waiting_count >= kInputs || codec->input_ended) return -1;
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
    for (auto &output : codec->outputs) {
        output.state = Output::kFree, output.token = 0;  // the runtime ended the tokens
        output.format_only = false;
        AMediaFormat_delete(output.format);
        output.format = nullptr;
    }
    for (bool &dequeued : codec->input_dequeued) dequeued = false;
    codec->ended = false;
    codec->input_ended = false;
}

/// Joins an asynchronous thread that was stopped, with the codec's lock held, which it lets go while it waits; on
/// the thread itself (a delete from its own callback, which Android does not allow either) it detaches it.
void join_thread(AMediaCodec *codec) {
    if (!codec->thread_joinable) return;
    codec->thread_joinable = false;
    if (pthread_equal(codec->thread, pthread_self())) {
        pthread_detach(codec->thread);
        return;
    }
    pthread_mutex_unlock(&codec->mutex);
    pthread_join(codec->thread, nullptr);
    pthread_mutex_lock(&codec->mutex);
}

/// Ends the asynchronous thread, with the codec's lock held. Stopped from its own callback (a stop of the app's) the
/// thread stays joinable, as after the codec released itself (e2c3), and the next start or the delete joins it, so
/// that a delete on another thread never frees the codec while the thread still returns into it.
void stop_thread(AMediaCodec *codec) {
    if (!codec->thread_running) return;
    codec->stopping = true;
    codec->thread_running = false;
    if (!pthread_equal(codec->thread, pthread_self())) join_thread(codec);
}

void end_decoder(AMediaCodec *codec) {
    if (codec->decoder != 0 && codec->audio) audio_call(APKRT_AUDIO_CODEC_DESTROY, codec->decoder);
    if (codec->decoder != 0 && !codec->audio) video_call(APKRT_VIDEO_DECODER_DESTROY, codec->decoder);
    codec->decoder = 0;
    drop_buffers(codec);
    codec->error = AMEDIA_OK;
}

/// Ends the surface and the codec's claim of it, with the codec's lock held.
void end_window(AMediaCodec *codec) {
    if (codec->window != nullptr) {
        window_functions()->unclaim(codec->window);
        ANativeWindow_release(codec->window);
    }
    codec->window = nullptr;
    codec->host_window = 0;
}

/// What release_frame did with a frame.
enum class Released { kDropped, kRendered, kWindowGone };

/// A surface decoder's frame of `token` drawn into a slot of the window and queued at `timestamp_ns`, or with
/// `render` false dropped, with the codec's lock held. A slot the host cannot draw into goes back free, and the
/// frame with it. Into a host window the host draws and presents it itself, at `timestamp_ns` when `at_time`.
Released release_frame(AMediaCodec *codec, uint64_t token, bool render, int64_t timestamp_ns, bool at_time) {
    apkrt_video_decoder call{};
    call.id = codec->decoder;
    call.data = token;
    if (render && codec->host_window != 0) {
        call.size = codec->host_window;
        call.time_us = at_time && timestamp_ns > 0 ? timestamp_ns / 1000 : 0;
        call.flags = codec->scaling;
        const long rendered = video_call(APKRT_VIDEO_DECODER_RENDER_WINDOW, reinterpret_cast<uintptr_t>(&call));
        return rendered == 0         ? Released::kRendered
               : rendered == -ENODEV ? Released::kWindowGone
                                     : Released::kDropped;  // the background's -EIO among them: no frame shows
    }
    const WindowFunctions *functions = window_functions();
    AHardwareBuffer *buffer = nullptr;
    int32_t slot = -1;
    uint64_t buffer_id = 0;
    if (!render || functions == nullptr || functions->dequeue(codec->window, &buffer, &slot) != 0) {
        video_call(APKRT_VIDEO_DECODER_DISCARD, reinterpret_cast<uintptr_t>(&call));
        return Released::kDropped;
    }
    if (AHardwareBuffer_getId(buffer, &buffer_id) != 0) buffer_id = 0;
    call.size = buffer_id;
    const long rendered = buffer_id != 0 ? video_call(APKRT_VIDEO_DECODER_RENDER, reinterpret_cast<uintptr_t>(&call))
                                         : -EINVAL;
    if (rendered == 0) {
        minato_BufferQueue_settings settings{};
        settings.timestamp = timestamp_ns;
        settings.exact_time = 1;  // a frame at time 0 keeps it, as Android's codec queues its time
        functions->queue(codec->window, slot, &settings);
        return Released::kRendered;
    }
    {
        if (rendered == -EINVAL && buffer_id != 0 && !codec->told_format) {
            // Android configures such a surface too and its consumer never gets an image (an RGB_565 reader).
            AHardwareBuffer_Desc description{};
            AHardwareBuffer_describe(buffer, &description);
            __android_log_print(ANDROID_LOG_WARN, "AMediaCodec", "no frame drawn into a buffer of format %u",
                                description.format);
            codec->told_format = true;
        }
        if (buffer_id == 0) video_call(APKRT_VIDEO_DECODER_DISCARD, reinterpret_cast<uintptr_t>(&call));
        functions->cancel(codec->window, slot);
    }
    return Released::kDropped;
}

/// The codec released by itself, its window gone, with the codec's lock held: no decoder, no surface, no more calls.
/// Its thread is told to stop and never joined here, as the caller is the app's thread of a releaseOutputBuffer, which
/// may hold a lock of the app's that the thread's callback waits for; the delete joins it.
void release_itself(AMediaCodec *codec) {
    codec->stopping = true;
    codec->thread_running = false;
    end_decoder(codec);
    end_window(codec);
    codec->state = AMediaCodec::kReleased;
}

/// releaseOutputBuffer and releaseOutputBufferAtTime: the buffer free again, its frame drawn first when `render`, at
/// `timestamp_ns` (below 0: the frame's time, presented at once); the frame rendered callback after it.
media_status_t release_output(AMediaCodec *codec, size_t index, bool render, int64_t timestamp_ns) {
    if (codec == nullptr || index >= kOutputs) return AMEDIA_ERROR_INVALID_PARAMETER;
    AMediaCodecOnFrameRendered callback = nullptr;
    void *userdata = nullptr;
    int64_t time_us = 0, rendered_ns = 0;
    {
        Lock hold(&codec->mutex);
        Output &output = codec->outputs[index];
        if (codec->state != AMediaCodec::kStarted || output.state != Output::kHeld) {
            return AMEDIA_ERROR_INVALID_OPERATION;
        }
        if (output.token != 0) {
            time_us = output.info.presentationTimeUs;
            rendered_ns = timestamp_ns >= 0 ? timestamp_ns : minato_media_time_ns(time_us);
            const Released released = release_frame(codec, output.token, render, rendered_ns, timestamp_ns >= 0);
            output.token = 0;
            if (released == Released::kWindowGone) {
                release_itself(codec);
                return AMEDIA_ERROR_INVALID_OPERATION;
            }
            if (released == Released::kRendered) {
                callback = codec->rendered;
                userdata = codec->rendered_userdata;
            }
        }
        output.state = Output::kFree;
        pump(codec);
    }
    if (callback != nullptr) callback(codec, userdata, time_us, rendered_ns);
    return AMEDIA_OK;
}

/// Asynchronous mode's thread: pumps and calls the app back, outside the lock, until stop.
void *run_callbacks(void *argument) {
    auto *codec = static_cast<AMediaCodec *>(argument);
    pthread_mutex_lock(&codec->mutex);
    const unsigned generation = codec->generation;
    const auto running = [&] { return !codec->stopping && codec->generation == generation; };
    while (running()) {
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
        if (codec->ready_count > 0 && running()) {
            if (take_format(codec) || (codec->format_changed && codec->output_format != nullptr)) {
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
            if (codec->ready_count > 0 && running()) {
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

/// configure for an audio codec, with the codec's lock held.
media_status_t configure_audio(AMediaCodec *codec, AMediaFormat *asked) {
    const char *mime = nullptr;
    if (!AMediaFormat_getString(asked, AMEDIAFORMAT_KEY_MIME, &mime) || audio_type(mime) != codec->type) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    codec->codec_data.size = 0;
    void *data = nullptr;
    size_t size = 0;
    // AAC's AudioSpecificConfig goes to the host whole; FLAC's metadata may hold pictures, of which only STREAMINFO
    // goes.
    const size_t most =
            codec->type == APKRT_AUDIO_CODEC_AAC ? APKRT_AUDIO_CODEC_MAX_CONFIG : APKRT_AUDIO_CODEC_MAX_UNIT;
    if (AMediaFormat_getBuffer(asked, AMEDIAFORMAT_KEY_CSD_0, &data, &size) && size > 0) {
        if (size > most || !resize(&codec->codec_data, size)) return AMEDIA_ERROR_INVALID_PARAMETER;
        memcpy(codec->codec_data.data, data, size);
    }
    // The decoders' input buffers: C2SoftAacDec's and C2SoftMp3Dec's 8192 bytes, C2SoftFlacDec's 32768 and
    // C2SoftRawDec's 65536, or the format's max-input-size where larger.
    const size_t least = codec->type == APKRT_AUDIO_CODEC_FLAC ? 32768 : codec->type == kRawCodec ? 65536 : 8192;
    int32_t max_input = 0;
    size_t capacity = AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, &max_input) &&
                                      static_cast<size_t>(max_input) > least
                              ? static_cast<size_t>(max_input)
                              : least;
    if (capacity > APKRT_AUDIO_CODEC_MAX_UNIT) capacity = APKRT_AUDIO_CODEC_MAX_UNIT;
    for (auto &input : codec->inputs) {
        if (!resize(&input, capacity)) return AMEDIA_ERROR_UNKNOWN;
    }
    if (codec->input_format == nullptr) codec->input_format = AMediaFormat_new();
    if (codec->input_format == nullptr) return AMEDIA_ERROR_UNKNOWN;
    AMediaFormat_copy(codec->input_format, asked);
    // CCodec refuses an audio format without its rate or channels [verified: media-codec-decoders-test]; neither
    // can be 0 or less for the decoders' ranges either.
    int32_t rate = 0, channels = 0;
    if (!AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate) ||
        !AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels) || rate <= 0 || channels <= 0) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (!AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_ENCODER_DELAY, &codec->delay) || codec->delay < 0) {
        codec->delay = -1;
    }
    if (!AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_ENCODER_PADDING, &codec->padding) || codec->padding < 0) {
        codec->padding = -1;
    }
    // The PCM encodings FLAC's and raw's decoders take; another one is 16-bit here (unverified on Android).
    int32_t encoding = kPcm16;
    if (!AMediaFormat_getInt32(asked, AMEDIAFORMAT_KEY_PCM_ENCODING, &encoding)) encoding = kPcm16;
    const bool flac_takes = encoding == kPcm16 || encoding == kPcmFloat;
    const bool raw_takes = flac_takes || encoding == kPcm8 || encoding == kPcm24Packed || encoding == kPcm32;
    codec->encoding = (codec->type == APKRT_AUDIO_CODEC_FLAC && flac_takes) || (codec->type == kRawCodec && raw_takes)
                              ? encoding
                              : kPcm16;
    codec->configured = AudioShape{rate, channels, 0};
    AMediaFormat_delete(codec->output_format);
    codec->output_format = audio_format(codec, codec->configured);
    codec->told_decoded = false;
    codec->state = AMediaCodec::kConfigured;
    return AMEDIA_OK;
}

/// start for an audio codec, with the codec's lock held: the host's codec now where csd-0 configures it, else at
/// the unit that does.
media_status_t start_audio(AMediaCodec *codec) {
    const bool configures = codec->type == APKRT_AUDIO_CODEC_AAC || codec->type == APKRT_AUDIO_CODEC_FLAC;
    if (configures && codec->codec_data.data != nullptr && codec->codec_data.size > 0 &&
        !make_audio(codec, codec->codec_data.data, codec->codec_data.size)) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    if (codec->type == kRawCodec) {
        codec->channels = static_cast<uint32_t>(codec->configured.channels);
        codec->decoded = codec->configured;
    }
    codec->skip = codec->delay > 0 ? static_cast<uint32_t>(codec->delay) : 0;
    codec->cut = codec->padding > 0 ? static_cast<uint32_t>(codec->padding) : 0;
    codec->cut_rate = codec->configured.rate;
    codec->cut_channels = codec->configured.channels;
    codec->held.size = 0;
    codec->attached = AudioShape{-1, -1, -1};  // the first frames come with the decoder's format
    codec->first = true;
    codec->gapless = false;
    codec->stuck = false;
    codec->processed = 0;
    return AMEDIA_OK;
}

/// flush for an audio codec, with the codec's lock held. Configure's format comes again on an empty buffer while the
/// app has not been given a decoded one, as Android's resume reports it, and the decoded one with the next frames.
media_status_t flush_audio(AMediaCodec *codec) {
    if (codec->decoder != 0 && audio_call(APKRT_AUDIO_CODEC_FLUSH, codec->decoder) != 0) return AMEDIA_ERROR_UNKNOWN;
    drop_buffers(codec);
    codec->skip = codec->delay > 0 ? static_cast<uint32_t>(codec->delay) : 0;
    codec->held.size = 0;
    codec->first = true;
    codec->stuck = false;
    codec->processed = 0;
    if (!codec->told_decoded) {
        Output &output = codec->outputs[0];
        output.format = audio_format(codec, codec->configured);
        output.decoded = false;
        output.format_only = true;
        output.bytes.size = 0;
        output.info = AMediaCodecBufferInfo{0, 0, 0, 0};
        make_ready(codec, 0);
        codec->attached = codec->configured;
    }
    return AMEDIA_OK;
}

/// The end of start: the codec started, and in asynchronous mode its thread, with the codec's lock held.
media_status_t start_running(AMediaCodec *codec) {
    codec->format_changed = false;
    codec->state = AMediaCodec::kStarted;
    if (codec->asynchronous) {
        codec->stopping = false;
        codec->generation++;
        codec->thread_running = pthread_create(&codec->thread, nullptr, run_callbacks, codec) == 0;
        codec->thread_joinable = codec->thread_running;
    }
    return AMEDIA_OK;
}

AMediaCodec *make_codec(const minato_media_codec_info *info) {
    if (info == nullptr || info->encoder) return nullptr;
    auto *codec = static_cast<AMediaCodec *>(calloc(1, sizeof(AMediaCodec)));
    if (codec == nullptr) return nullptr;
    pthread_mutex_init(&codec->mutex, nullptr);
    codec->info = info;
    codec->audio = audio_type(info->type) != 0;
    codec->type = codec->audio ? audio_type(info->type) : decoder_type(info->type);
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
    join_thread(codec);
    end_decoder(codec);
    end_window(codec);
    pthread_mutex_unlock(&codec->mutex);
    AMediaFormat_delete(codec->input_format);
    AMediaFormat_delete(codec->output_format);
    free(codec->codec_data.data);
    free(codec->pcm.data);
    free(codec->held.data);
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
    uint64_t host = 0;
    if ((surface != nullptr && !is_surface(surface, &host)) || crypto != nullptr ||
        (flags & AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != 0) {
        return AMEDIA_ERROR_UNSUPPORTED;  // no crypto, no encoder
    }
    auto *asked = const_cast<AMediaFormat *>(format);
    const char *mime = nullptr;
    int32_t width = 0, height = 0, max_input = 0;
    if (codec->audio) return configure_audio(codec, asked);
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
        if (window_functions()->claim(surface) != 0) return AMEDIA_ERROR_UNKNOWN;  // another producer's
        ANativeWindow_acquire(surface);
        codec->window = surface;
        codec->host_window = host;
    }
    codec->state = AMediaCodec::kConfigured;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_setAsyncNotifyCallback(AMediaCodec *codec, AMediaCodecOnAsyncNotifyCallback callback,
                                                  void *userdata) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state == AMediaCodec::kStarted || codec->state == AMediaCodec::kReleased) {
        return AMEDIA_ERROR_INVALID_OPERATION;
    }
    codec->callbacks = callback;
    codec->userdata = userdata;
    codec->asynchronous = callback.onAsyncInputAvailable != nullptr || callback.onAsyncOutputAvailable != nullptr;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_start(AMediaCodec *codec) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    join_thread(codec);  // one stopped from its own callback
    if (codec->state != AMediaCodec::kConfigured) return AMEDIA_ERROR_INVALID_OPERATION;
    if (codec->audio) {
        const media_status_t started = start_audio(codec);
        if (started != AMEDIA_OK) return started;
        return start_running(codec);
    }
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
    return start_running(codec);
}

media_status_t AMediaCodec_stop(AMediaCodec *codec) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state == AMediaCodec::kReleased) return AMEDIA_ERROR_INVALID_OPERATION;
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
    if (codec->audio) return flush_audio(codec);
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
    if (codec->input_ended) {
        codec->input_dequeued[index] = false;  // after the end of the stream: ignored, as CCodec's buffer channel does
        return AMEDIA_OK;
    }
    if (codec->waiting_count >= kInputs) return AMEDIA_ERROR_INVALID_OPERATION;  // free_input keeps this from coming
    Unit &unit = codec->waiting[(codec->waiting_head + codec->waiting_count) % kInputs];
    if (!resize(&unit.bytes, size)) return AMEDIA_ERROR_UNKNOWN;
    if (size > 0) memcpy(unit.bytes.data, input.data + offset, size);
    unit.time_us = static_cast<int64_t>(time);
    unit.flags = (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0 ? APKRT_VIDEO_DECODER_END_OF_STREAM : 0;
    if (codec->audio && (flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0) unit.flags |= kUnitConfig;
    // An audio codec takes no input after the end of the stream until a flush, as CCodec's buffer channel.
    codec->input_ended = codec->audio && (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
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
    if (take_format(codec)) return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
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
    if (codec->window == nullptr || codec->state == AMediaCodec::kMade || codec->state == AMediaCodec::kReleased) {
        return AMEDIA_ERROR_INVALID_OPERATION;
    }
    uint64_t host = 0;
    if (!is_surface(surface, &host)) return AMEDIA_ERROR_UNSUPPORTED;
    if (surface == codec->window) return AMEDIA_OK;
    if (window_functions()->claim(surface) != 0) return AMEDIA_ERROR_UNKNOWN;  // another producer's
    ANativeWindow_acquire(surface);
    end_window(codec);
    codec->window = surface;
    codec->host_window = host;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_setOnFrameRenderedCallback(AMediaCodec *codec, AMediaCodecOnFrameRendered callback,
                                                      void *userdata) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state == AMediaCodec::kReleased) return AMEDIA_ERROR_INVALID_OPERATION;
    codec->rendered = callback;
    codec->rendered_userdata = userdata;
    return AMEDIA_OK;
}

int minato_AMediaCodec_isReleased(AMediaCodec *codec) {
    if (codec == nullptr) return 0;
    Lock hold(&codec->mutex);
    return codec->state == AMediaCodec::kReleased ? 1 : 0;
}

int minato_AMediaCodec_setVideoScalingMode(AMediaCodec *codec, int mode) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    if (codec->state == AMediaCodec::kReleased) return AMEDIA_ERROR_INVALID_OPERATION;
    // MediaCodec's VIDEO_SCALING_MODE_SCALE_TO_FIT (1) stretches, SCALE_TO_FIT_WITH_CROPPING (2) crops; another mode
    // is refused, for MediaCodec's IllegalArgumentException.
    if (mode != 1 && mode != 2) return AMEDIA_ERROR_INVALID_PARAMETER;
    codec->scaling = mode == 2 ? APKRT_VIDEO_DECODER_CROP : 0;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_setParameters(AMediaCodec *codec, const AMediaFormat *) {
    if (codec == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    Lock hold(&codec->mutex);
    return codec->state == AMediaCodec::kReleased ? AMEDIA_ERROR_INVALID_OPERATION : AMEDIA_OK;  // none it takes
}

}  // extern "C"
