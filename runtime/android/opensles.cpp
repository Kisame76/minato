// libOpenSLES.so (ADR-0036, decision 7), written for Minato from the NDK's OpenSL ES headers and Android 15's answers
// (fixtures/apps/opensl-test, and probe programs on the emulator for the rest): engines, output mixes, and audio
// players from an (Android) simple buffer queue of PCM into an output mix, each on a FLOAT stream of libaaudio in
// callback mode. The stream's thread pulls the queue while the player plays, as wilhelm's AudioTrack callback does
// [from memory of AOSP, not read again]: frames from the front buffer on, and a buffer that ends calls the app back
// before the next frames are taken, at the next pull, so that a callback that refills plays on without a gap. A pull
// that finds the queue empty stops the stream until the next Enqueue, so an idle player costs nothing. The pull claims
// its frames from libaaudio under the player's lock, which also orders Clear's flush and STOPPED's stop: one of them
// drops what the pull took before it, also when it comes while the app's callback runs within the pull, and nothing the
// pull takes after it. A mono source plays on two channels, so that its stereo position can pan it. VOLUME is a gain,
// computed as Android's volume update computes it. The position counts the frames heard since the last STOPPED, Clear
// or empty queue, as Android 15 answers; the play events are HEADATMARKER, HEADATNEWPOS and HEADATEND, the ones
// Android 15 gives a buffer queue. ANDROIDCONFIGURATION works before Realize: its performance mode becomes the
// stream's, and its stream type is kept and not used. EFFECTSEND keeps its direct level and does not apply it; since an
// output mix here has no effects, the calls that name one answer PARAMETER_INVALID, as Android's do for an effect that
// is not of the output mix. Once a Destroy begins, no callback starts; an output mix that players play into, and an
// engine whose objects live, ignore their Destroy, as Android 15's. Every other part answers
// SL_RESULT_FEATURE_UNSUPPORTED and logs once.
//
// Audio recorders (ADR-0064): from the default audio input device into a buffer queue of PCM, on an input stream of
// libaaudio in callback mode. Without the microphone grant the recorder is created, as Android 15's, and its Realize
// fails with SL_RESULT_CONTENT_UNSUPPORTED (audio-input-test).
// The stream's thread fills the front buffer, tells the app when one is full, and loses frames that find the queue
// empty; RECORD has the states, the position, the marker and the new-position events.
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

#define SL_LOG(priority, ...) __android_log_print(ANDROID_LOG_##priority, "libOpenSLES", __VA_ARGS__)

// The 53 interface IDs as Android 15 exports them, read from the emulator's /system/lib64/libOpenSLES.so
// (AE3A.240806.019), whose SL_IID_* point into libwilhelm.so's SL_IID_array (AOSP, Apache 2.0): Khronos's UUIDs of
// OpenSL ES 1.0.1 and Android's own for its extensions. The array's IDs of OpenMAX AL are left out.
#define MINATO_IIDS(X) \
    X(3DCOMMIT, 0x3564ad80, 0xdd0f, 0x11db, 0x9e19, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(3DDOPPLER, 0xb45c9a80, 0xddd2, 0x11db, 0xb028, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(3DGROUPING, 0xebe844e0, 0xddd2, 0x11db, 0xb510, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(3DLOCATION, 0x2b878020, 0xddd3, 0x11db, 0x8a01, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(3DMACROSCOPIC, 0x5089aec0, 0xddd3, 0x11db, 0x9ad3, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(3DSOURCE, 0x70bc7b00, 0xddd3, 0x11db, 0xa873, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(AUDIODECODERCAPABILITIES, 0x3fe5a3a0, 0xfcc6, 0x11db, 0x94ac, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(AUDIOENCODER, 0xd7d5af7a, 0x351c, 0x41a6, 0x94ec, 0x1a, 0xc9, 0x5c, 0x71, 0x82, 0x2c) \
    X(AUDIOENCODERCAPABILITIES, 0x0f52a340, 0xfcd1, 0x11db, 0xa993, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(AUDIOIODEVICECAPABILITIES, 0xb2564dc0, 0xddd3, 0x11db, 0xbd62, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(BASSBOOST, 0x0634f220, 0xddd4, 0x11db, 0xa0fc, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(BUFFERQUEUE, 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(DEVICEVOLUME, 0xe1634760, 0xf3e2, 0x11db, 0x9ca9, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(DYNAMICINTERFACEMANAGEMENT, 0x63936540, 0xf775, 0x11db, 0x9cc4, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(DYNAMICSOURCE, 0xc55cc100, 0x038b, 0x11dc, 0xbb45, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(EFFECTSEND, 0x56e7d200, 0xddd4, 0x11db, 0xaefb, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ENGINE, 0x8d97c260, 0xddd4, 0x11db, 0x958f, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ENGINECAPABILITIES, 0x8320d0a0, 0xddd5, 0x11db, 0xa1b1, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ENVIRONMENTALREVERB, 0xc2e5d5f0, 0x94bd, 0x4763, 0x9cac, 0x4e, 0x23, 0x4d, 0x06, 0x83, 0x9e) \
    X(EQUALIZER, 0x0bed4300, 0xddd6, 0x11db, 0x8f34, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(LED, 0x2cc1cd80, 0xddd6, 0x11db, 0x807e, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(METADATAEXTRACTION, 0xaa5b1f80, 0xddd6, 0x11db, 0xac8e, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(METADATATRAVERSAL, 0xc43662c0, 0xddd6, 0x11db, 0xa7ab, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(MIDIMESSAGE, 0xddf4a820, 0xddd6, 0x11db, 0xb174, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(MIDIMUTESOLO, 0x039eaf80, 0xddd7, 0x11db, 0x9a02, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(MIDITEMPO, 0x1f347400, 0xddd7, 0x11db, 0xa7ce, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(MIDITIME, 0x3da51de0, 0xddd7, 0x11db, 0xaf70, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(MUTESOLO, 0x5a28ebe0, 0xddd7, 0x11db, 0x8220, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(NULL, 0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, 0x46, 0x57, 0xe6, 0x79, 0x52, 0x10) \
    X(OBJECT, 0x79216360, 0xddd7, 0x11db, 0xac16, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(OUTPUTMIX, 0x97750f60, 0xddd7, 0x11db, 0x92b1, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(PITCH, 0xc7e8ee00, 0xddd7, 0x11db, 0xa42c, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(PLAY, 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(PLAYBACKRATE, 0x2e3b2a40, 0xddda, 0x11db, 0xa349, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(PREFETCHSTATUS, 0x2a41ee80, 0xddd8, 0x11db, 0xa41f, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(PRESETREVERB, 0x47382d60, 0xddd8, 0x11db, 0xbf3a, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(RATEPITCH, 0x61b62e60, 0xddda, 0x11db, 0x9eb8, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(RECORD, 0xc5657aa0, 0xdddb, 0x11db, 0x82f7, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(SEEK, 0xd43135a0, 0xdddc, 0x11db, 0xb458, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(THREADSYNC, 0xf6ac6b40, 0xdddc, 0x11db, 0xa62e, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(VIBRA, 0x169a8d60, 0xdddd, 0x11db, 0x923d, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(VIRTUALIZER, 0x37cc2c00, 0xdddd, 0x11db, 0x8577, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(VISUALIZATION, 0xe46b26a0, 0xdddd, 0x11db, 0x8afd, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(VOLUME, 0x09e8ede0, 0xddde, 0x11db, 0xb4f6, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDEFFECT, 0xae12da60, 0x99ac, 0x11df, 0xb456, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDEFFECTCAPABILITIES, 0x6a4f6d60, 0xb5e6, 0x11df, 0xbb3b, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDEFFECTSEND, 0x7be462c0, 0xbc43, 0x11df, 0x8670, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDCONFIGURATION, 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDSIMPLEBUFFERQUEUE, 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDBUFFERQUEUESOURCE, 0x7fc1a460, 0xeec1, 0x11df, 0xa306, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDACOUSTICECHOCANCELLATION, 0x7b491460, 0x8d4d, 0x11e0, 0xbd61, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDAUTOMATICGAINCONTROL, 0x0a8abfe0, 0x654c, 0x11e0, 0xba26, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b) \
    X(ANDROIDNOISESUPPRESSION, 0x58b4b260, 0x8e06, 0x11e0, 0xaa8e, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)

namespace {

#define MINATO_INDEX(name, ...) k_##name,
enum Interface : int { MINATO_IIDS(MINATO_INDEX) kInterfaces };
#undef MINATO_INDEX

#define MINATO_VALUE(name, a, b, c, d, n0, n1, n2, n3, n4, n5) {a, b, c, d, {n0, n1, n2, n3, n4, n5}},
const SLInterfaceID_ kIids[kInterfaces] = {MINATO_IIDS(MINATO_VALUE)};
#undef MINATO_VALUE

#define MINATO_NAME(name, ...) "SL_IID_" #name,
const char* const kIidNames[kInterfaces] = {MINATO_IIDS(MINATO_NAME)};
#undef MINATO_NAME

// MARK: - Logs

// What is logged once: each interface that a creation asks for and does not get, then these.
enum Once : int {
    kUnknownInterface = kInterfaces,
    kDataLocator,
    kRecorder,
    kLedDevice,
    kVibraDevice,
    kMidiPlayer,
    kListener,
    k3DGroup,
    kMetadataExtractor,
    kExtensionObject,
    kEngineQueries,
    kJavaProxy,
    kDisconnected,
    kDestroyInCallback,
    kOnce
};
char logged[kOnce];

bool first_time(int what) {
    return !__atomic_exchange_n(&logged[what], 1, __ATOMIC_RELAXED);
}

SLresult unsupported(int what, const char* name) {
    if (first_time(what)) SL_LOG(WARN, "%s is not supported", name);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

// MARK: - Objects and their interfaces

enum class Kind : int { engine, output_mix, player, recorder };

struct Object;

/// An interface as its object holds it: the app's handle points at `vtable`.
struct Itf {
    const void* vtable;
    Object* object;
};

struct Object {
    Itf self;  // the SLObjectItf, so that the handle of the object is its address
    Kind kind;
    pthread_mutex_t lock;
    SLuint32 state;
    slObjectCallback callback;
    void* callback_context;
    uint64_t asked;       // the interfaces the creation asked for, by Interface
    Object* maker;        // the engine that made it; NULL for an engine
    uint32_t dependants;  // what it made or what plays into it, alive (atomic): while there is any, Destroy is ignored
};

struct Engine : Object {
    Itf engine;
};

enum class Sample : int { u8, s16, s24, s32, f32 };

struct Buffer {
    const uint8_t* data;
    SLuint32 size;
};

struct Player : Object {
    Itf play_itf, queue_itf, volume_itf, send_itf, configuration_itf;
    // The source, and the stream it plays on.
    Sample sample;
    int32_t channels;      // the source's
    int32_t out_channels;  // the stream's: at least 2
    int32_t rate;          // Hz
    SLuint32 frame_bytes;
    SLint32 stream_type;   // ANDROIDCONFIGURATION's, kept and not used
    SLuint32 performance_mode;
    AAudioStream* stream;
    uint32_t drops;  // the stream's flushes and stops so far
    bool has_stream_thread;
    pthread_t stream_thread;
    bool destroyed;  // Destroy began (atomic): the stream thread takes no frames and calls the app no more
    Object* mix;     // the output mix it plays into
    // PLAY
    SLuint32 play_state;
    bool idle;  // PLAYING, but the stream stopped when the queue ran empty: an Enqueue starts it again
    slPlayCallback play_callback;
    void* play_context;
    SLuint32 event_mask;
    SLmillisecond period, next_position, marker;
    bool has_marker, marker_passed;
    bool track_events;  // the marker and the new positions reach the app, as the AudioTrack's copies of them do
    int64_t origin;     // the stream's frame from which the position counts
    // The buffer queue: `count` buffers from `front` on in a ring of `capacity`.
    Buffer* buffers;
    SLuint32 capacity, front, count, index, consumed;
    bool callback_pending;  // the front buffer ended at a pull: the app hears of it at the next
    slAndroidSimpleBufferQueueCallback queue_callback;
    void* queue_context;
    // VOLUME and EFFECTSEND
    SLmillibel level, direct_level;
    bool mute, stereo_enabled;
    SLpermille stereo_position;
    float gain, gain_left, gain_right;
};

/// An audio recorder (ADR-0064): a buffer queue of PCM that the microphone fills, from a FLOAT input stream of
/// libaaudio in callback mode. A buffer that is full goes to the app's callback, as wilhelm's does; frames that come
/// while the queue is empty are lost.
struct Recorder : Object {
    Itf record_itf, queue_itf, configuration_itf;
    Sample sample;
    int32_t channels;
    int32_t rate;  // Hz
    SLuint32 frame_bytes;
    SLuint32 preset;  // ANDROIDCONFIGURATION's recording preset, kept and not used
    AAudioStream* stream;
    bool destroyed;  // Destroy began (atomic)
    SLuint32 record_state;
    // The buffer queue: `count` buffers from `front` in a ring of `capacity`; `filled` bytes of the front one are in.
    struct Slot {
        uint8_t* data;
        SLuint32 size;
    }* buffers;
    SLuint32 capacity, front, count, index, filled;
    slAndroidSimpleBufferQueueCallback queue_callback;
    void* queue_context;
    // RECORD
    slRecordCallback record_callback;
    void* record_context;
    SLuint32 event_mask;
    SLmillisecond period, next_position, marker;
    bool has_marker, marker_passed;
    int64_t recorded;  // frames in buffers since the last STOPPED or Clear
};

/// Holds a mutex for its scope.
struct Hold {
    explicit Hold(pthread_mutex_t* mutex) : held(mutex) { pthread_mutex_lock(held); }
    ~Hold() { pthread_mutex_unlock(held); }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
    pthread_mutex_t* held;
};

Object* object_of(const void* handle) {
    return static_cast<const Itf*>(handle)->object;
}

Player* player_of(const void* handle) {
    return static_cast<Player*>(object_of(handle));
}

template <typename Handle>
Handle handle_of(Itf* itf) {
    return reinterpret_cast<Handle>(&itf->vtable);
}

uint64_t bit(int index) {
    return uint64_t{1} << index;
}

int interface_of(SLInterfaceID id) {
    for (int index = 0; index < kInterfaces; ++index) {
        if (memcmp(id, &kIids[index], sizeof(SLInterfaceID_)) == 0) return index;
    }
    return -1;
}

/// How an object of `kind` offers an interface: not at all; always; once a creation asks for it; or asked for, and
/// before Realize too.
enum class Offer : int { none, always, asked, asked_early };

Offer offer(Kind kind, int index) {
    if (index == k_OBJECT) return Offer::always;
    switch (kind) {
        case Kind::engine:
            return index == k_ENGINE ? Offer::always : Offer::none;
        case Kind::output_mix:
            return Offer::none;
        case Kind::recorder:
            switch (index) {
                case k_RECORD:
                    return Offer::always;
                case k_BUFFERQUEUE:
                case k_ANDROIDSIMPLEBUFFERQUEUE:
                    return Offer::asked;
                case k_ANDROIDCONFIGURATION:
                    return Offer::asked_early;
                default:
                    return Offer::none;
            }
        case Kind::player:
            switch (index) {
                case k_PLAY:
                    return Offer::always;
                case k_BUFFERQUEUE:
                case k_ANDROIDSIMPLEBUFFERQUEUE:
                case k_VOLUME:
                case k_EFFECTSEND:
                    return Offer::asked;
                case k_ANDROIDCONFIGURATION:
                    return Offer::asked_early;
                default:
                    return Offer::none;
            }
    }
    return Offer::none;
}

/// Takes the interfaces a creation asks for: FEATURE_UNSUPPORTED for a required one that `kind` does not offer.
SLresult take_interfaces(Kind kind, SLuint32 count, const SLInterfaceID* ids, const SLboolean* required,
                         uint64_t* asked) {
    *asked = 0;
    if (count > 0 && (ids == nullptr || required == nullptr)) return SL_RESULT_PARAMETER_INVALID;
    for (SLuint32 at = 0; at < count; ++at) {
        if (ids[at] == nullptr) return SL_RESULT_PARAMETER_INVALID;
        const int index = interface_of(ids[at]);
        if (index >= 0 && offer(kind, index) != Offer::none) {
            *asked |= bit(index);
            continue;
        }
        const SLresult result = index >= 0 ? unsupported(index, kIidNames[index])
                                           : unsupported(kUnknownInterface, "An interface ID Android does not know");
        if (required[at]) return result;
    }
    return SL_RESULT_SUCCESS;
}

bool asked_for(const Object* object, int index) {
    if (index == k_BUFFERQUEUE || index == k_ANDROIDSIMPLEBUFFERQUEUE) {  // one interface under both IDs, as Android's
        return (object->asked & (bit(k_BUFFERQUEUE) | bit(k_ANDROIDSIMPLEBUFFERQUEUE))) != 0;
    }
    return (object->asked & bit(index)) != 0;
}

void* interface_at(Object* object, int index) {
    if (index == k_OBJECT) return &object->self;
    if (object->kind == Kind::engine) return &static_cast<Engine*>(object)->engine;
    if (object->kind == Kind::recorder) {
        auto* recorder = static_cast<Recorder*>(object);
        return index == k_RECORD ? &recorder->record_itf
               : index == k_ANDROIDCONFIGURATION ? &recorder->configuration_itf
                                                 : &recorder->queue_itf;
    }
    auto* player = static_cast<Player*>(object);
    switch (index) {
        case k_PLAY:
            return &player->play_itf;
        case k_BUFFERQUEUE:
        case k_ANDROIDSIMPLEBUFFERQUEUE:
            return &player->queue_itf;
        case k_VOLUME:
            return &player->volume_itf;
        case k_EFFECTSEND:
            return &player->send_itf;
        default:
            return &player->configuration_itf;
    }
}

// MARK: - Players: the stream and its pulls

/// The milliseconds of the queue's frames heard since the position's origin, `read` being the stream's frames read.
SLmillisecond heard(const Player* player, int64_t read) {
    const int64_t frames = read - player->origin;
    return frames > 0 ? static_cast<SLmillisecond>(frames * 1'000 / player->rate) : 0;
}

/// The position starts again from 0 at the stream's frame `origin`, and with it the new positions and the marker.
void restart_position(Player* player, int64_t origin) {
    player->origin = origin;
    player->next_position = player->period;
    player->marker_passed = false;
}

/// A change of the event mask, the marker or the period sets the marker and the period again, as Android's
/// useEventMask sets them on its AudioTrack [from memory of AOSP]: the marker can come again, and the new positions
/// count from the current one.
void set_events_again(Player* player) {
    player->track_events = true;
    player->marker_passed = false;
    player->next_position = heard(player, AAudioStream_getFramesRead(player->stream)) + player->period;
}

/// The gains of the left and right channels, and of all for more than two, as Android's volume update computes them
/// [from memory of AOSP, not read again]: the level in millibels, then the stereo position (a balance for stereo, a
/// pan of constant power for mono), then the mute. EFFECTSEND's direct level is not applied. libm's functions come
/// as builtins: bionic's <math.h> and the compiler's <float.h> both define INFINITY in C++20.
void update_gains(Player* player) {
    constexpr double quarter_pi = 0.78539816339744830962;
    float gain = player->level <= SL_MILLIBEL_MIN
                     ? 0.0f
                     : __builtin_powf(10.0f, static_cast<float>(player->level) / 2000.0f);
    if (player->mute) gain = 0.0f;
    float left = gain, right = gain;
    if (player->stereo_enabled && player->channels == 1) {
        const double angle = static_cast<double>(1'000 + player->stereo_position) * quarter_pi / 1'000.0;
        left *= static_cast<float>(__builtin_cos(angle));
        right *= static_cast<float>(__builtin_sin(angle));
    } else if (player->stereo_enabled && player->channels == 2) {
        if (player->stereo_position > 0) {
            left *= static_cast<float>(1'000 - player->stereo_position) / 1'000.0f;
        } else {
            right *= static_cast<float>(1'000 + player->stereo_position) / 1'000.0f;
        }
    }
    player->gain = gain;
    player->gain_left = left;
    player->gain_right = right;
}

/// `frames` frames from `from` as floats into `to`, with the gains, `load` reading one sample.
template <typename Load>
void convert_frames(const Player* player, const uint8_t* from, float* to, int32_t frames, Load load) {
    const SLuint32 step = player->frame_bytes / static_cast<SLuint32>(player->channels);
    if (player->channels == 1) {
        for (int32_t frame = 0; frame < frames; ++frame, from += step) {
            const float value = load(from);
            *to++ = value * player->gain_left;
            *to++ = value * player->gain_right;
        }
    } else if (player->channels == 2) {
        for (int32_t frame = 0; frame < frames; ++frame) {
            *to++ = load(from) * player->gain_left;
            from += step;
            *to++ = load(from) * player->gain_right;
            from += step;
        }
    } else {
        const int64_t samples = int64_t{frames} * player->channels;
        for (int64_t sample = 0; sample < samples; ++sample, from += step) *to++ = load(from) * player->gain;
    }
}

/// Scaled as AudioFlinger's conversions to float scale them.
void convert(const Player* player, const uint8_t* from, float* to, int32_t frames) {
    switch (player->sample) {
        case Sample::u8:
            convert_frames(player, from, to, frames,
                           [](const uint8_t* at) { return (static_cast<float>(*at) - 128.0f) * (1.0f / 128.0f); });
            break;
        case Sample::s16:
            convert_frames(player, from, to, frames, [](const uint8_t* at) {
                int16_t value;
                memcpy(&value, at, sizeof(value));
                return static_cast<float>(value) * (1.0f / 32768.0f);
            });
            break;
        case Sample::s24:
            convert_frames(player, from, to, frames, [](const uint8_t* at) {
                const uint32_t bits = (uint32_t{at[0]} << 8) | (uint32_t{at[1]} << 16) | (uint32_t{at[2]} << 24);
                return static_cast<float>(static_cast<int32_t>(bits) >> 8) * (1.0f / 8388608.0f);
            });
            break;
        case Sample::s32:
            convert_frames(player, from, to, frames, [](const uint8_t* at) {
                int32_t value;
                memcpy(&value, at, sizeof(value));
                return static_cast<float>(value) * (1.0f / 2147483648.0f);
            });
            break;
        case Sample::f32:
            convert_frames(player, from, to, frames, [](const uint8_t* at) {
                float value;
                memcpy(&value, at, sizeof(value));
                return value;
            });
            break;
    }
}

bool destroyed(const Player* player) {
    return __atomic_load_n(&player->destroyed, __ATOMIC_ACQUIRE);
}

/// Whether the stream's thread may take frames and call the app: while the player plays, until its Destroy begins, as
/// Android's CallbackProtector lets callbacks in. With the lock held.
bool takes(const Player* player) {
    return player->play_state == SL_PLAYSTATE_PLAYING && !destroyed(player);
}

/// The stream's stop, with the lock held; like the flush, it drops what a pull under way took before it (pull).
void stop_stream(Player* player) {
    ++player->drops;
    AAudioStream_requestStop(player->stream);
}

void flush_stream(Player* player) {
    ++player->drops;
    AAudioStream_requestFlush(player->stream);
}

/// The stream's data callback, on its thread: the queue's frames as floats, and zeros for the rest. The app hears of a
/// buffer that ended before the next frames are taken; then come the play events. A pull that finds the queue empty
/// stops the stream, as wilhelm stops its AudioTrack "so it restarts playing faster when new data is enqueued" [from
/// memory of AOSP]: an idle player costs nothing, and its position is 0 until the next frames play. The pull claims its
/// frames under the lock, so that libaaudio drops them only for a flush or a stop after that; one that came while the
/// app's callback ran drops what the pull took before it, and the pull takes on.
aaudio_data_callback_result_t pull(AAudioStream* stream, void* user, void* audio, int32_t frames) {
    auto* player = static_cast<Player*>(user);
    auto* out = static_cast<float*>(audio);
    const int64_t written = AAudioStream_getFramesWritten(stream);  // where these frames go: the ones before are in
    const int64_t read = AAudioStream_getFramesRead(stream);
    pthread_mutex_lock(&player->lock);
    player->stream_thread = pthread_self();
    player->has_stream_thread = true;
    int32_t done = 0;
    uint32_t drops = player->drops;
    while (done < frames && takes(player)) {
        if (player->callback_pending) {
            player->callback_pending = false;
            const slAndroidSimpleBufferQueueCallback callback = player->queue_callback;
            void* context = player->queue_context;
            if (callback != nullptr) {
                pthread_mutex_unlock(&player->lock);
                callback(handle_of<SLAndroidSimpleBufferQueueItf>(&player->queue_itf), context);
                pthread_mutex_lock(&player->lock);
                if (player->drops != drops) {  // a flush or a stop came meanwhile: what the pull took went with it
                    drops = player->drops;
                    done = 0;
                }
            }
            continue;
        }
        if (player->count == 0) break;
        const Buffer& buffer = player->buffers[player->front];
        const auto whole = static_cast<int32_t>((buffer.size - player->consumed) / player->frame_bytes);
        const int32_t taken = whole < frames - done ? whole : frames - done;
        convert(player, buffer.data + player->consumed, out + int64_t{done} * player->out_channels, taken);
        player->consumed += static_cast<SLuint32>(taken) * player->frame_bytes;
        done += taken;
        if (buffer.size - player->consumed < player->frame_bytes) {  // ended; a part of a frame is left out
            player->front = (player->front + 1) % player->capacity;
            --player->count;
            ++player->index;
            player->consumed = 0;
            player->callback_pending = true;
        }
    }
    memset(out + int64_t{done} * player->out_channels, 0,
           static_cast<size_t>(frames - done) * static_cast<size_t>(player->out_channels) * sizeof(float));
    // The queue had fewer frames than the pull asked for: only those are audio. The rest is no silence to play
    // (the app's next buffer may be about to come): the host's read of a short ring is the gap, and counts as the xrun.
    minato_AAudioStream_setCallbackFrames(stream, done);
    if (!takes(player)) {  // a stop, a pause or a Destroy came during the pull
        pthread_mutex_unlock(&player->lock);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    const SLmillisecond now = heard(player, read);
    int positions = 0;
    for (; now >= player->next_position; player->next_position += player->period) ++positions;
    // AudioTrack's marker at frame 0 is none.
    const bool at_marker = player->has_marker && player->marker > 0 && !player->marker_passed && now >= player->marker;
    if (at_marker) player->marker_passed = true;
    const bool at_end = done == 0 && player->count == 0;
    if (at_end) {
        player->idle = true;
        stop_stream(player);  // this pull's zeros are dropped; what the stream holds plays out
        restart_position(player, written);
    } else {
        minato_AAudioStream_claimCallbackFrames(stream);
    }
    const SLuint32 mask = player->track_events ? player->event_mask : player->event_mask & SL_PLAYEVENT_HEADATEND;
    const slPlayCallback callback = player->play_callback;
    void* context = player->play_context;
    pthread_mutex_unlock(&player->lock);
    if (callback == nullptr) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    // As AudioTrack's: the marker, then the new positions; and the end, as wilhelm's pull finds it. None once a Destroy
    // began, which one of them may start.
    const auto play = handle_of<SLPlayItf>(&player->play_itf);
    if ((mask & SL_PLAYEVENT_HEADATMARKER) != 0 && at_marker && !destroyed(player)) {
        callback(play, context, SL_PLAYEVENT_HEADATMARKER);
    }
    for (; (mask & SL_PLAYEVENT_HEADATNEWPOS) != 0 && positions > 0 && !destroyed(player); --positions) {
        callback(play, context, SL_PLAYEVENT_HEADATNEWPOS);
    }
    if ((mask & SL_PLAYEVENT_HEADATEND) != 0 && at_end && !destroyed(player)) {
        callback(play, context, SL_PLAYEVENT_HEADATEND);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void on_stream_error(AAudioStream*, void*, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED && first_time(kDisconnected)) {
        SL_LOG(WARN, "a player's stream is disconnected: the player stays silent");
    }
}

/// Opens the player's stream at Realize, with the lock held.
SLresult open_stream(Player* player) {
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return SL_RESULT_MEMORY_FAILURE;
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, player->out_channels);
    AAudioStreamBuilder_setSampleRate(builder, player->rate);
    // LATENCY and LATENCY_EFFECTS ask for Android's fast track, which is AAudio's LOW_LATENCY.
    AAudioStreamBuilder_setPerformanceMode(builder, player->performance_mode == SL_ANDROID_PERFORMANCE_NONE
                                                        ? AAUDIO_PERFORMANCE_MODE_NONE
                                                    : player->performance_mode == SL_ANDROID_PERFORMANCE_POWER_SAVING
                                                        ? AAUDIO_PERFORMANCE_MODE_POWER_SAVING
                                                        : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, pull, player);
    AAudioStreamBuilder_setErrorCallback(builder, on_stream_error, player);
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &player->stream);
    AAudioStreamBuilder_delete(builder);
    if (opened == AAUDIO_OK) return SL_RESULT_SUCCESS;
    SL_LOG(ERROR, "Realize: the player has no stream (%s)", AAudio_convertResultToText(opened));
    player->stream = nullptr;
    return SL_RESULT_CONTENT_UNSUPPORTED;  // as Android's when its AudioTrack does not start [from memory]
}

/// Closes the player's stream, unless one of its callbacks destroys it: then the stream stops, and the player is left.
/// From now on the stream's thread calls the app no more.
bool close_stream(Player* player) {
    pthread_mutex_lock(&player->lock);
    __atomic_store_n(&player->destroyed, true, __ATOMIC_RELEASE);
    if (player->has_stream_thread && pthread_equal(pthread_self(), player->stream_thread)) {
        stop_stream(player);
        pthread_mutex_unlock(&player->lock);
        if (first_time(kDestroyInCallback)) SL_LOG(ERROR, "Destroy of a player from its own callback: it only stops");
        return false;
    }
    AAudioStream* stream = player->stream;
    pthread_mutex_unlock(&player->lock);
    if (stream != nullptr) AAudioStream_close(stream);  // ends its thread, after a callback under way
    return true;
}

// MARK: - Recorders: the stream and its fills

bool recorder_destroyed(const Recorder* recorder) {
    return __atomic_load_n(&recorder->destroyed, __ATOMIC_ACQUIRE);
}

/// `frames` float frames as the recorder's sample format into `to`.
void convert_in(const Recorder* recorder, const float* from, uint8_t* to, int32_t frames) {
    const int64_t samples = int64_t{frames} * recorder->channels;
    for (int64_t index = 0; index < samples; ++index) {
        const float value = from[index] < -1.0f ? -1.0f : from[index] > 1.0f ? 1.0f : from[index];
        switch (recorder->sample) {
            case Sample::u8:
                *to++ = static_cast<uint8_t>(value * 127.0f + 128.0f + 0.5f);
                break;
            case Sample::s16: {
                const auto sample =
                    static_cast<int16_t>(value >= 1.0f ? 32767 : static_cast<int32_t>(value * 32768.0f));
                memcpy(to, &sample, sizeof(sample));
                to += sizeof(sample);
                break;
            }
            case Sample::s24: {
                const int32_t sample = value >= 1.0f ? 8388607 : static_cast<int32_t>(value * 8388608.0f);
                *to++ = static_cast<uint8_t>(sample);
                *to++ = static_cast<uint8_t>(sample >> 8);
                *to++ = static_cast<uint8_t>(sample >> 16);
                break;
            }
            case Sample::s32: {
                const int32_t sample =
                    value >= 1.0f ? INT32_MAX : static_cast<int32_t>(static_cast<double>(value) * 2147483648.0);
                memcpy(to, &sample, sizeof(sample));
                to += sizeof(sample);
                break;
            }
            case Sample::f32:
                memcpy(to, &value, sizeof(value));
                to += sizeof(value);
                break;
        }
    }
}

/// The stream's data callback, on its thread: the captured frames go into the queue's front buffer; a buffer that
/// fills is told to the app after the frames are in. Frames that find the queue empty are lost.
aaudio_data_callback_result_t capture(AAudioStream*, void* user, void* audio, int32_t frames) {
    auto* recorder = static_cast<Recorder*>(user);
    const auto* in = static_cast<const float*>(audio);
    pthread_mutex_lock(&recorder->lock);
    int32_t done = 0;
    uint32_t completed = 0;
    while (done < frames && recorder->record_state == SL_RECORDSTATE_RECORDING && !recorder_destroyed(recorder) &&
           recorder->count > 0) {
        const Recorder::Slot& buffer = recorder->buffers[recorder->front];
        const auto room = static_cast<int32_t>((buffer.size - recorder->filled) / recorder->frame_bytes);
        const int32_t taken = room < frames - done ? room : frames - done;
        convert_in(recorder, in + int64_t{done} * recorder->channels, buffer.data + recorder->filled, taken);
        recorder->filled += static_cast<SLuint32>(taken) * recorder->frame_bytes;
        recorder->recorded += taken;
        done += taken;
        if (buffer.size - recorder->filled < recorder->frame_bytes) {  // full; a part of a frame is left out
            recorder->front = (recorder->front + 1) % recorder->capacity;
            --recorder->count;
            ++recorder->index;
            recorder->filled = 0;
            ++completed;
        }
    }
    const SLmillisecond now = static_cast<SLmillisecond>(recorder->recorded * 1'000 / recorder->rate);
    int positions = 0;
    if (recorder->record_state == SL_RECORDSTATE_RECORDING) {
        for (; recorder->period > 0 && now >= recorder->next_position; recorder->next_position += recorder->period) {
            ++positions;
        }
    }
    const bool at_marker = recorder->has_marker && recorder->marker > 0 && !recorder->marker_passed &&
                           now >= recorder->marker && done > 0;
    if (at_marker) recorder->marker_passed = true;
    const slAndroidSimpleBufferQueueCallback queue_callback = recorder->queue_callback;
    void* queue_context = recorder->queue_context;
    const slRecordCallback record_callback = recorder->record_callback;
    void* record_context = recorder->record_context;
    const SLuint32 mask = recorder->event_mask;
    pthread_mutex_unlock(&recorder->lock);
    for (; queue_callback != nullptr && completed > 0 && !recorder_destroyed(recorder); --completed) {
        queue_callback(handle_of<SLAndroidSimpleBufferQueueItf>(&recorder->queue_itf), queue_context);
    }
    const auto record = handle_of<SLRecordItf>(&recorder->record_itf);
    if (record_callback != nullptr) {
        if ((mask & SL_RECORDEVENT_HEADATMARKER) != 0 && at_marker && !recorder_destroyed(recorder)) {
            record_callback(record, record_context, SL_RECORDEVENT_HEADATMARKER);
        }
        for (; (mask & SL_RECORDEVENT_HEADATNEWPOS) != 0 && positions > 0 && !recorder_destroyed(recorder);
             --positions) {
            record_callback(record, record_context, SL_RECORDEVENT_HEADATNEWPOS);
        }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void on_recorder_error(AAudioStream*, void*, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED && first_time(kDisconnected)) {
        SL_LOG(WARN, "a recorder's stream is disconnected: the recorder records nothing");
    }
}

/// Opens the recorder's stream at Realize, with the lock held. Without the microphone grant it fails as Android's does.
SLresult open_recorder(Recorder* recorder) {
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return SL_RESULT_MEMORY_FAILURE;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, recorder->channels);
    AAudioStreamBuilder_setSampleRate(builder, recorder->rate);
    AAudioStreamBuilder_setInputPreset(builder, static_cast<aaudio_input_preset_t>(recorder->preset));
    AAudioStreamBuilder_setDataCallback(builder, capture, recorder);
    AAudioStreamBuilder_setErrorCallback(builder, on_recorder_error, recorder);
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &recorder->stream);
    AAudioStreamBuilder_delete(builder);
    if (opened == AAUDIO_OK) return SL_RESULT_SUCCESS;
    SL_LOG(ERROR, "Realize: the recorder has no stream (%s)", AAudio_convertResultToText(opened));
    recorder->stream = nullptr;
    return SL_RESULT_CONTENT_UNSUPPORTED;
}

void close_recorder(Recorder* recorder) {
    pthread_mutex_lock(&recorder->lock);
    __atomic_store_n(&recorder->destroyed, true, __ATOMIC_RELEASE);
    AAudioStream* stream = recorder->stream;
    pthread_mutex_unlock(&recorder->lock);
    if (stream != nullptr) AAudioStream_close(stream);  // ends its thread, after a callback under way
}

// MARK: - SLObjectItf

SLresult realize(SLObjectItf self, SLboolean async) {
    Object* object = object_of(self);
    pthread_mutex_lock(&object->lock);
    if (object->state != SL_OBJECT_STATE_UNREALIZED) {
        pthread_mutex_unlock(&object->lock);
        return SL_RESULT_PRECONDITIONS_VIOLATED;
    }
    const SLresult result = object->kind == Kind::player     ? open_stream(static_cast<Player*>(object))
                            : object->kind == Kind::recorder ? open_recorder(static_cast<Recorder*>(object))
                                                             : SL_RESULT_SUCCESS;
    if (result == SL_RESULT_SUCCESS) object->state = SL_OBJECT_STATE_REALIZED;
    const SLuint32 state = object->state;
    const slObjectCallback callback = object->callback;
    void* context = object->callback_context;
    pthread_mutex_unlock(&object->lock);
    if (!async) return result;
    // Realized at once all the same, and the callback told as Android's thread pool tells it [guess: Android 15's
    // asynchronous Realize never ended in a plain program].
    if (callback != nullptr) callback(self, context, SL_OBJECT_EVENT_ASYNC_TERMINATION, result, state, nullptr);
    return SL_RESULT_SUCCESS;
}

SLresult resume(SLObjectItf, SLboolean) {
    return SL_RESULT_PRECONDITIONS_VIOLATED;  // no object is ever suspended
}

SLresult get_state(SLObjectItf self, SLuint32* state) {
    if (state == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Object* object = object_of(self);
    const Hold hold(&object->lock);
    *state = object->state;
    return SL_RESULT_SUCCESS;
}

/// As Android 15 answers: PARAMETER_INVALID without an ID; FEATURE_UNSUPPORTED for an interface the object does not
/// offer; PRECONDITIONS_VIOLATED before Realize, but for OBJECT and ANDROIDCONFIGURATION; FEATURE_UNSUPPORTED for one
/// that was not asked for. The interface is NULL unless it is given.
SLresult get_interface(SLObjectItf self, const SLInterfaceID iid, void* pInterface) {
    if (pInterface == nullptr) return SL_RESULT_PARAMETER_INVALID;
    void* found = nullptr;
    SLresult result = SL_RESULT_PARAMETER_INVALID;
    if (iid != nullptr) {
        Object* object = object_of(self);
        const int index = interface_of(iid);
        const Offer offered = index < 0 ? Offer::none : offer(object->kind, index);
        if (offered == Offer::none) {
            result = SL_RESULT_FEATURE_UNSUPPORTED;
        } else {
            const Hold hold(&object->lock);
            if (object->state != SL_OBJECT_STATE_REALIZED && index != k_OBJECT && offered != Offer::asked_early) {
                result = SL_RESULT_PRECONDITIONS_VIOLATED;
            } else if (offered != Offer::always && !asked_for(object, index)) {
                result = SL_RESULT_FEATURE_UNSUPPORTED;
            } else {
                found = interface_at(object, index);
                result = SL_RESULT_SUCCESS;
            }
        }
    }
    *static_cast<void**>(pInterface) = found;
    return result;
}

SLresult register_object_callback(SLObjectItf self, slObjectCallback callback, void* context) {
    Object* object = object_of(self);
    const Hold hold(&object->lock);
    object->callback = callback;
    object->callback_context = context;
    return SL_RESULT_SUCCESS;
}

void abort_async_operation(SLObjectItf) {}  // Realize never waits

/// As Android 15's: an output mix that players play into, or an engine whose objects live, stays, with a log line.
void destroy(SLObjectItf self) {
    Object* object = object_of(self);
    const uint32_t dependants = __atomic_load_n(&object->dependants, __ATOMIC_ACQUIRE);
    if (dependants != 0) {
        SL_LOG(ERROR, "Destroy of %s ignored: %u %s", object->kind == Kind::engine ? "an engine" : "an output mix",
               dependants, object->kind == Kind::engine ? "objects live" : "players play into it");
        return;
    }
    if (object->kind == Kind::recorder) {
        auto* recorder = static_cast<Recorder*>(object);
        close_recorder(recorder);
        __atomic_sub_fetch(&recorder->maker->dependants, 1u, __ATOMIC_ACQ_REL);
        free(recorder->buffers);
    } else if (object->kind == Kind::player) {
        auto* player = static_cast<Player*>(object);
        const bool closed = close_stream(player);
        __atomic_sub_fetch(&player->mix->dependants, 1u, __ATOMIC_ACQ_REL);
        __atomic_sub_fetch(&player->maker->dependants, 1u, __ATOMIC_ACQ_REL);
        if (!closed) return;  // left as it is
        free(player->buffers);
    } else if (object->maker != nullptr) {
        __atomic_sub_fetch(&object->maker->dependants, 1u, __ATOMIC_ACQ_REL);
    }
    pthread_mutex_destroy(&object->lock);
    free(object);
}

SLresult set_priority(SLObjectItf, SLint32, SLboolean) {
    return SL_RESULT_FEATURE_UNSUPPORTED;  // as Android's
}

SLresult get_priority(SLObjectItf, SLint32*, SLboolean*) {
    return SL_RESULT_FEATURE_UNSUPPORTED;  // as Android's
}

SLresult set_loss_of_control_interfaces(SLObjectItf, SLint16, SLInterfaceID*, SLboolean) {
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

const SLObjectItf_ kObjectVtable = {realize,      resume,       get_state,   get_interface,
                                    register_object_callback, abort_async_operation, destroy,
                                    set_priority, get_priority, set_loss_of_control_interfaces};

// MARK: - SLPlayItf

SLresult set_play_state(SLPlayItf self, SLuint32 state) {
    if (state != SL_PLAYSTATE_STOPPED && state != SL_PLAYSTATE_PAUSED && state != SL_PLAYSTATE_PLAYING) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    if (state == player->play_state) return SL_RESULT_SUCCESS;
    player->idle = false;
    if (state == SL_PLAYSTATE_PLAYING) {
        AAudioStream_requestStart(player->stream);
    } else if (state == SL_PLAYSTATE_PAUSED) {
        AAudioStream_requestPause(player->stream);
    } else {
        // What a playing stream holds still plays, as AudioTrack's stop lets it, and a paused one drops it, as
        // AudioFlinger resets a stopped track that was not active [from memory of AOSP]. The queue keeps its buffers.
        const bool paused = player->play_state == SL_PLAYSTATE_PAUSED;
        stop_stream(player);
        if (paused) flush_stream(player);
        restart_position(player, AAudioStream_getFramesWritten(player->stream));
    }
    player->play_state = state;
    return SL_RESULT_SUCCESS;
}

SLresult get_play_state(SLPlayItf self, SLuint32* state) {
    if (state == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *state = player->play_state;
    return SL_RESULT_SUCCESS;
}

SLresult get_duration(SLPlayItf, SLmillisecond* duration) {
    if (duration == nullptr) return SL_RESULT_PARAMETER_INVALID;
    *duration = SL_TIME_UNKNOWN;  // a buffer queue has no end
    return SL_RESULT_SUCCESS;
}

SLresult get_position(SLPlayItf self, SLmillisecond* position) {
    if (position == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *position = heard(player, AAudioStream_getFramesRead(player->stream));
    return SL_RESULT_SUCCESS;
}

SLresult register_play_callback(SLPlayItf self, slPlayCallback callback, void* context) {
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    player->play_callback = callback;
    player->play_context = context;
    return SL_RESULT_SUCCESS;
}

SLresult set_callback_events_mask(SLPlayItf self, SLuint32 mask) {
    if ((mask & ~SLuint32{0x1f}) != 0) return SL_RESULT_PARAMETER_INVALID;  // [guess: Android's check]
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    if (mask != player->event_mask) {
        player->event_mask = mask;
        set_events_again(player);
    }
    return SL_RESULT_SUCCESS;
}

SLresult get_callback_events_mask(SLPlayItf self, SLuint32* mask) {
    if (mask == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *mask = player->event_mask;
    return SL_RESULT_SUCCESS;
}

SLresult set_marker_position(SLPlayItf self, SLmillisecond marker) {
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    if (!player->has_marker || marker != player->marker) {  // the same marker again changes nothing, as Android 15's
        player->marker = marker;
        player->has_marker = true;
        set_events_again(player);
    }
    return SL_RESULT_SUCCESS;
}

SLresult clear_marker_position(SLPlayItf self) {
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    if (player->has_marker) {
        player->has_marker = false;
        set_events_again(player);
    }
    return SL_RESULT_SUCCESS;
}

SLresult get_marker_position(SLPlayItf self, SLmillisecond* marker) {
    if (marker == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *marker = player->has_marker ? player->marker : SL_TIME_UNKNOWN;
    return player->has_marker ? SL_RESULT_SUCCESS : SL_RESULT_PRECONDITIONS_VIOLATED;
}

SLresult set_position_update_period(SLPlayItf self, SLmillisecond period) {
    if (period == 0) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    if (period != player->period) {
        player->period = period;
        set_events_again(player);
    }
    return SL_RESULT_SUCCESS;
}

SLresult get_position_update_period(SLPlayItf self, SLmillisecond* period) {
    if (period == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *period = player->period;
    return SL_RESULT_SUCCESS;
}

const SLPlayItf_ kPlayVtable = {set_play_state,           get_play_state,           get_duration,
                                get_position,             register_play_callback,   set_callback_events_mask,
                                get_callback_events_mask, set_marker_position,      clear_marker_position,
                                get_marker_position,      set_position_update_period, get_position_update_period};

// MARK: - The buffer queue, SLBufferQueueItf and SLAndroidSimpleBufferQueueItf alike

SLresult enqueue(SLAndroidSimpleBufferQueueItf self, const void* buffer, SLuint32 size) {
    if (buffer == nullptr || size == 0) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    if (player->count == player->capacity) return SL_RESULT_BUFFER_INSUFFICIENT;
    player->buffers[(player->front + player->count) % player->capacity] = {static_cast<const uint8_t*>(buffer), size};
    ++player->count;
    if (player->idle) {  // as wilhelm's queue restarts its AudioTrack
        player->idle = false;
        AAudioStream_requestStart(player->stream);
    }
    return SL_RESULT_SUCCESS;
}

/// Empties the queue without callbacks, and drops the frames the stream holds unless it plays, as Android's Clear
/// flushes its AudioTrack; the position starts again. As Android 15's, that flush also ends the marker and the new
/// positions until the mask, the marker or the period changes: AudioTrack's flush clears its copies of them.
SLresult clear(SLAndroidSimpleBufferQueueItf self) {
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    player->front = player->count = player->index = player->consumed = 0;
    player->callback_pending = false;
    if (player->play_state != SL_PLAYSTATE_PLAYING || player->idle) {  // a stopped AudioTrack flushes
        flush_stream(player);
        player->track_events = false;
    }
    restart_position(player, AAudioStream_getFramesWritten(player->stream));
    return SL_RESULT_SUCCESS;
}

SLresult get_queue_state(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState* state) {
    if (state == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *state = {player->count, player->index};
    return SL_RESULT_SUCCESS;
}

SLresult register_queue_callback(SLAndroidSimpleBufferQueueItf self, slAndroidSimpleBufferQueueCallback callback,
                                 void* context) {
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    if (player->play_state != SL_PLAYSTATE_STOPPED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    player->queue_callback = callback;
    player->queue_context = context;
    return SL_RESULT_SUCCESS;
}

const SLAndroidSimpleBufferQueueItf_ kQueueVtable = {enqueue, clear, get_queue_state, register_queue_callback};

// MARK: - SLRecordItf, and the recorder's queue and configuration

bool is_key(const SLchar* key, const SLchar* name);
SLresult acquire_java_proxy(SLAndroidConfigurationItf, SLuint32, jobject*);
SLresult release_java_proxy(SLAndroidConfigurationItf, SLuint32);

Recorder* recorder_of(const void* handle) {
    return static_cast<Recorder*>(object_of(handle));
}

SLresult set_record_state(SLRecordItf self, SLuint32 state) {
    if (state != SL_RECORDSTATE_STOPPED && state != SL_RECORDSTATE_PAUSED && state != SL_RECORDSTATE_RECORDING) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    if (state == recorder->record_state) return SL_RESULT_SUCCESS;
    if (state == SL_RECORDSTATE_RECORDING) {
        AAudioStream_requestStart(recorder->stream);
    } else if (state == SL_RECORDSTATE_PAUSED) {
        AAudioStream_requestPause(recorder->stream);
    } else {
        AAudioStream_requestStop(recorder->stream);
        recorder->recorded = 0;
        recorder->next_position = recorder->period;
        recorder->marker_passed = false;
    }
    recorder->record_state = state;
    return SL_RESULT_SUCCESS;
}

SLresult get_record_state(SLRecordItf self, SLuint32* state) {
    if (state == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    *state = recorder->record_state;
    return SL_RESULT_SUCCESS;
}

SLresult set_duration_limit(SLRecordItf, SLmillisecond) {
    return unsupported(kRecorder, "SetDurationLimit");
}

SLresult get_record_position(SLRecordItf self, SLmillisecond* position) {
    if (position == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    *position = static_cast<SLmillisecond>(recorder->recorded * 1'000 / recorder->rate);
    return SL_RESULT_SUCCESS;
}

SLresult register_record_callback(SLRecordItf self, slRecordCallback callback, void* context) {
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    recorder->record_callback = callback;
    recorder->record_context = context;
    return SL_RESULT_SUCCESS;
}

SLresult set_record_events_mask(SLRecordItf self, SLuint32 mask) {
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    recorder->event_mask = mask;
    return SL_RESULT_SUCCESS;
}

SLresult get_record_events_mask(SLRecordItf self, SLuint32* mask) {
    if (mask == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    *mask = recorder->event_mask;
    return SL_RESULT_SUCCESS;
}

SLresult set_record_marker(SLRecordItf self, SLmillisecond marker) {
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    recorder->marker = marker;
    recorder->has_marker = true;
    recorder->marker_passed = false;
    return SL_RESULT_SUCCESS;
}

SLresult clear_record_marker(SLRecordItf self) {
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    recorder->has_marker = false;
    return SL_RESULT_SUCCESS;
}

SLresult get_record_marker(SLRecordItf self, SLmillisecond* marker) {
    if (marker == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    if (!recorder->has_marker) return SL_RESULT_PRECONDITIONS_VIOLATED;
    *marker = recorder->marker;
    return SL_RESULT_SUCCESS;
}

SLresult set_record_period(SLRecordItf self, SLmillisecond period) {
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    recorder->period = period;
    recorder->next_position = period;
    return SL_RESULT_SUCCESS;
}

SLresult get_record_period(SLRecordItf self, SLmillisecond* period) {
    if (period == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    *period = recorder->period;
    return SL_RESULT_SUCCESS;
}

const SLRecordItf_ kRecordVtable = {set_record_state,     get_record_state,        set_duration_limit,
                                    get_record_position,  register_record_callback, set_record_events_mask,
                                    get_record_events_mask, set_record_marker,     clear_record_marker,
                                    get_record_marker,    set_record_period,       get_record_period};

SLresult record_enqueue(SLAndroidSimpleBufferQueueItf self, const void* buffer, SLuint32 size) {
    if (buffer == nullptr || size == 0) return SL_RESULT_PARAMETER_INVALID;
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    if (recorder->count == recorder->capacity) return SL_RESULT_BUFFER_INSUFFICIENT;
    recorder->buffers[(recorder->front + recorder->count) % recorder->capacity] = {
        const_cast<uint8_t*>(static_cast<const uint8_t*>(buffer)), size};  // the app's buffer, which the recorder fills
    ++recorder->count;
    return SL_RESULT_SUCCESS;
}

SLresult record_clear(SLAndroidSimpleBufferQueueItf self) {
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    recorder->front = recorder->count = recorder->index = recorder->filled = 0;
    return SL_RESULT_SUCCESS;
}

SLresult record_queue_state(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState* state) {
    if (state == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    *state = {recorder->count, recorder->index};
    return SL_RESULT_SUCCESS;
}

SLresult register_record_queue_callback(SLAndroidSimpleBufferQueueItf self,
                                        slAndroidSimpleBufferQueueCallback callback, void* context) {
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    if (recorder->record_state != SL_RECORDSTATE_STOPPED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    recorder->queue_callback = callback;
    recorder->queue_context = context;
    return SL_RESULT_SUCCESS;
}

const SLAndroidSimpleBufferQueueItf_ kRecordQueueVtable = {record_enqueue, record_clear, record_queue_state,
                                                           register_record_queue_callback};

SLresult set_record_configuration(SLAndroidConfigurationItf self, const SLchar* key, const void* value, SLuint32 size) {
    if (key == nullptr || value == nullptr || size == 0) return SL_RESULT_PARAMETER_INVALID;
    if (!is_key(key, SL_ANDROID_KEY_RECORDING_PRESET)) return SL_RESULT_PARAMETER_INVALID;
    if (size < sizeof(SLuint32)) return SL_RESULT_BUFFER_INSUFFICIENT;
    SLuint32 given;
    memcpy(&given, value, sizeof(given));
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    if (recorder->state != SL_OBJECT_STATE_UNREALIZED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    if (given > SL_ANDROID_RECORDING_PRESET_UNPROCESSED) return SL_RESULT_PARAMETER_INVALID;
    recorder->preset = given;
    return SL_RESULT_SUCCESS;
}

SLresult get_record_configuration(SLAndroidConfigurationItf self, const SLchar* key, SLuint32* size, void* value) {
    if (key == nullptr || size == nullptr) return SL_RESULT_PARAMETER_INVALID;
    if (!is_key(key, SL_ANDROID_KEY_RECORDING_PRESET)) return SL_RESULT_PARAMETER_INVALID;
    Recorder* recorder = recorder_of(self);
    const Hold hold(&recorder->lock);
    SLresult result = SL_RESULT_SUCCESS;
    if (value != nullptr && *size < sizeof(SLuint32)) {
        result = SL_RESULT_BUFFER_INSUFFICIENT;
    } else if (value != nullptr) {
        memcpy(value, &recorder->preset, sizeof(SLuint32));
    }
    *size = sizeof(SLuint32);
    return result;
}

const SLAndroidConfigurationItf_ kRecordConfigurationVtable = {set_record_configuration, get_record_configuration,
                                                               acquire_java_proxy, release_java_proxy};

// MARK: - SLVolumeItf

SLresult set_volume_level(SLVolumeItf self, SLmillibel level) {
    if (level > 0) return SL_RESULT_PARAMETER_INVALID;  // the most is 0
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    player->level = level;
    update_gains(player);
    return SL_RESULT_SUCCESS;
}

SLresult get_volume_level(SLVolumeItf self, SLmillibel* level) {
    if (level == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *level = player->level;
    return SL_RESULT_SUCCESS;
}

SLresult get_max_volume_level(SLVolumeItf, SLmillibel* level) {
    if (level == nullptr) return SL_RESULT_PARAMETER_INVALID;
    *level = 0;
    return SL_RESULT_SUCCESS;
}

SLresult set_mute(SLVolumeItf self, SLboolean mute) {
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    player->mute = mute != SL_BOOLEAN_FALSE;
    update_gains(player);
    return SL_RESULT_SUCCESS;
}

SLresult get_mute(SLVolumeItf self, SLboolean* mute) {
    if (mute == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *mute = player->mute ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

SLresult enable_stereo_position(SLVolumeItf self, SLboolean enable) {
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    player->stereo_enabled = enable != SL_BOOLEAN_FALSE;
    update_gains(player);
    return SL_RESULT_SUCCESS;
}

SLresult is_enabled_stereo_position(SLVolumeItf self, SLboolean* enabled) {
    if (enabled == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *enabled = player->stereo_enabled ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

SLresult set_stereo_position(SLVolumeItf self, SLpermille position) {
    if (position < -1'000 || position > 1'000) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    player->stereo_position = position;
    update_gains(player);
    return SL_RESULT_SUCCESS;
}

SLresult get_stereo_position(SLVolumeItf self, SLpermille* position) {
    if (position == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *position = player->stereo_position;
    return SL_RESULT_SUCCESS;
}

const SLVolumeItf_ kVolumeVtable = {set_volume_level,       get_volume_level,           get_max_volume_level,
                                    set_mute,               get_mute,                   enable_stereo_position,
                                    is_enabled_stereo_position, set_stereo_position,     get_stereo_position};

// MARK: - SLEffectSendItf: an output mix here has no effects, so every effect is one that is not of the output mix,
// and Android answers PARAMETER_INVALID for such an effect.

SLresult enable_effect_send(SLEffectSendItf, const void*, SLboolean, SLmillibel) {
    return SL_RESULT_PARAMETER_INVALID;
}

SLresult is_effect_enabled(SLEffectSendItf, const void*, SLboolean* enabled) {
    if (enabled == nullptr) return SL_RESULT_PARAMETER_INVALID;
    *enabled = SL_BOOLEAN_FALSE;
    return SL_RESULT_PARAMETER_INVALID;
}

SLresult set_direct_level(SLEffectSendItf self, SLmillibel level) {
    if (level > 0) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    player->direct_level = level;
    return SL_RESULT_SUCCESS;
}

SLresult get_direct_level(SLEffectSendItf self, SLmillibel* level) {
    if (level == nullptr) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    *level = player->direct_level;
    return SL_RESULT_SUCCESS;
}

SLresult set_send_level(SLEffectSendItf, const void*, SLmillibel) {
    return SL_RESULT_PARAMETER_INVALID;
}

SLresult get_send_level(SLEffectSendItf, const void*, SLmillibel*) {
    return SL_RESULT_PARAMETER_INVALID;
}

const SLEffectSendItf_ kEffectSendVtable = {enable_effect_send, is_effect_enabled, set_direct_level,
                                            get_direct_level,   set_send_level,    get_send_level};

// MARK: - SLAndroidConfigurationItf, as Android 15 answers

bool is_key(const SLchar* key, const SLchar* name) {
    return strcmp(reinterpret_cast<const char*>(key), reinterpret_cast<const char*>(name)) == 0;
}

SLresult set_configuration(SLAndroidConfigurationItf self, const SLchar* key, const void* value, SLuint32 size) {
    if (key == nullptr || value == nullptr || size == 0) return SL_RESULT_PARAMETER_INVALID;
    const bool type = is_key(key, SL_ANDROID_KEY_STREAM_TYPE);
    if (!type && !is_key(key, SL_ANDROID_KEY_PERFORMANCE_MODE)) return SL_RESULT_PARAMETER_INVALID;
    if (size < sizeof(SLuint32)) return SL_RESULT_BUFFER_INSUFFICIENT;
    SLuint32 given;
    memcpy(&given, value, sizeof(given));
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    if (player->state != SL_OBJECT_STATE_UNREALIZED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    if (given > (type ? SLuint32{SL_ANDROID_STREAM_NOTIFICATION} : SL_ANDROID_PERFORMANCE_POWER_SAVING)) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    if (type) {
        player->stream_type = static_cast<SLint32>(given);
    } else {
        player->performance_mode = given;
    }
    return SL_RESULT_SUCCESS;
}

/// The value's size is always written back; without a place for the value, only the size is asked for.
SLresult get_configuration(SLAndroidConfigurationItf self, const SLchar* key, SLuint32* size, void* value) {
    if (key == nullptr || size == nullptr) return SL_RESULT_PARAMETER_INVALID;
    const bool type = is_key(key, SL_ANDROID_KEY_STREAM_TYPE);
    if (!type && !is_key(key, SL_ANDROID_KEY_PERFORMANCE_MODE)) return SL_RESULT_PARAMETER_INVALID;
    Player* player = player_of(self);
    const Hold hold(&player->lock);
    SLresult result = SL_RESULT_SUCCESS;
    if (value != nullptr && *size < sizeof(SLuint32)) {
        result = SL_RESULT_BUFFER_INSUFFICIENT;
    } else if (value != nullptr) {
        const SLuint32 current = type ? static_cast<SLuint32>(player->stream_type) : player->performance_mode;
        memcpy(value, &current, sizeof(current));
    }
    *size = sizeof(SLuint32);
    return result;
}

SLresult acquire_java_proxy(SLAndroidConfigurationItf, SLuint32, jobject*) {
    return unsupported(kJavaProxy, "AcquireJavaProxy");  // no Java in M3
}

SLresult release_java_proxy(SLAndroidConfigurationItf, SLuint32) {
    return unsupported(kJavaProxy, "ReleaseJavaProxy");
}

const SLAndroidConfigurationItf_ kConfigurationVtable = {set_configuration, get_configuration, acquire_java_proxy,
                                                         release_java_proxy};

// MARK: - Objects' creation

void init_object(Object* object, Kind kind, uint64_t asked) {
    object->self = {&kObjectVtable, object};
    object->kind = kind;
    pthread_mutex_init(&object->lock, nullptr);
    object->state = SL_OBJECT_STATE_UNREALIZED;
    object->asked = asked;
}

SLObjectItf handle_of(Object* object) {
    return handle_of<SLObjectItf>(&object->self);
}

/// What a player plays, as its data source and format give it.
struct Source {
    Sample sample;
    SLuint32 channels, rate, frame_bytes, buffers;
};

/// Android 15's checks of a PCM format, in an order that gives its answers.
SLresult check_format(const void* format, Source* source) {
    if (format == nullptr) return SL_RESULT_CONTENT_UNSUPPORTED;
    SLAndroidDataFormat_PCM_EX pcm;
    const SLuint32 type = *static_cast<const SLuint32*>(format);
    if (type == SL_DATAFORMAT_PCM) {
        memcpy(&pcm, format, sizeof(SLDataFormat_PCM));
        pcm.representation = pcm.bitsPerSample == 8 ? SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT
                                                     : SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT;
    } else if (type == SL_ANDROID_DATAFORMAT_PCM_EX) {
        memcpy(&pcm, format, sizeof(pcm));
        const SLuint32 bits = pcm.bitsPerSample;
        const bool fits = pcm.representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT          ? bits == 32
                          : pcm.representation == SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT   ? bits == 16 || bits == 24 ||
                                                                                                 bits == 32
                          : pcm.representation == SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT ? bits == 8
                                                                                             : false;
        if (!fits) return SL_RESULT_PARAMETER_INVALID;
    } else {
        return SL_RESULT_PARAMETER_INVALID;
    }
    if (pcm.numChannels == 0) return SL_RESULT_PARAMETER_INVALID;
    if (pcm.numChannels > 8) return SL_RESULT_CONTENT_UNSUPPORTED;  // Android's FCC_8, and the runtime's most
    if (pcm.sampleRate == 0) return SL_RESULT_PARAMETER_INVALID;
    if (pcm.sampleRate < SL_SAMPLINGRATE_8 || pcm.sampleRate > SL_SAMPLINGRATE_192) {
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    const SLuint32 bits = pcm.bitsPerSample;
    if (bits == 0 || pcm.containerSize < bits) return SL_RESULT_PARAMETER_INVALID;
    if ((bits != 8 && bits != 16 && bits != 24 && bits != 32) || pcm.containerSize != bits) {
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    if (pcm.endianness == SL_BYTEORDER_BIGENDIAN) return SL_RESULT_CONTENT_UNSUPPORTED;
    if (pcm.endianness != SL_BYTEORDER_LITTLEENDIAN) return SL_RESULT_PARAMETER_INVALID;
    const SLuint32 positions = pcm.channelMask & ~SL_ANDROID_SPEAKER_NON_POSITIONAL;  // positional or indexed
    if (pcm.channelMask != 0 && static_cast<SLuint32>(__builtin_popcount(positions)) != pcm.numChannels) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    source->sample = pcm.representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT ? Sample::f32
                     : bits == 8                                             ? Sample::u8
                     : bits == 16                                            ? Sample::s16
                     : bits == 24                                            ? Sample::s24
                                                                             : Sample::s32;
    source->channels = pcm.numChannels;
    source->rate = pcm.sampleRate / 1'000;
    source->frame_bytes = pcm.numChannels * bits / 8;
    return SL_RESULT_SUCCESS;
}

SLresult check_source(const SLDataSource* data, Source* source) {
    if (data == nullptr || data->pLocator == nullptr) return SL_RESULT_PARAMETER_INVALID;
    const SLuint32 locator = *static_cast<const SLuint32*>(data->pLocator);
    if (locator != SL_DATALOCATOR_BUFFERQUEUE && locator != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) {
        return unsupported(kDataLocator, "A player from another data locator than a buffer queue");
    }
    source->buffers = static_cast<const SLDataLocator_BufferQueue*>(data->pLocator)->numBuffers;
    if (source->buffers == 0 || source->buffers > 255) return SL_RESULT_PARAMETER_INVALID;
    return check_format(data->pFormat, source);
}

/// The output mix of the sink, which must be realized, as Android 15's says: "the output mix is not realized".
SLresult check_sink(const SLDataSink* sink, Object** mix) {
    if (sink == nullptr || sink->pLocator == nullptr) return SL_RESULT_PARAMETER_INVALID;
    if (*static_cast<const SLuint32*>(sink->pLocator) != SL_DATALOCATOR_OUTPUTMIX) return SL_RESULT_CONTENT_UNSUPPORTED;
    const SLObjectItf handle = static_cast<const SLDataLocator_OutputMix*>(sink->pLocator)->outputMix;
    if (handle == nullptr || object_of(handle)->kind != Kind::output_mix) return SL_RESULT_PARAMETER_INVALID;
    *mix = object_of(handle);
    const Hold hold(&(*mix)->lock);
    return (*mix)->state == SL_OBJECT_STATE_REALIZED ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID;
}

// MARK: - SLEngineItf

SLresult create_audio_player(SLEngineItf self, SLObjectItf* pPlayer, SLDataSource* data, SLDataSink* sink,
                             SLuint32 count, const SLInterfaceID* ids, const SLboolean* required) {
    if (pPlayer == nullptr) return SL_RESULT_PARAMETER_INVALID;
    *pPlayer = nullptr;
    uint64_t asked = 0;
    Source source{};
    Object* mix = nullptr;
    SLresult result = take_interfaces(Kind::player, count, ids, required, &asked);
    if (result == SL_RESULT_SUCCESS) result = check_source(data, &source);
    if (result == SL_RESULT_SUCCESS) result = check_sink(sink, &mix);
    if (result != SL_RESULT_SUCCESS) return result;
    auto* player = static_cast<Player*>(calloc(1, sizeof(Player)));
    auto* buffers = static_cast<Buffer*>(calloc(source.buffers, sizeof(Buffer)));
    if (player == nullptr || buffers == nullptr) {
        free(player);
        free(buffers);
        return SL_RESULT_MEMORY_FAILURE;
    }
    init_object(player, Kind::player, asked);
    player->maker = object_of(self);
    player->mix = mix;
    __atomic_add_fetch(&player->maker->dependants, 1u, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&mix->dependants, 1u, __ATOMIC_ACQ_REL);
    player->play_itf = {&kPlayVtable, player};
    player->queue_itf = {&kQueueVtable, player};
    player->volume_itf = {&kVolumeVtable, player};
    player->send_itf = {&kEffectSendVtable, player};
    player->configuration_itf = {&kConfigurationVtable, player};
    player->sample = source.sample;
    player->channels = static_cast<int32_t>(source.channels);
    player->out_channels = player->channels < 2 ? 2 : player->channels;
    player->rate = static_cast<int32_t>(source.rate);
    player->frame_bytes = source.frame_bytes;
    player->stream_type = SL_ANDROID_STREAM_MEDIA;  // Android 15's defaults
    player->performance_mode = SL_ANDROID_PERFORMANCE_LATENCY;
    player->play_state = SL_PLAYSTATE_STOPPED;
    player->period = 1'000;
    player->track_events = true;
    restart_position(player, 0);
    player->buffers = buffers;
    player->capacity = source.buffers;
    update_gains(player);
    *pPlayer = handle_of(player);
    return SL_RESULT_SUCCESS;
}

SLresult create_output_mix(SLEngineItf self, SLObjectItf* pMix, SLuint32 count, const SLInterfaceID* ids,
                           const SLboolean* required) {
    if (pMix == nullptr) return SL_RESULT_PARAMETER_INVALID;
    *pMix = nullptr;
    uint64_t asked = 0;
    const SLresult result = take_interfaces(Kind::output_mix, count, ids, required, &asked);
    if (result != SL_RESULT_SUCCESS) return result;
    auto* mix = static_cast<Object*>(calloc(1, sizeof(Object)));
    if (mix == nullptr) return SL_RESULT_MEMORY_FAILURE;
    init_object(mix, Kind::output_mix, asked);
    mix->maker = object_of(self);
    __atomic_add_fetch(&mix->maker->dependants, 1u, __ATOMIC_ACQ_REL);
    *pMix = handle_of(mix);
    return SL_RESULT_SUCCESS;
}

/// What an engine does not create here.
SLresult not_created(SLObjectItf* object, int what, const char* name) {
    if (object != nullptr) *object = nullptr;
    return unsupported(what, name);
}

SLresult create_led_device(SLEngineItf, SLObjectItf* device, SLuint32, SLuint32, const SLInterfaceID*,
                           const SLboolean*) {
    return not_created(device, kLedDevice, "CreateLEDDevice");
}

SLresult create_vibra_device(SLEngineItf, SLObjectItf* device, SLuint32, SLuint32, const SLInterfaceID*,
                             const SLboolean*) {
    return not_created(device, kVibraDevice, "CreateVibraDevice");
}

SLresult create_audio_recorder(SLEngineItf self, SLObjectItf* pRecorder, SLDataSource* data, SLDataSink* sink,
                               SLuint32 count, const SLInterfaceID* ids, const SLboolean* required) {
    if (pRecorder == nullptr) return SL_RESULT_PARAMETER_INVALID;
    *pRecorder = nullptr;
    uint64_t asked = 0;
    SLresult result = take_interfaces(Kind::recorder, count, ids, required, &asked);
    if (result != SL_RESULT_SUCCESS) return result;
    // The source: the default audio input device.
    if (data == nullptr || data->pLocator == nullptr) return SL_RESULT_PARAMETER_INVALID;
    if (*static_cast<const SLuint32*>(data->pLocator) != SL_DATALOCATOR_IODEVICE) {
        return unsupported(kDataLocator, "A recorder from another data locator than an I/O device");
    }
    const auto* device = static_cast<const SLDataLocator_IODevice*>(data->pLocator);
    if (device->deviceType != SL_IODEVICE_AUDIOINPUT || device->deviceID != SL_DEFAULTDEVICEID_AUDIOINPUT) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    // The sink: a buffer queue of PCM.
    if (sink == nullptr || sink->pLocator == nullptr) return SL_RESULT_PARAMETER_INVALID;
    const SLuint32 locator = *static_cast<const SLuint32*>(sink->pLocator);
    if (locator != SL_DATALOCATOR_BUFFERQUEUE && locator != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) {
        return unsupported(kDataLocator, "A recorder to another data locator than a buffer queue");
    }
    Source format{};
    format.buffers = static_cast<const SLDataLocator_BufferQueue*>(sink->pLocator)->numBuffers;
    if (format.buffers == 0 || format.buffers > 255) return SL_RESULT_PARAMETER_INVALID;
    result = check_format(sink->pFormat, &format);
    if (result != SL_RESULT_SUCCESS) return result;
    auto* recorder = static_cast<Recorder*>(calloc(1, sizeof(Recorder)));
    auto* buffers = static_cast<Recorder::Slot*>(calloc(format.buffers, sizeof(Recorder::Slot)));
    if (recorder == nullptr || buffers == nullptr) {
        free(recorder);
        free(buffers);
        return SL_RESULT_MEMORY_FAILURE;
    }
    init_object(recorder, Kind::recorder, asked);
    recorder->maker = object_of(self);
    __atomic_add_fetch(&recorder->maker->dependants, 1u, __ATOMIC_ACQ_REL);
    recorder->record_itf = {&kRecordVtable, recorder};
    recorder->queue_itf = {&kRecordQueueVtable, recorder};
    recorder->configuration_itf = {&kRecordConfigurationVtable, recorder};
    recorder->sample = format.sample;
    recorder->channels = static_cast<int32_t>(format.channels);
    recorder->rate = static_cast<int32_t>(format.rate);
    recorder->frame_bytes = format.frame_bytes;
    recorder->preset = SL_ANDROID_RECORDING_PRESET_GENERIC;
    recorder->record_state = SL_RECORDSTATE_STOPPED;
    recorder->buffers = buffers;
    recorder->capacity = format.buffers;
    *pRecorder = handle_of(recorder);
    return SL_RESULT_SUCCESS;
}

SLresult create_midi_player(SLEngineItf, SLObjectItf* player, SLDataSource*, SLDataSource*, SLDataSink*, SLDataSink*,
                            SLDataSink*, SLuint32, const SLInterfaceID*, const SLboolean*) {
    return not_created(player, kMidiPlayer, "CreateMidiPlayer");
}

SLresult create_listener(SLEngineItf, SLObjectItf* listener, SLuint32, const SLInterfaceID*, const SLboolean*) {
    return not_created(listener, kListener, "CreateListener");
}

SLresult create_3d_group(SLEngineItf, SLObjectItf* group, SLuint32, const SLInterfaceID*, const SLboolean*) {
    return not_created(group, k3DGroup, "Create3DGroup");
}

SLresult create_metadata_extractor(SLEngineItf, SLObjectItf* extractor, SLDataSource*, SLuint32, const SLInterfaceID*,
                                   const SLboolean*) {
    return not_created(extractor, kMetadataExtractor, "CreateMetadataExtractor");
}

SLresult create_extension_object(SLEngineItf, SLObjectItf* object, void*, SLuint32, SLuint32, const SLInterfaceID*,
                                 const SLboolean*) {
    return not_created(object, kExtensionObject, "CreateExtensionObject");
}

SLresult query_num_supported_interfaces(SLEngineItf, SLuint32, SLuint32*) {
    return unsupported(kEngineQueries, "The engine's queries");
}

SLresult query_supported_interfaces(SLEngineItf, SLuint32, SLuint32, SLInterfaceID*) {
    return unsupported(kEngineQueries, "The engine's queries");
}

SLresult query_num_supported_extensions(SLEngineItf, SLuint32*) {
    return unsupported(kEngineQueries, "The engine's queries");
}

SLresult query_supported_extension(SLEngineItf, SLuint32, SLchar*, SLint16*) {
    return unsupported(kEngineQueries, "The engine's queries");
}

SLresult is_extension_supported(SLEngineItf, const SLchar*, SLboolean*) {
    return unsupported(kEngineQueries, "The engine's queries");
}

const SLEngineItf_ kEngineVtable = {create_led_device,         create_vibra_device,
                                    create_audio_player,       create_audio_recorder,
                                    create_midi_player,        create_listener,
                                    create_3d_group,           create_output_mix,
                                    create_metadata_extractor, create_extension_object,
                                    query_num_supported_interfaces, query_supported_interfaces,
                                    query_num_supported_extensions, query_supported_extension,
                                    is_extension_supported};

}  // namespace

extern "C" {

#define MINATO_EXPORT(name, ...) const SLInterfaceID SL_IID_##name = &kIids[k_##name];
MINATO_IIDS(MINATO_EXPORT)
#undef MINATO_EXPORT

/// Any number of engines, as Android 15 allows.
SLresult slCreateEngine(SLObjectItf* pEngine, SLuint32 numOptions, const SLEngineOption* pEngineOptions,
                        SLuint32 numInterfaces, const SLInterfaceID* pInterfaceIds,
                        const SLboolean* pInterfaceRequired) {
    if (pEngine == nullptr) return SL_RESULT_PARAMETER_INVALID;
    *pEngine = nullptr;
    if (numOptions > 0 && pEngineOptions == nullptr) return SL_RESULT_PARAMETER_INVALID;
    for (SLuint32 at = 0; at < numOptions; ++at) {
        const SLuint32 feature = pEngineOptions[at].feature;
        if (feature != SL_ENGINEOPTION_THREADSAFE && feature != SL_ENGINEOPTION_LOSSOFCONTROL) {
            return SL_RESULT_PARAMETER_INVALID;
        }
    }
    uint64_t asked = 0;
    const SLresult result = take_interfaces(Kind::engine, numInterfaces, pInterfaceIds, pInterfaceRequired, &asked);
    if (result != SL_RESULT_SUCCESS) return result;
    auto* engine = static_cast<Engine*>(calloc(1, sizeof(Engine)));
    if (engine == nullptr) return SL_RESULT_MEMORY_FAILURE;
    init_object(engine, Kind::engine, asked);
    engine->engine = {&kEngineVtable, engine};
    *pEngine = handle_of(engine);
    return SL_RESULT_SUCCESS;
}

/// OBJECT and ENGINE.
SLresult slQueryNumSupportedEngineInterfaces(SLuint32* pNumSupportedInterfaces) {
    if (pNumSupportedInterfaces == nullptr) return SL_RESULT_PARAMETER_INVALID;
    *pNumSupportedInterfaces = 2;
    return SL_RESULT_SUCCESS;
}

SLresult slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID* pInterfaceId) {
    if (pInterfaceId == nullptr || index > 1) return SL_RESULT_PARAMETER_INVALID;
    *pInterfaceId = index == 0 ? SL_IID_OBJECT : SL_IID_ENGINE;
    return SL_RESULT_SUCCESS;
}

}  // extern "C"
