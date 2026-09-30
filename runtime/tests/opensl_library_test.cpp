// Checks libOpenSLES (runtime/android/opensles.cpp, ADR-0036, decision 7) on the Mac, built with libaaudio and
// tests/aaudio_host as aaudio_library_test builds it: a fake host of 48 kHz with 5 ms IO buffers takes the players'
// streams, and the test reads them as the render block would. It covers what opensl-test on Android cannot show,
// with Android 15's answers where probe programs on the emulator asked for them: the checks of formats, locators
// and interfaces; the samples of each PCM format and what volume, mute and stereo position make of them; the buffer
// queue's order, callbacks, Clear and STOPPED, also against a pull under way, an empty queue's stop and a producer
// thread; the position and the play events; ANDROIDCONFIGURATION's and EFFECTSEND's answers; Destroy while a player
// plays, from its callbacks and of objects others depend on; and what Minato leaves out.
#include "check.h"
#include "host.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace audio_host;
using namespace std::chrono_literals;

constexpr SLuint32 kQueue = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;

/// How long a check waits for what the stream's thread brings about, and a read for the next frame: long, since a
/// machine under load may keep the thread waiting; a check whose state comes returns as soon as it does.
constexpr auto kWait = 5'000ms;

// MARK: - Engines, formats and players

struct Engine {
    SLObjectItf object = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf mix = nullptr;

    Engine() {
        use_fake_host();
        CHECK(slCreateEngine(&object, 0, nullptr, 0, nullptr, nullptr) == SL_RESULT_SUCCESS);
        if (object == nullptr) return;
        CHECK((*object)->Realize(object, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
        CHECK((*object)->GetInterface(object, SL_IID_ENGINE, &engine) == SL_RESULT_SUCCESS);
        if (engine == nullptr) return;
        CHECK((*engine)->CreateOutputMix(engine, &mix, 0, nullptr, nullptr) == SL_RESULT_SUCCESS);
        if (mix != nullptr) CHECK((*mix)->Realize(mix, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    }
    ~Engine() {
        if (mix != nullptr) (*mix)->Destroy(mix);
        if (object != nullptr) (*object)->Destroy(object);
    }
    bool ok() const { return mix != nullptr; }
};

SLDataFormat_PCM pcm(SLuint32 channels, SLuint32 bits, SLuint32 container, SLuint32 rate = SL_SAMPLINGRATE_48,
                     SLuint32 mask = 0, SLuint32 order = SL_BYTEORDER_LITTLEENDIAN) {
    return {SL_DATAFORMAT_PCM, channels, rate, bits, container, mask, order};
}

SLAndroidDataFormat_PCM_EX pcm_ex(SLuint32 channels, SLuint32 bits, SLuint32 representation, SLuint32 mask = 0) {
    return {SL_ANDROID_DATAFORMAT_PCM_EX, channels, SL_SAMPLINGRATE_48, bits, bits, mask, SL_BYTEORDER_LITTLEENDIAN,
            representation};
}

const SLDataFormat_PCM stereo16 = pcm(2, 16, 16);
const SLAndroidDataFormat_PCM_EX mono_float = pcm_ex(1, 32, SL_ANDROID_PCM_REPRESENTATION_FLOAT);
const SLAndroidDataFormat_PCM_EX stereo_float = pcm_ex(2, 32, SL_ANDROID_PCM_REPRESENTATION_FLOAT);

struct Spec {
    const void *format = &stereo16;
    SLuint32 locator = kQueue;
    SLuint32 buffers = 2;
    std::vector<SLInterfaceID> ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
    std::vector<SLboolean> required = {};  // every one when empty
};

SLresult create(const Engine &engine, const Spec &spec, SLObjectItf *player) {
    SLDataLocator_AndroidSimpleBufferQueue queue{spec.locator, spec.buffers};
    SLDataSource source{&queue, const_cast<void *>(spec.format)};
    SLDataLocator_OutputMix mix{SL_DATALOCATOR_OUTPUTMIX, engine.mix};
    SLDataSink sink{&mix, nullptr};
    std::vector<SLboolean> required = spec.required;
    if (required.empty()) required.assign(spec.ids.size(), SL_BOOLEAN_TRUE);
    return (*engine.engine)->CreateAudioPlayer(engine.engine, player, &source, &sink,
                                               static_cast<SLuint32>(spec.ids.size()), spec.ids.data(),
                                               required.data());
}

/// A realized player with its PLAY, buffer queue and VOLUME interfaces, and the stream the host opened for it.
struct Player {
    SLObjectItf object = nullptr;
    SLPlayItf play = nullptr;
    SLAndroidSimpleBufferQueueItf queue = nullptr;
    SLVolumeItf volume = nullptr;
    Opened stream;

    Player(const Engine &engine, const Spec &spec = {}) {
        if (!engine.ok()) return;
        CHECK(create(engine, spec, &object) == SL_RESULT_SUCCESS);
        if (object == nullptr) return;
        CHECK((*object)->Realize(object, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
        stream = last_opened();
        CHECK((*object)->GetInterface(object, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS);
        CHECK((*object)->GetInterface(object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue) == SL_RESULT_SUCCESS);
        (*object)->GetInterface(object, SL_IID_VOLUME, &volume);
    }
    ~Player() { destroy(); }
    void destroy() {
        if (object != nullptr) (*object)->Destroy(object);
        object = nullptr;
    }
    bool ok() const { return play != nullptr && queue != nullptr && stream.handle != 0; }
    SLresult set(SLuint32 state) { return (*play)->SetPlayState(play, state); }
    SLresult enqueue(const void *buffer, SLuint32 bytes) { return (*queue)->Enqueue(queue, buffer, bytes); }
    SLAndroidSimpleBufferQueueState queue_state() {
        SLAndroidSimpleBufferQueueState state{99, 99};
        (*queue)->GetState(queue, &state);
        return state;
    }
    SLmillisecond position() {
        SLmillisecond position = 12345;
        (*play)->GetPosition(play, &position);
        return position;
    }
};

/// Reads `frames` frames of the stream as the render block would, 240 at a time, until they came or none came for
/// kWait.
std::vector<float> read_frames(const Opened &stream, std::int64_t frames) {
    std::vector<float> samples;
    const auto want = static_cast<std::size_t>(frames * stream.channels);
    auto last = std::chrono::steady_clock::now();
    while (samples.size() < want && std::chrono::steady_clock::now() - last < kWait) {
        const std::int64_t left = frames - static_cast<std::int64_t>(samples.size()) / stream.channels;
        const auto got = render(stream.handle, std::min<std::int64_t>(240, left), stream.channels);
        if (got.empty()) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        samples.insert(samples.end(), got.begin(), got.end());
        last = std::chrono::steady_clock::now();
    }
    return samples;
}

/// Reads until the stream gives no more frames, for up to a second.
void drain(const Opened &stream) {
    within(1'000ms, [&] { return render(stream.handle, 240, stream.channels).empty(); });
}

/// Reads, as the render block would, all the stream plays until it stops, as a stop or an empty queue stops it: until
/// a STOPPED stream gives nothing more, or none came for kWait.
std::vector<float> read_all(const Opened &stream) {
    std::vector<float> samples;
    auto last = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - last < kWait) {
        const bool was_stopped = stopped(stream.handle);  // before the read, which then finds it played out
        const auto got = render(stream.handle, 240, stream.channels);
        if (!got.empty()) {
            samples.insert(samples.end(), got.begin(), got.end());
            last = std::chrono::steady_clock::now();
        } else if (was_stopped) {
            break;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    return samples;
}

bool near(float a, float b) {
    return std::fabs(a - b) <= 1e-6f * std::max(1.0f, std::fabs(b));
}

bool near(const std::vector<float> &a, const std::vector<float> &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (!near(a[index], b[index])) return false;
    }
    return true;
}

void expect(const char *what, SLresult result, SLresult expected) {
    if (result != expected) std::fprintf(stderr, "%s: %u, expected %u\n", what, result, expected);
    CHECK(result == expected);
}

// MARK: - IDs, engines and output mixes

TEST(the_interface_ids_are_android_15s) {
    const SLInterfaceID_ engine = {0x8d97c260, 0xddd4, 0x11db, 0x958f, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
    const SLInterfaceID_ queue = {0x198e4940, 0xc5d7, 0x11df, 0xa2a6, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
    const SLInterfaceID_ null = {0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, {0x46, 0x57, 0xe6, 0x79, 0x52, 0x10}};
    CHECK(std::memcmp(SL_IID_ENGINE, &engine, sizeof(engine)) == 0);
    CHECK(std::memcmp(SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue, sizeof(queue)) == 0);
    CHECK(std::memcmp(SL_IID_NULL, &null, sizeof(null)) == 0);
    const SLInterfaceID all[] = {
        SL_IID_3DCOMMIT, SL_IID_3DDOPPLER, SL_IID_3DGROUPING, SL_IID_3DLOCATION, SL_IID_3DMACROSCOPIC, SL_IID_3DSOURCE,
        SL_IID_AUDIODECODERCAPABILITIES, SL_IID_AUDIOENCODER, SL_IID_AUDIOENCODERCAPABILITIES,
        SL_IID_AUDIOIODEVICECAPABILITIES, SL_IID_BASSBOOST, SL_IID_BUFFERQUEUE, SL_IID_DEVICEVOLUME,
        SL_IID_DYNAMICINTERFACEMANAGEMENT, SL_IID_DYNAMICSOURCE, SL_IID_EFFECTSEND, SL_IID_ENGINE,
        SL_IID_ENGINECAPABILITIES, SL_IID_ENVIRONMENTALREVERB, SL_IID_EQUALIZER, SL_IID_LED, SL_IID_METADATAEXTRACTION,
        SL_IID_METADATATRAVERSAL, SL_IID_MIDIMESSAGE, SL_IID_MIDIMUTESOLO, SL_IID_MIDITEMPO, SL_IID_MIDITIME,
        SL_IID_MUTESOLO, SL_IID_NULL, SL_IID_OBJECT, SL_IID_OUTPUTMIX, SL_IID_PITCH, SL_IID_PLAY, SL_IID_PLAYBACKRATE,
        SL_IID_PREFETCHSTATUS, SL_IID_PRESETREVERB, SL_IID_RATEPITCH, SL_IID_RECORD, SL_IID_SEEK, SL_IID_THREADSYNC,
        SL_IID_VIBRA, SL_IID_VIRTUALIZER, SL_IID_VISUALIZATION, SL_IID_VOLUME, SL_IID_ANDROIDEFFECT,
        SL_IID_ANDROIDEFFECTCAPABILITIES, SL_IID_ANDROIDEFFECTSEND, SL_IID_ANDROIDCONFIGURATION,
        SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDBUFFERQUEUESOURCE, SL_IID_ANDROIDACOUSTICECHOCANCELLATION,
        SL_IID_ANDROIDAUTOMATICGAINCONTROL, SL_IID_ANDROIDNOISESUPPRESSION};
    std::set<std::vector<std::uint8_t>> values;
    for (const SLInterfaceID id : all) {
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(id);
        values.insert(std::vector<std::uint8_t>(bytes, bytes + sizeof(SLInterfaceID_)));
    }
    CHECK(values.size() == 53);
}

TEST(the_engine_is_an_object_with_the_engine_interface) {
    use_fake_host();
    CHECK(slCreateEngine(nullptr, 0, nullptr, 0, nullptr, nullptr) == SL_RESULT_PARAMETER_INVALID);
    SLObjectItf object = nullptr;
    const SLEngineOption safe[] = {{SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE}};
    CHECK(slCreateEngine(&object, 1, safe, 0, nullptr, nullptr) == SL_RESULT_SUCCESS);
    if (object == nullptr) return;
    SLObjectItf second = nullptr;
    CHECK(slCreateEngine(&second, 0, nullptr, 0, nullptr, nullptr) == SL_RESULT_SUCCESS);  // as Android 15
    if (second != nullptr) (*second)->Destroy(second);
    SLuint32 state = 0;
    CHECK((*object)->GetState(object, &state) == SL_RESULT_SUCCESS && state == SL_OBJECT_STATE_UNREALIZED);
    SLEngineItf engine = nullptr;
    CHECK((*object)->GetInterface(object, SL_IID_ENGINE, &engine) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK((*object)->Realize(object, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    CHECK((*object)->Realize(object, SL_BOOLEAN_FALSE) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK((*object)->Resume(object, SL_BOOLEAN_FALSE) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK((*object)->GetState(object, &state) == SL_RESULT_SUCCESS && state == SL_OBJECT_STATE_REALIZED);
    CHECK((*object)->GetInterface(object, SL_IID_ENGINE, &engine) == SL_RESULT_SUCCESS && engine != nullptr);
    void *other = nullptr;
    CHECK((*object)->GetInterface(object, SL_IID_THREADSYNC, &other) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK((*object)->SetPriority(object, 5, SL_BOOLEAN_FALSE) == SL_RESULT_FEATURE_UNSUPPORTED);
    SLint32 priority = 0;
    SLboolean preemptable = SL_BOOLEAN_FALSE;
    CHECK((*object)->GetPriority(object, &priority, &preemptable) == SL_RESULT_FEATURE_UNSUPPORTED);
    SLuint32 count = 0;
    CHECK(slQueryNumSupportedEngineInterfaces(&count) == SL_RESULT_SUCCESS && count == 2);
    SLInterfaceID id = nullptr;
    CHECK(slQuerySupportedEngineInterfaces(0, &id) == SL_RESULT_SUCCESS && id == SL_IID_OBJECT);
    CHECK(slQuerySupportedEngineInterfaces(1, &id) == SL_RESULT_SUCCESS && id == SL_IID_ENGINE);
    CHECK(slQuerySupportedEngineInterfaces(2, &id) == SL_RESULT_PARAMETER_INVALID);
    CHECK(slQueryNumSupportedEngineInterfaces(nullptr) == SL_RESULT_PARAMETER_INVALID);
    (*object)->Destroy(object);
}

TEST(an_output_mix_takes_only_the_interfaces_it_offers) {
    Engine engine;
    if (!engine.ok()) return;
    SLObjectItf mix = nullptr;
    const SLInterfaceID play[] = {SL_IID_PLAY};
    const SLboolean yes[] = {SL_BOOLEAN_TRUE};
    const SLboolean no[] = {SL_BOOLEAN_FALSE};
    CHECK((*engine.engine)->CreateOutputMix(engine.engine, &mix, 1, play, yes) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK(mix == nullptr);
    CHECK((*engine.engine)->CreateOutputMix(engine.engine, &mix, 1, play, no) == SL_RESULT_SUCCESS);
    if (mix == nullptr) return;
    CHECK((*mix)->Realize(mix, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    void *object = nullptr;
    CHECK((*mix)->GetInterface(mix, SL_IID_OBJECT, &object) == SL_RESULT_SUCCESS && object == mix);
    (*mix)->Destroy(mix);
}

// MARK: - Creating players

TEST(a_player_checks_its_format_as_android_15) {
    Engine engine;
    if (!engine.ok()) return;
    const auto check = [&](const char *what, const void *format, SLresult expected) {
        SLObjectItf player = nullptr;
        expect(what, create(engine, {.format = format, .ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE}}, &player), expected);
        CHECK((expected == SL_RESULT_SUCCESS) == (player != nullptr));
        if (player == nullptr) return;
        expect(what, (*player)->Realize(player, SL_BOOLEAN_FALSE), SL_RESULT_SUCCESS);
        (*player)->Destroy(player);
    };
    SLDataFormat_PCM f = pcm(1, 8, 8, SL_SAMPLINGRATE_8, SL_SPEAKER_FRONT_CENTER);
    check("PCM 8-bit mono at 8 kHz", &f, SL_RESULT_SUCCESS);
    f = pcm(2, 24, 24);
    check("PCM 24/24", &f, SL_RESULT_SUCCESS);
    f = pcm(2, 32, 32);
    check("PCM 32/32", &f, SL_RESULT_SUCCESS);
    f = pcm(2, 16, 32);
    check("PCM 16 in 32", &f, SL_RESULT_CONTENT_UNSUPPORTED);
    f = pcm(2, 16, 8);
    check("PCM 16 in 8", &f, SL_RESULT_PARAMETER_INVALID);
    f = pcm(2, 12, 16);
    check("PCM 12 in 16", &f, SL_RESULT_CONTENT_UNSUPPORTED);
    f = pcm(2, 16, 16, SL_SAMPLINGRATE_48, 0, SL_BYTEORDER_BIGENDIAN);
    check("PCM big-endian", &f, SL_RESULT_CONTENT_UNSUPPORTED);
    f = pcm(2, 16, 16, SL_SAMPLINGRATE_48, 0, 7);
    check("PCM byte order 7", &f, SL_RESULT_PARAMETER_INVALID);
    f = pcm(0, 16, 16);
    check("PCM 0 channels", &f, SL_RESULT_PARAMETER_INVALID);
    f = pcm(3, 16, 16);
    check("PCM 3 channels", &f, SL_RESULT_SUCCESS);
    f = pcm(8, 16, 16);
    check("PCM 8 channels", &f, SL_RESULT_SUCCESS);
    f = pcm(9, 16, 16);
    check("PCM 9 channels", &f, SL_RESULT_CONTENT_UNSUPPORTED);
    f = pcm(2, 16, 16, SL_SAMPLINGRATE_48, SL_SPEAKER_FRONT_CENTER);
    check("PCM 2 channels, a mask of 1", &f, SL_RESULT_PARAMETER_INVALID);
    f = pcm(1, 16, 16, SL_SAMPLINGRATE_48, SL_SPEAKER_FRONT_LEFT);
    check("PCM 1 channel, left", &f, SL_RESULT_SUCCESS);
    f = pcm(2, 16, 16, 7'999'000);
    check("PCM 7999 Hz", &f, SL_RESULT_CONTENT_UNSUPPORTED);
    f = pcm(2, 16, 16, 192'001'000);
    check("PCM 192001 Hz", &f, SL_RESULT_CONTENT_UNSUPPORTED);
    f = pcm(2, 16, 16, 22'050'500);
    check("PCM 22050.5 Hz", &f, SL_RESULT_SUCCESS);
    f = pcm(2, 16, 16, 0);
    check("PCM 0 Hz", &f, SL_RESULT_PARAMETER_INVALID);
    SLAndroidDataFormat_PCM_EX e = pcm_ex(2, 32, SL_ANDROID_PCM_REPRESENTATION_FLOAT);
    check("EX float 32", &e, SL_RESULT_SUCCESS);
    e = pcm_ex(2, 16, SL_ANDROID_PCM_REPRESENTATION_FLOAT);
    check("EX float 16", &e, SL_RESULT_PARAMETER_INVALID);
    e = pcm_ex(2, 8, SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT);
    check("EX signed 8", &e, SL_RESULT_PARAMETER_INVALID);
    e = pcm_ex(2, 24, SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT);
    check("EX signed 24", &e, SL_RESULT_SUCCESS);
    e = pcm_ex(2, 8, SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT);
    check("EX unsigned 8", &e, SL_RESULT_SUCCESS);
    e = pcm_ex(2, 16, SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT);
    check("EX unsigned 16", &e, SL_RESULT_PARAMETER_INVALID);
    e = pcm_ex(2, 16, 9);
    check("EX representation 9", &e, SL_RESULT_PARAMETER_INVALID);
    e = pcm_ex(2, 16, SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT, SL_ANDROID_MAKE_INDEXED_CHANNEL_MASK(3));
    check("EX indexed mask of 2", &e, SL_RESULT_SUCCESS);
    const SLuint32 other = 99;
    check("format type 99", &other, SL_RESULT_PARAMETER_INVALID);
    check("no format", nullptr, SL_RESULT_CONTENT_UNSUPPORTED);
}

TEST(a_player_checks_its_locators_and_interfaces_as_android_15) {
    Engine engine;
    if (!engine.ok()) return;
    SLObjectItf player = nullptr;
    const auto result = [&](const Spec &spec) {
        const SLresult created = create(engine, spec, &player);
        if (player != nullptr) (*player)->Destroy(player);
        player = nullptr;
        return created;
    };
    CHECK(result({.buffers = 0}) == SL_RESULT_PARAMETER_INVALID);
    CHECK(result({.buffers = 255}) == SL_RESULT_SUCCESS);
    CHECK(result({.buffers = 256}) == SL_RESULT_PARAMETER_INVALID);
    CHECK(result({.locator = SL_DATALOCATOR_BUFFERQUEUE}) == SL_RESULT_SUCCESS);
    CHECK(result({.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDSIMPLEBUFFERQUEUE}}) == SL_RESULT_SUCCESS);
    // Android 15 plays SEEK and MUTESOLO on a buffer queue only partly or not at all; Minato offers neither.
    CHECK(result({.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_SEEK}}) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK(result({.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_MUTESOLO}}) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK(result({.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ENGINE}}) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK(result({.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_SEEK},
                  .required = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE}}) == SL_RESULT_SUCCESS);

    SLDataLocator_AndroidSimpleBufferQueue queue{kQueue, 2};
    SLDataSource source{&queue, const_cast<SLDataFormat_PCM *>(&stereo16)};
    SLDataLocator_OutputMix mix{SL_DATALOCATOR_OUTPUTMIX, engine.mix};
    SLDataSink sink{&mix, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean yes[] = {SL_BOOLEAN_TRUE};
    SLEngineItf e = engine.engine;
    CHECK((*e)->CreateAudioPlayer(e, nullptr, &source, &sink, 1, ids, yes) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*e)->CreateAudioPlayer(e, &player, nullptr, &sink, 1, ids, yes) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*e)->CreateAudioPlayer(e, &player, &source, nullptr, 1, ids, yes) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*e)->CreateAudioPlayer(e, &player, &source, &sink, 1, nullptr, yes) == SL_RESULT_PARAMETER_INVALID);
    SLDataLocator_OutputMix no_mix{SL_DATALOCATOR_OUTPUTMIX, nullptr};
    SLDataSink no_mix_sink{&no_mix, nullptr};
    CHECK((*e)->CreateAudioPlayer(e, &player, &source, &no_mix_sink, 1, ids, yes) == SL_RESULT_PARAMETER_INVALID);
    std::int16_t memory[16] = {};
    SLDataLocator_Address address{SL_DATALOCATOR_ADDRESS, memory, sizeof(memory)};
    SLDataSink address_sink{&address, nullptr};
    CHECK((*e)->CreateAudioPlayer(e, &player, &source, &address_sink, 1, ids, yes) == SL_RESULT_CONTENT_UNSUPPORTED);
    CHECK(player == nullptr);
}

TEST(an_output_mix_must_be_realized_to_take_players) {
    Engine engine;
    if (!engine.ok()) return;
    SLObjectItf unrealized = nullptr;
    CHECK((*engine.engine)->CreateOutputMix(engine.engine, &unrealized, 0, nullptr, nullptr) == SL_RESULT_SUCCESS);
    if (unrealized == nullptr) return;
    SLDataLocator_AndroidSimpleBufferQueue queue{kQueue, 2};
    SLDataSource source{&queue, const_cast<SLDataFormat_PCM *>(&stereo16)};
    SLDataLocator_OutputMix mix{SL_DATALOCATOR_OUTPUTMIX, unrealized};
    SLDataSink sink{&mix, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean yes[] = {SL_BOOLEAN_TRUE};
    SLObjectItf player = nullptr;
    CHECK((*engine.engine)->CreateAudioPlayer(engine.engine, &player, &source, &sink, 1, ids, yes) ==
          SL_RESULT_PARAMETER_INVALID);  // as Android 15: "the output mix is not realized"
    (*unrealized)->Destroy(unrealized);
}

TEST(get_interface_answers_in_android_15s_order) {
    Engine engine;
    if (!engine.ok()) return;
    SLObjectItf player = nullptr;
    CHECK(create(engine, {.ids = {SL_IID_BUFFERQUEUE}}, &player) == SL_RESULT_SUCCESS);
    if (player == nullptr) return;
    void *itf = &player;
    CHECK((*player)->GetInterface(player, SL_IID_PLAY, nullptr) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*player)->GetInterface(player, nullptr, &itf) == SL_RESULT_PARAMETER_INVALID && itf == nullptr);
    CHECK((*player)->GetInterface(player, SL_IID_PLAY, &itf) == SL_RESULT_PRECONDITIONS_VIOLATED && itf == nullptr);
    CHECK((*player)->GetInterface(player, SL_IID_VOLUME, &itf) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK((*player)->GetInterface(player, SL_IID_ANDROIDCONFIGURATION, &itf) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK((*player)->GetInterface(player, SL_IID_OBJECT, &itf) == SL_RESULT_SUCCESS && itf == player);
    CHECK((*player)->Realize(player, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    CHECK((*player)->GetInterface(player, SL_IID_VOLUME, &itf) == SL_RESULT_FEATURE_UNSUPPORTED && itf == nullptr);
    CHECK((*player)->GetInterface(player, SL_IID_EFFECTSEND, &itf) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK((*player)->GetInterface(player, SL_IID_ENGINE, &itf) == SL_RESULT_FEATURE_UNSUPPORTED);
    void *play = nullptr, *by_copy = nullptr, *queue = nullptr, *simple = nullptr;
    const SLInterfaceID_ copy = *SL_IID_PLAY;
    CHECK((*player)->GetInterface(player, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS);
    CHECK((*player)->GetInterface(player, &copy, &by_copy) == SL_RESULT_SUCCESS && by_copy == play);
    CHECK((*player)->GetInterface(player, SL_IID_BUFFERQUEUE, &queue) == SL_RESULT_SUCCESS);
    CHECK((*player)->GetInterface(player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &simple) == SL_RESULT_SUCCESS &&
          simple == queue);
    SLuint32 state = 0;
    CHECK((*player)->GetState(player, nullptr) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*player)->GetState(player, &state) == SL_RESULT_SUCCESS && state == SL_OBJECT_STATE_REALIZED);
    (*player)->Destroy(player);
}

TEST(a_player_opens_a_float_stream_of_its_rate_and_at_least_two_channels) {
    Engine engine;
    if (!engine.ok()) return;
    const auto opened = [&](const void *format) {
        Player player(engine, {.format = format});
        return player.stream;
    };
    SLDataFormat_PCM f = pcm(1, 16, 16, SL_SAMPLINGRATE_44_1);
    Opened stream = opened(&f);
    CHECK(stream.sample_rate == 44'100 && stream.channels == 2);
    f = pcm(2, 16, 16, 22'050'500);
    stream = opened(&f);
    CHECK(stream.sample_rate == 22'050 && stream.channels == 2);
    f = pcm(6, 16, 16);
    stream = opened(&f);
    CHECK(stream.sample_rate == 48'000 && stream.channels == 6);
}

// MARK: - Samples

template <typename Sample>
void check_samples(const void *format, const std::vector<Sample> &samples, const std::vector<float> &expected) {
    Engine engine;
    Player player(engine, {.format = format});
    if (!player.ok()) return;
    CHECK(player.enqueue(samples.data(), static_cast<SLuint32>(samples.size() * sizeof(Sample))) == SL_RESULT_SUCCESS);
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    const auto channels = static_cast<std::size_t>(player.stream.channels);
    const auto got = read_frames(player.stream, static_cast<std::int64_t>(expected.size() / channels));
    if (!near(got, expected)) std::fprintf(stderr, "the samples differ\n");
    CHECK(near(got, expected));
}

TEST(each_pcm_format_plays_as_floats) {
    const SLDataFormat_PCM u8 = pcm(2, 8, 8);
    const SLAndroidDataFormat_PCM_EX ex_u8 = pcm_ex(2, 8, SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT);
    const std::vector<std::uint8_t> bytes = {0, 64, 128, 255, 192, 1, 128, 0};
    const std::vector<float> from_u8 = {-1.0f, -0.5f, 0.0f, 127.0f / 128, 0.5f, -127.0f / 128, 0.0f, -1.0f};
    check_samples(&u8, bytes, from_u8);
    check_samples(&ex_u8, bytes, from_u8);
    const SLDataFormat_PCM s16 = pcm(2, 16, 16);
    check_samples<std::int16_t>(&s16, {0, 16384, -32768, 32767, -1, 1, -16384, 8192},
                                {0.0f, 0.5f, -1.0f, 32767.0f / 32768, -1.0f / 32768, 1.0f / 32768, -0.5f, 0.25f});
    const SLDataFormat_PCM s24 = pcm(2, 24, 24);
    const SLAndroidDataFormat_PCM_EX ex_s24 = pcm_ex(2, 24, SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT);
    // little-endian: 0, 0x400000, 0x800000, 0x7fffff, 0xffffff, 1, 0xc00000, 0x200000
    const std::vector<std::uint8_t> packed = {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x80, 0xff, 0xff, 0x7f,
                                              0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x20};
    const std::vector<float> from_s24 = {0.0f,           0.5f,           -1.0f, 8388607.0f / 8388608,
                                         -1.0f / 8388608, 1.0f / 8388608, -0.5f, 0.25f};
    check_samples(&s24, packed, from_s24);
    check_samples(&ex_s24, packed, from_s24);
    const SLDataFormat_PCM s32 = pcm(2, 32, 32);
    check_samples<std::int32_t>(&s32, {0, 0x40000000, INT32_MIN, INT32_MAX, -1, 1, -0x40000000, 0x20000000},
                                {0.0f, 0.5f, -1.0f, 1.0f, -1.0f / 2147483648.0f, 1.0f / 2147483648.0f, -0.5f, 0.25f});
    check_samples<float>(&stereo_float, {0.0f, 0.5f, -1.0f, 1.0f, -0.25f, 0.25f, 0.125f, -0.125f},
                         {0.0f, 0.5f, -1.0f, 1.0f, -0.25f, 0.25f, 0.125f, -0.125f});
    check_samples<float>(&mono_float, {0.5f, -0.25f}, {0.5f, 0.5f, -0.25f, -0.25f});
    // The NDK's native-audio sample plays mono 16-bit at 8 kHz.
    const SLDataFormat_PCM mono16 = pcm(1, 16, 16, SL_SAMPLINGRATE_8);
    check_samples<std::int16_t>(&mono16, {16384, -8192}, {0.5f, 0.5f, -0.25f, -0.25f});
    const SLDataFormat_PCM mono8 = pcm(1, 8, 8);
    check_samples<std::uint8_t>(&mono8, {192, 64}, {0.5f, 0.5f, -0.5f, -0.5f});
    const SLDataFormat_PCM three = pcm(3, 16, 16);
    check_samples<std::int16_t>(&three, {16384, -16384, 8192, 0, 32767, -32768},
                                {0.5f, -0.5f, 0.25f, 0.0f, 32767.0f / 32768, -1.0f});
}

/// The first two frames a stereo or mono float player of 0.5 plays with `setup` done to its VOLUME before the start.
template <typename Setup>
std::vector<float> with_volume(const SLAndroidDataFormat_PCM_EX &format, Setup setup) {
    Engine engine;
    Player player(engine, {.format = &format});
    if (!player.ok() || player.volume == nullptr) return {};
    setup(player.volume);
    const std::vector<float> samples(4, 0.5f);
    player.enqueue(samples.data(), static_cast<SLuint32>(format.numChannels * 2 * sizeof(float)));
    player.set(SL_PLAYSTATE_PLAYING);
    return read_frames(player.stream, 2);
}

TEST(volume_mute_and_stereo_position_scale_the_samples_as_android_does) {
    const float level = 0.5f * std::pow(10.0f, -600.0f / 2000.0f);
    CHECK(near(with_volume(stereo_float, [](SLVolumeItf v) { (*v)->SetVolumeLevel(v, -600); }),
               {level, level, level, level}));
    CHECK(near(with_volume(stereo_float, [](SLVolumeItf v) { (*v)->SetVolumeLevel(v, SL_MILLIBEL_MIN); }),
               {0.0f, 0.0f, 0.0f, 0.0f}));
    CHECK(near(with_volume(stereo_float, [](SLVolumeItf v) { (*v)->SetMute(v, SL_BOOLEAN_TRUE); }),
               {0.0f, 0.0f, 0.0f, 0.0f}));
    // Stereo: a balance. Mono: a pan of constant power.
    CHECK(near(with_volume(stereo_float,
                           [](SLVolumeItf v) {
                               (*v)->SetStereoPosition(v, 500);
                               (*v)->EnableStereoPosition(v, SL_BOOLEAN_TRUE);
                           }),
               {0.25f, 0.5f, 0.25f, 0.5f}));
    CHECK(near(with_volume(stereo_float,
                           [](SLVolumeItf v) {
                               (*v)->SetStereoPosition(v, -250);
                               (*v)->EnableStereoPosition(v, SL_BOOLEAN_TRUE);
                           }),
               {0.5f, 0.375f, 0.5f, 0.375f}));
    CHECK(near(with_volume(stereo_float, [](SLVolumeItf v) { (*v)->SetStereoPosition(v, 500); }),
               {0.5f, 0.5f, 0.5f, 0.5f}));  // not enabled
    const float centre = 0.5f * std::cos(std::acos(-1.0f) / 4);
    CHECK(near(with_volume(mono_float, [](SLVolumeItf v) { (*v)->EnableStereoPosition(v, SL_BOOLEAN_TRUE); }),
               {centre, centre, centre, centre}));
    CHECK(near(with_volume(mono_float,
                           [](SLVolumeItf v) {
                               (*v)->SetStereoPosition(v, -1000);
                               (*v)->EnableStereoPosition(v, SL_BOOLEAN_TRUE);
                           }),
               {0.5f, 0.0f, 0.5f, 0.0f}));
}

TEST(the_volume_interface_answers_as_android_15) {
    Engine engine;
    Player player(engine);
    if (!player.ok() || player.volume == nullptr) return;
    SLVolumeItf v = player.volume;
    SLmillibel level = 7;
    SLboolean flag = 7;
    SLpermille position = 7;
    CHECK((*v)->GetVolumeLevel(v, &level) == SL_RESULT_SUCCESS && level == 0);
    CHECK((*v)->GetMaxVolumeLevel(v, &level) == SL_RESULT_SUCCESS && level == 0);
    CHECK((*v)->GetMute(v, &flag) == SL_RESULT_SUCCESS && flag == SL_BOOLEAN_FALSE);
    CHECK((*v)->IsEnabledStereoPosition(v, &flag) == SL_RESULT_SUCCESS && flag == SL_BOOLEAN_FALSE);
    CHECK((*v)->GetStereoPosition(v, &position) == SL_RESULT_SUCCESS && position == 0);
    CHECK((*v)->SetVolumeLevel(v, 1) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*v)->SetVolumeLevel(v, SL_MILLIBEL_MIN) == SL_RESULT_SUCCESS);
    CHECK((*v)->GetVolumeLevel(v, &level) == SL_RESULT_SUCCESS && level == SL_MILLIBEL_MIN);
    CHECK((*v)->SetMute(v, 2) == SL_RESULT_SUCCESS);
    CHECK((*v)->GetMute(v, &flag) == SL_RESULT_SUCCESS && flag == SL_BOOLEAN_TRUE);
    CHECK((*v)->SetStereoPosition(v, -1001) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*v)->SetStereoPosition(v, 1001) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*v)->SetStereoPosition(v, 500) == SL_RESULT_SUCCESS);
    CHECK((*v)->GetStereoPosition(v, &position) == SL_RESULT_SUCCESS && position == 500);
    CHECK((*v)->EnableStereoPosition(v, 2) == SL_RESULT_SUCCESS);
    CHECK((*v)->IsEnabledStereoPosition(v, &flag) == SL_RESULT_SUCCESS && flag == SL_BOOLEAN_TRUE);
    CHECK((*v)->GetVolumeLevel(v, nullptr) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*v)->GetMaxVolumeLevel(v, nullptr) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*v)->GetMute(v, nullptr) == SL_RESULT_PARAMETER_INVALID);
}

// MARK: - The buffer queue

/// Mono float buffers of `frames` frames, the i-th holding i + 1.
std::vector<std::vector<float>> numbered_buffers(int count, int frames) {
    std::vector<std::vector<float>> buffers;
    for (int index = 0; index < count; ++index) buffers.emplace_back(frames, static_cast<float>(index + 1));
    return buffers;
}

struct Calls {
    std::atomic<int> count{0};
    std::atomic<bool> on_test_thread{false};
    std::atomic<bool> other_caller{false};
    pthread_t test_thread = pthread_self();
    SLAndroidSimpleBufferQueueItf queue = nullptr;
    std::vector<std::vector<float>> *refills = nullptr;  // enqueued one by one from the callback, if set
    std::atomic<int> next_refill{0};
};

void count_call(SLAndroidSimpleBufferQueueItf caller, void *context) {
    auto &calls = *static_cast<Calls *>(context);
    if (caller != calls.queue) calls.other_caller.store(true);
    if (pthread_equal(pthread_self(), calls.test_thread)) calls.on_test_thread.store(true);
    if (calls.refills != nullptr) {
        const int next = calls.next_refill.fetch_add(1);
        if (next < static_cast<int>(calls.refills->size())) {
            const auto &buffer = (*calls.refills)[static_cast<std::size_t>(next)];
            (*caller)->Enqueue(caller, buffer.data(), static_cast<SLuint32>(buffer.size() * sizeof(float)));
        }
    }
    calls.count.fetch_add(1);
}

TEST(buffers_play_in_order_and_each_calls_back_once_on_the_stream_thread) {
    Calls calls;  // before the player, whose stream thread calls back until the player is destroyed
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 3});
    if (!player.ok()) return;
    calls.queue = player.queue;
    CHECK((*player.queue)->RegisterCallback(player.queue, count_call, &calls) == SL_RESULT_SUCCESS);
    const auto buffers = numbered_buffers(3, 100);
    for (const auto &buffer : buffers) {
        CHECK(player.enqueue(buffer.data(), static_cast<SLuint32>(buffer.size() * sizeof(float))) == SL_RESULT_SUCCESS);
    }
    CHECK(player.queue_state().count == 3);
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    const auto samples = read_all(player.stream);  // the queue's frames, the rest of that pull as zeros, then nothing
    bool in_order = samples.size() > 300 * 2;
    for (std::size_t frame = 0; in_order && frame < samples.size() / 2; ++frame) {
        const float expected = frame < 300 ? static_cast<float>(frame / 100 + 1) : 0.0f;
        in_order = samples[2 * frame] == expected && samples[2 * frame + 1] == expected;
    }
    CHECK(in_order);
    CHECK(within(kWait, [&] { return calls.count.load() == 3; }));
    std::this_thread::sleep_for(20ms);
    CHECK(calls.count.load() == 3);
    CHECK(!calls.on_test_thread.load() && !calls.other_caller.load());
    const auto state = player.queue_state();
    CHECK(state.count == 0 && state.index == 3);
}

TEST(a_refill_from_the_callback_plays_on_without_a_gap) {
    auto buffers = numbered_buffers(20, 100);
    Calls calls;
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 1});
    if (!player.ok()) return;
    calls.queue = player.queue;
    calls.refills = &buffers;
    calls.next_refill.store(1);
    (*player.queue)->RegisterCallback(player.queue, count_call, &calls);
    CHECK(player.enqueue(buffers[0].data(), 100 * sizeof(float)) == SL_RESULT_SUCCESS);
    CHECK(player.enqueue(buffers[1].data(), 100 * sizeof(float)) == SL_RESULT_BUFFER_INSUFFICIENT);
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    const auto samples = read_frames(player.stream, 2'000);
    bool whole = samples.size() == 4'000;
    for (std::size_t frame = 0; whole && frame < 2'000; ++frame) {
        whole = samples[2 * frame] == static_cast<float>(frame / 100 + 1);
    }
    CHECK(whole);
}

TEST(enqueue_and_the_callbacks_registration_answer_as_android_15) {
    Engine engine;
    Player player(engine);
    if (!player.ok()) return;
    std::int16_t frames[8] = {};
    CHECK(player.enqueue(nullptr, 8) == SL_RESULT_PARAMETER_INVALID);
    CHECK(player.enqueue(frames, 0) == SL_RESULT_PARAMETER_INVALID);
    CHECK(player.enqueue(frames, 3) == SL_RESULT_SUCCESS);  // not a whole frame: taken anyway
    CHECK(player.enqueue(frames, sizeof(frames)) == SL_RESULT_SUCCESS);
    CHECK(player.enqueue(frames, sizeof(frames)) == SL_RESULT_BUFFER_INSUFFICIENT);
    CHECK((*player.queue)->GetState(player.queue, nullptr) == SL_RESULT_PARAMETER_INVALID);
    Calls calls;
    CHECK((*player.queue)->RegisterCallback(player.queue, nullptr, nullptr) == SL_RESULT_SUCCESS);
    CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
    CHECK((*player.queue)->RegisterCallback(player.queue, count_call, &calls) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    CHECK((*player.queue)->RegisterCallback(player.queue, count_call, &calls) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK(player.set(SL_PLAYSTATE_STOPPED) == SL_RESULT_SUCCESS);
    CHECK((*player.queue)->RegisterCallback(player.queue, count_call, &calls) == SL_RESULT_SUCCESS);
}

TEST(clear_when_paused_empties_the_queue_and_drops_what_the_stream_holds) {
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 4});
    if (!player.ok()) return;
    const auto buffers = numbered_buffers(4, 480);
    for (const auto &buffer : buffers) player.enqueue(buffer.data(), 480 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    CHECK(read_frames(player.stream, 240).size() == 480);
    CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
    CHECK((*player.queue)->Clear(player.queue) == SL_RESULT_SUCCESS);
    const auto state = player.queue_state();
    CHECK(state.count == 0 && state.index == 0);
    player.enqueue(buffers[3].data(), 480 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    const auto samples = read_frames(player.stream, 480);  // the stream's frames of buffer 1 went with the Clear
    CHECK(samples.size() == 960 && samples.front() == 4.0f && samples.back() == 4.0f);
}

/// A queue callback that holds the stream thread in its first call, inside a pull, until the test opens it: what
/// a preempted stream thread does under load.
struct Held {
    std::atomic<bool> inside{false};
    std::atomic<bool> open{false};
    std::atomic<int> calls{0};
};

void held_call(SLAndroidSimpleBufferQueueItf, void *context) {
    auto &held = *static_cast<Held *>(context);
    if (held.calls.fetch_add(1) > 0) return;
    held.inside.store(true);
    within(kWait, [&] { return held.open.load(); });
}

TEST(a_pull_under_way_across_a_clear_when_paused_and_a_restart_plays_what_it_takes_after_them) {
    Held held;
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 2});
    if (!player.ok()) return;
    (*player.queue)->RegisterCallback(player.queue, held_call, &held);
    const std::vector<float> first(240, 1.0f), second(480, 2.0f);
    player.enqueue(first.data(), 240 * sizeof(float));  // one pull; the next one tells its end before taking a frame
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    CHECK(within(kWait, [&] { return held.inside.load(); }));
    CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
    CHECK((*player.queue)->Clear(player.queue) == SL_RESULT_SUCCESS);
    player.enqueue(second.data(), 480 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    held.open.store(true);
    const auto samples = read_frames(player.stream, 480);  // the flush came after the pull began, before it took them
    CHECK(samples.size() == 960 && samples.front() == 2.0f && samples.back() == 2.0f);
}

TEST(what_a_pull_took_before_a_clear_when_paused_or_a_stop_goes_with_it_though_the_pull_goes_on_after_a_restart) {
    for (const bool stop : {false, true}) {
        Held held;
        Engine engine;
        Player player(engine, {.format = &mono_float, .buffers = 2});
        if (!player.ok()) return;
        (*player.queue)->RegisterCallback(player.queue, held_call, &held);
        const std::vector<float> first(120, 1.0f), second(480, 2.0f);
        player.enqueue(first.data(), 120 * sizeof(float));  // half a pull, whose end the same pull tells
        if (stop) player.enqueue(second.data(), 480 * sizeof(float));  // STOPPED keeps the queue
        CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
        CHECK(within(kWait, [&] { return held.inside.load(); }));
        if (stop) {
            CHECK(player.set(SL_PLAYSTATE_STOPPED) == SL_RESULT_SUCCESS);
        } else {
            CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
            CHECK((*player.queue)->Clear(player.queue) == SL_RESULT_SUCCESS);
            player.enqueue(second.data(), 480 * sizeof(float));
        }
        CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
        held.open.store(true);
        const auto samples = read_frames(player.stream, 480);
        CHECK(samples.size() == 960 && samples.front() == 2.0f && samples.back() == 2.0f);
    }
}

TEST(stopped_keeps_the_queue_and_drops_what_a_paused_stream_holds) {
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 2});
    if (!player.ok()) return;
    const auto buffers = numbered_buffers(2, 480);
    for (const auto &buffer : buffers) player.enqueue(buffer.data(), 480 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    CHECK(read_frames(player.stream, 240).size() == 480);  // the stream holds the rest of buffer 1
    CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
    CHECK(player.set(SL_PLAYSTATE_STOPPED) == SL_RESULT_SUCCESS);
    CHECK(player.queue_state().count == 1);  // as Android 15: only Clear empties the queue
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    const auto samples = read_frames(player.stream, 1);
    CHECK(samples.size() == 2 && samples.front() == 2.0f);  // the queue goes on; the rest of buffer 1 is gone
}

TEST(clear_forgets_the_end_of_a_buffer_the_app_was_not_told_yet) {
    Calls calls;
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 1});
    if (!player.ok()) return;
    calls.queue = player.queue;
    (*player.queue)->RegisterCallback(player.queue, count_call, &calls);
    const std::vector<float> frames(480, 0.5f);  // two pulls, which fill the stream's buffer: no third pull tells it
    player.enqueue(frames.data(), 480 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    CHECK(within(kWait, [&] { return player.queue_state().index == 1; }));
    CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
    CHECK((*player.queue)->Clear(player.queue) == SL_RESULT_SUCCESS);
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    read_all(player.stream);
    CHECK(calls.count.load() == 0);
}

struct StopOnFirst {
    SLPlayItf play = nullptr;
    std::atomic<int> calls{0};
};

void stop_on_first(SLAndroidSimpleBufferQueueItf, void *context) {
    auto &stop = *static_cast<StopOnFirst *>(context);
    if (stop.calls.fetch_add(1) == 0) (*stop.play)->SetPlayState(stop.play, SL_PLAYSTATE_STOPPED);
}

TEST(a_stop_from_the_queue_callback_takes_no_more_frames) {
    StopOnFirst stop;
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 3});
    if (!player.ok()) return;
    stop.play = player.play;
    (*player.queue)->RegisterCallback(player.queue, stop_on_first, &stop);
    const auto buffers = numbered_buffers(3, 100);
    for (const auto &buffer : buffers) player.enqueue(buffer.data(), 100 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    CHECK(within(kWait, [&] { return stop.calls.load() > 0; }));
    read_all(player.stream);
    const auto state = player.queue_state();
    CHECK(state.count == 2 && state.index == 1);  // the pull under way took nothing after the stop
    CHECK(stop.calls.load() == 1);
}

TEST(a_player_whose_queue_runs_empty_stops_its_stream_and_an_enqueue_starts_it_again) {
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 2});
    if (!player.ok()) return;
    const std::vector<float> first(100, 0.5f), second(100, 0.25f);
    player.enqueue(first.data(), 100 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    const auto played = read_all(player.stream);  // the frames and the rest of their pull as zeros; then the stop
    CHECK(played.size() >= 2 * 100 && played.front() == 0.5f && played[2 * 99] == 0.5f && played.back() == 0.0f);
    std::this_thread::sleep_for(20ms);
    CHECK(render(player.stream.handle, 240, 2).empty());  // no pulls, no zeros: an idle player costs nothing
    SLuint32 state = 0;
    CHECK((*player.play)->GetPlayState(player.play, &state) == SL_RESULT_SUCCESS && state == SL_PLAYSTATE_PLAYING);
    CHECK(player.enqueue(second.data(), 100 * sizeof(float)) == SL_RESULT_SUCCESS);
    const auto samples = read_frames(player.stream, 100);
    CHECK(samples.size() == 200 && samples.front() == 0.25f && samples.back() == 0.25f);
}

TEST(a_buffer_of_less_than_a_frame_ends_at_once_and_plays_nothing) {
    Calls calls;
    Engine engine;
    Player player(engine, {.format = &stereo16});
    if (!player.ok()) return;
    calls.queue = player.queue;
    (*player.queue)->RegisterCallback(player.queue, count_call, &calls);
    const std::vector<std::int16_t> frames = {16384, 16384, 16384, 16384};  // two frames of 4 bytes
    player.enqueue(frames.data(), 3);
    player.enqueue(frames.data(), 8);
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    const auto samples = read_frames(player.stream, 2);
    CHECK(samples.size() == 4 && samples.front() == 0.5f && samples.back() == 0.5f);
    CHECK(within(kWait, [&] { return calls.count.load() == 2; }));
}

TEST(a_paused_player_whose_queue_ran_empty_starts_at_the_next_play_not_at_an_enqueue) {
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 2});
    if (!player.ok()) return;
    const std::vector<float> frames(100, 0.5f);
    player.enqueue(frames.data(), 100 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    read_all(player.stream);  // played, then the queue ran empty and the stream stopped
    CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
    player.enqueue(frames.data(), 100 * sizeof(float));
    std::this_thread::sleep_for(20ms);
    CHECK(render(player.stream.handle, 240, 2).empty());  // paused: the enqueue starts nothing
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    const auto samples = read_frames(player.stream, 100);
    CHECK(samples.size() == 200 && samples.front() == 0.5f && samples.back() == 0.5f);
}

TEST(clear_of_a_playing_player_whose_queue_ran_empty_drops_what_its_stream_holds) {
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 1});
    if (!player.ok()) return;
    const std::vector<float> frames(480, 0.5f);
    player.enqueue(frames.data(), 480 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    CHECK(read_frames(player.stream, 240).size() == 480);  // room for a pull, which finds the queue empty: a stop
    std::this_thread::sleep_for(20ms);
    CHECK((*player.queue)->Clear(player.queue) == SL_RESULT_SUCCESS);  // a stopped AudioTrack flushes
    CHECK(render(player.stream.handle, 240, 2).empty());
}

TEST(clear_while_playing_empties_the_queue_but_not_the_stream) {
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 3});
    if (!player.ok()) return;
    const auto buffers = numbered_buffers(3, 480);
    for (const auto &buffer : buffers) player.enqueue(buffer.data(), 480 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    CHECK(read_frames(player.stream, 240).size() == 480);
    CHECK((*player.queue)->Clear(player.queue) == SL_RESULT_SUCCESS);
    const auto state = player.queue_state();
    CHECK(state.count == 0 && state.index == 0);
    const auto rest = read_frames(player.stream, 1);
    CHECK(rest.size() == 2 && rest.front() == 1.0f);  // what the stream holds still plays, as a playing AudioTrack's
}

TEST(a_producer_thread_feeds_the_queue_while_the_stream_thread_takes_it) {
    Calls calls;
    const auto buffers = numbered_buffers(100, 240);
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 4});
    if (!player.ok()) return;
    calls.queue = player.queue;
    (*player.queue)->RegisterCallback(player.queue, count_call, &calls);
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    std::atomic<int> refused{0};
    std::thread producer([&] {
        for (const auto &buffer : buffers) {
            if (!within(kWait, [&] { return player.queue_state().count < 4; })) break;
            if (player.enqueue(buffer.data(), 240 * sizeof(float)) != SL_RESULT_SUCCESS) refused.fetch_add(1);
        }
    });
    std::vector<float> samples;
    within(kWait, [&] {
        const auto got = render(player.stream.handle, 240, 2);
        samples.insert(samples.end(), got.begin(), got.end());
        return calls.count.load() == 100;
    });
    producer.join();
    const auto rest = read_all(player.stream);  // the last buffer's end is told before all its frames are heard
    samples.insert(samples.end(), rest.begin(), rest.end());
    CHECK(refused.load() == 0 && calls.count.load() == 100);
    float last = 0.0f;
    bool rising = true;  // every buffer in order, with zeros only where the queue ran empty
    for (std::size_t sample = 0; sample < samples.size(); sample += 2) {
        if (samples[sample] == 0.0f) continue;
        rising = rising && samples[sample] >= last;
        last = samples[sample];
    }
    CHECK(rising && last == 100.0f);
}

// MARK: - The play interface

std::atomic<int> events[5];  // HEADATEND, HEADATMARKER, HEADATNEWPOS, HEADMOVING, HEADSTALLED

void count_event(SLPlayItf caller, void *, SLuint32 event) {
    SLmillisecond position = 0;
    (*caller)->GetPosition(caller, &position);  // as apps do: the player's lock is not held
    for (int bit = 0; bit < 5; ++bit) {
        if ((event & (1u << bit)) != 0) events[bit].fetch_add(1);
    }
}

TEST(the_play_interface_answers_as_android_15) {
    Engine engine;
    Player player(engine);
    if (!player.ok()) return;
    SLPlayItf p = player.play;
    SLuint32 state = 0, mask = 7;
    SLmillisecond time = 7;
    CHECK((*p)->GetPlayState(p, &state) == SL_RESULT_SUCCESS && state == SL_PLAYSTATE_STOPPED);
    CHECK((*p)->SetPlayState(p, 0) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*p)->SetPlayState(p, 4) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*p)->GetDuration(p, &time) == SL_RESULT_SUCCESS && time == SL_TIME_UNKNOWN);
    CHECK((*p)->GetPosition(p, &time) == SL_RESULT_SUCCESS && time == 0);
    CHECK((*p)->GetCallbackEventsMask(p, &mask) == SL_RESULT_SUCCESS && mask == 0);
    CHECK((*p)->GetPositionUpdatePeriod(p, &time) == SL_RESULT_SUCCESS && time == 1'000);
    time = 7;
    CHECK((*p)->GetMarkerPosition(p, &time) == SL_RESULT_PRECONDITIONS_VIOLATED && time == SL_TIME_UNKNOWN);
    CHECK((*p)->SetPositionUpdatePeriod(p, 0) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*p)->SetMarkerPosition(p, 250) == SL_RESULT_SUCCESS);
    CHECK((*p)->GetMarkerPosition(p, &time) == SL_RESULT_SUCCESS && time == 250);
    CHECK((*p)->ClearMarkerPosition(p) == SL_RESULT_SUCCESS);
    CHECK((*p)->GetMarkerPosition(p, &time) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK((*p)->GetPlayState(p, nullptr) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*p)->GetPosition(p, nullptr) == SL_RESULT_PARAMETER_INVALID);
}

TEST(the_position_counts_what_played_since_a_stop_a_clear_or_an_empty_queue) {
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 3});
    if (!player.ok()) return;
    const std::vector<float> tenth(4'800, 0.25f);  // 100 ms at 48 kHz
    for (int index = 0; index < 3; ++index) player.enqueue(tenth.data(), 4'800 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    read_frames(player.stream, 4'800);
    CHECK(player.position() == 100);
    CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
    CHECK(render(player.stream.handle, 240, 2).empty() && player.position() == 100);
    CHECK(player.set(SL_PLAYSTATE_STOPPED) == SL_RESULT_SUCCESS);
    CHECK(player.position() == 0);
    drain(player.stream);  // a stop from PAUSED dropped what the stream held
    CHECK(player.position() == 0);
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    read_frames(player.stream, 2'400);
    CHECK(player.position() == 50);
    read_all(player.stream);  // past the end of the queue: the stream stops, and the position is 0 again
    CHECK(player.position() == 0);
    player.enqueue(tenth.data(), 4'800 * sizeof(float));  // still PLAYING: the stream starts again
    read_frames(player.stream, 480);
    CHECK(player.position() == 10);
    CHECK(player.set(SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS);
    CHECK((*player.queue)->Clear(player.queue) == SL_RESULT_SUCCESS);
    CHECK(player.position() == 0);
}

TEST(play_events_come_as_android_15s_new_positions_marker_and_end) {
    for (auto &count : events) count.store(0);
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 7});
    if (!player.ok()) return;
    SLPlayItf p = player.play;
    CHECK((*p)->RegisterCallback(p, count_event, nullptr) == SL_RESULT_SUCCESS);
    CHECK((*p)->SetCallbackEventsMask(p, 0x1f) == SL_RESULT_SUCCESS);
    CHECK((*p)->SetPositionUpdatePeriod(p, 100) == SL_RESULT_SUCCESS);
    CHECK((*p)->SetMarkerPosition(p, 250) == SL_RESULT_SUCCESS);
    const std::vector<float> tenth(4'800, 0.25f);
    for (int index = 0; index < 7; ++index) player.enqueue(tenth.data(), 4'800 * sizeof(float));
    CHECK(player.set(SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    read_frames(player.stream, 6 * 4'800);  // 600 ms
    CHECK(within(kWait, [] { return events[2].load() == 6; }));
    CHECK(events[1].load() == 1 && events[0].load() == 0);
    read_all(player.stream);  // the last 100 ms, then the empty queue stops the stream
    CHECK(within(kWait, [] { return events[0].load() == 1; }));
    read_all(player.stream);
    // The pull that finds the queue empty looks at the position between 695 and 700 ms, as the render block lets it.
    CHECK(events[0].load() == 1 && events[1].load() == 1 && (events[2].load() == 6 || events[2].load() == 7));
    CHECK(events[3].load() == 0 && events[4].load() == 0);
}

/// Keeps a stereo float player fed from its callback with 10 ms buffers of 0.25.
void keep_fed(SLAndroidSimpleBufferQueueItf queue, void *) {
    static const std::vector<float> tenth(2 * 480, 0.25f);
    (*queue)->Enqueue(queue, tenth.data(), static_cast<SLuint32>(tenth.size() * sizeof(float)));
}

TEST(a_stop_arms_the_marker_again_and_a_clear_when_paused_ends_marker_and_new_positions_until_they_change) {
    for (auto &count : events) count.store(0);
    Engine engine;
    Player player(engine, {.format = &stereo_float, .buffers = 2});
    if (!player.ok()) return;
    SLPlayItf p = player.play;
    (*p)->RegisterCallback(p, count_event, nullptr);
    (*p)->SetCallbackEventsMask(p, SL_PLAYEVENT_HEADATMARKER | SL_PLAYEVENT_HEADATNEWPOS);
    (*p)->SetPositionUpdatePeriod(p, 20);
    (*p)->SetMarkerPosition(p, 50);
    (*player.queue)->RegisterCallback(player.queue, keep_fed, nullptr);
    const std::vector<float> first(2 * 480, 0.25f);
    player.enqueue(first.data(), static_cast<SLuint32>(first.size() * sizeof(float)));
    const auto play_100_ms = [&] {
        player.set(SL_PLAYSTATE_PLAYING);
        read_frames(player.stream, 4'800);
    };
    const auto settled = [](int markers, int positions) {
        const bool reached =
            within(kWait, [&] { return events[1].load() >= markers && events[2].load() >= positions; });
        std::this_thread::sleep_for(20ms);
        return reached && events[1].load() == markers && events[2].load() == positions;
    };
    play_100_ms();
    CHECK(settled(1, 5));
    player.set(SL_PLAYSTATE_STOPPED);  // as Android 15: the position starts again, and the marker with it
    play_100_ms();  // what the stream held at the stop plays first and does not count: 90 or 95 ms
    CHECK(settled(2, 9));
    player.set(SL_PLAYSTATE_PAUSED);
    (*player.queue)->Clear(player.queue);  // as Android 15: no marker or new position from now on
    player.enqueue(first.data(), static_cast<SLuint32>(first.size() * sizeof(float)));
    play_100_ms();
    (*p)->SetMarkerPosition(p, 50);  // the same marker, period and mask: nothing changes
    (*p)->SetPositionUpdatePeriod(p, 20);
    (*p)->SetCallbackEventsMask(p, SL_PLAYEVENT_HEADATMARKER | SL_PLAYEVENT_HEADATNEWPOS);
    play_100_ms();
    CHECK(settled(2, 9));
    SLmillisecond value = 0;
    CHECK((*p)->GetMarkerPosition(p, &value) == SL_RESULT_SUCCESS && value == 50);
    CHECK((*p)->GetPositionUpdatePeriod(p, &value) == SL_RESULT_SUCCESS && value == 20);
    (*p)->SetPositionUpdatePeriod(p, 25);  // a change sets both again [from memory of AOSP's useEventMask]
    play_100_ms();  // from 200 to 300 ms: the marker, passed, and the new positions at 225 to 300
    CHECK(settled(3, 13));
}

TEST(play_events_come_only_as_the_mask_asks_and_once_per_period_within_a_pull) {
    const std::vector<float> tenth(4'800, 0.25f);
    for (const SLuint32 mask : {SL_PLAYEVENT_HEADATNEWPOS, SL_PLAYEVENT_HEADATMARKER}) {
        for (auto &count : events) count.store(0);
        Engine engine;
        Player player(engine, {.format = &mono_float, .buffers = 1});
        if (!player.ok()) return;
        SLPlayItf p = player.play;
        (*p)->RegisterCallback(p, count_event, nullptr);
        (*p)->SetCallbackEventsMask(p, mask);
        (*p)->SetPositionUpdatePeriod(p, 1);  // shorter than a pull of 5 ms
        (*p)->SetMarkerPosition(p, 5);
        player.enqueue(tenth.data(), 4'800 * sizeof(float));
        player.set(SL_PLAYSTATE_PLAYING);
        read_frames(player.stream, 480);  // 10 ms
        if (mask == SL_PLAYEVENT_HEADATNEWPOS) {
            CHECK(within(kWait, [] { return events[2].load() == 10; }));
            CHECK(events[1].load() == 0);
        } else {
            CHECK(within(kWait, [] { return events[1].load() == 1; }));
            std::this_thread::sleep_for(20ms);
            CHECK(events[2].load() == 0);
        }
    }
}

// MARK: - ANDROIDCONFIGURATION and EFFECTSEND

TEST(android_configuration_answers_as_android_15_and_sets_the_streams_mode) {
    Engine engine;
    if (!engine.ok()) return;
    SLObjectItf player = nullptr;
    CHECK(create(engine, {.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION}}, &player) ==
          SL_RESULT_SUCCESS);
    if (player == nullptr) return;
    SLAndroidConfigurationItf c = nullptr;
    CHECK((*player)->GetInterface(player, SL_IID_ANDROIDCONFIGURATION, &c) == SL_RESULT_SUCCESS);
    if (c == nullptr) return;
    SLint32 type = -1;
    SLuint32 mode = 99, size = 4;
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &size, &type) == SL_RESULT_SUCCESS && type == 3);
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_PERFORMANCE_MODE, &size, &mode) == SL_RESULT_SUCCESS && mode == 1);
    size = 2;
    type = -1;
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &size, &type) == SL_RESULT_BUFFER_INSUFFICIENT);
    CHECK(size == 4 && type == -1);
    size = 0;
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &size, nullptr) == SL_RESULT_SUCCESS && size == 4);
    size = 8;
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &size, &type) == SL_RESULT_SUCCESS && size == 4);
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, nullptr, &type) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*c)->GetConfiguration(c, nullptr, &size, &type) == SL_RESULT_PARAMETER_INVALID);
    const auto *unknown = reinterpret_cast<const SLchar *>("foo");
    CHECK((*c)->GetConfiguration(c, unknown, &size, &type) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_RECORDING_PRESET, &size, &type) == SL_RESULT_PARAMETER_INVALID);
    type = 99;
    CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &type, 4) == SL_RESULT_PARAMETER_INVALID);
    type = SL_ANDROID_STREAM_ALARM;
    CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &type, 2) == SL_RESULT_BUFFER_INSUFFICIENT);
    CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &type, 0) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, nullptr, 4) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*c)->SetConfiguration(c, nullptr, &type, 4) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*c)->SetConfiguration(c, unknown, &type, 4) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &type, 8) == SL_RESULT_SUCCESS);
    size = 4;
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_STREAM_TYPE, &size, &type) == SL_RESULT_SUCCESS && type == 4);
    mode = 9;
    CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_PERFORMANCE_MODE, &mode, 4) == SL_RESULT_PARAMETER_INVALID);
    mode = SL_ANDROID_PERFORMANCE_POWER_SAVING;
    CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_PERFORMANCE_MODE, &mode, 4) == SL_RESULT_SUCCESS);
    CHECK((*player)->Realize(player, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    CHECK(last_opened().performance_mode == 11);  // AAUDIO_PERFORMANCE_MODE_POWER_SAVING
    CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_PERFORMANCE_MODE, &mode, 4) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK((*c)->SetConfiguration(c, unknown, &mode, 4) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*c)->GetConfiguration(c, SL_ANDROID_KEY_PERFORMANCE_MODE, &size, &mode) == SL_RESULT_SUCCESS && mode == 3);
    (*player)->Destroy(player);

    // Each mode's stream, and the default's
    const SLuint32 aaudio_modes[] = {10, 12, 12, 11};
    for (SLuint32 each = 0; each <= 4; ++each) {
        player = nullptr;
        create(engine, {.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION}}, &player);
        if (player == nullptr) return;
        (*player)->GetInterface(player, SL_IID_ANDROIDCONFIGURATION, &c);
        if (each < 4) CHECK((*c)->SetConfiguration(c, SL_ANDROID_KEY_PERFORMANCE_MODE, &each, 4) == SL_RESULT_SUCCESS);
        (*player)->Realize(player, SL_BOOLEAN_FALSE);
        CHECK(last_opened().performance_mode == static_cast<std::int32_t>(each < 4 ? aaudio_modes[each] : 12));
        (*player)->Destroy(player);
    }
}

TEST(effect_send_keeps_the_direct_level_and_knows_no_effect) {
    Engine engine;
    Player player(engine, {.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_EFFECTSEND}});
    if (!player.ok()) return;
    SLEffectSendItf s = nullptr;
    CHECK((*player.object)->GetInterface(player.object, SL_IID_EFFECTSEND, &s) == SL_RESULT_SUCCESS);
    if (s == nullptr) return;
    SLmillibel level = 7;
    CHECK((*s)->GetDirectLevel(s, &level) == SL_RESULT_SUCCESS && level == 0);
    CHECK((*s)->SetDirectLevel(s, -300) == SL_RESULT_SUCCESS);
    CHECK((*s)->GetDirectLevel(s, &level) == SL_RESULT_SUCCESS && level == -300);
    CHECK((*s)->SetDirectLevel(s, 1) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*s)->SetDirectLevel(s, SL_MILLIBEL_MIN) == SL_RESULT_SUCCESS);
    CHECK((*s)->GetDirectLevel(s, nullptr) == SL_RESULT_PARAMETER_INVALID);
    int effect[16] = {};
    CHECK((*s)->EnableEffectSend(s, nullptr, SL_BOOLEAN_TRUE, 0) == SL_RESULT_PARAMETER_INVALID);
    CHECK((*s)->EnableEffectSend(s, effect, SL_BOOLEAN_TRUE, 0) == SL_RESULT_PARAMETER_INVALID);
    SLboolean enabled = 7;
    CHECK((*s)->IsEnabled(s, nullptr, &enabled) == SL_RESULT_PARAMETER_INVALID && enabled == SL_BOOLEAN_FALSE);
    CHECK((*s)->SetSendLevel(s, nullptr, -100) == SL_RESULT_PARAMETER_INVALID);
    level = 7;
    CHECK((*s)->GetSendLevel(s, nullptr, &level) == SL_RESULT_PARAMETER_INVALID && level == 7);
    // The direct level is not a gain here: the player plays as before.
    const std::vector<std::int16_t> frames = {16384, 16384};
    player.enqueue(frames.data(), 4);
    player.set(SL_PLAYSTATE_PLAYING);
    CHECK(near(read_frames(player.stream, 1), {0.5f, 0.5f}));
}

// MARK: - Lifetimes and what Minato leaves out

TEST(a_player_destroyed_while_it_plays_calls_back_no_more_and_closes_its_stream) {
    auto buffers = numbered_buffers(1'000, 100);
    Calls calls;
    Engine engine;
    auto player = std::make_unique<Player>(engine, Spec{.format = &mono_float, .buffers = 1});
    if (!player->ok()) return;
    calls.queue = player->queue;
    calls.refills = &buffers;
    calls.next_refill.store(1);
    (*player->queue)->RegisterCallback(player->queue, count_call, &calls);
    player->enqueue(buffers[0].data(), 100 * sizeof(float));
    player->set(SL_PLAYSTATE_PLAYING);
    const Opened stream = player->stream;
    CHECK(within(kWait, [&] {
        render(stream.handle, 240, 2);
        return calls.count.load() > 3;
    }));
    player->destroy();
    const int count = calls.count.load();
    render(stream.handle, 240, 2);
    std::this_thread::sleep_for(20ms);
    CHECK(calls.count.load() == count);
    const std::lock_guard hold(fake.lock);
    CHECK(std::find(fake.closed.begin(), fake.closed.end(), stream.handle) != fake.closed.end());
}

struct SelfDestroy {
    SLObjectItf object = nullptr;
    std::atomic<int> calls{0};
};

void destroy_own_player(SLAndroidSimpleBufferQueueItf, void *context) {
    auto &self = *static_cast<SelfDestroy *>(context);
    if (self.calls.fetch_add(1) == 0) (*self.object)->Destroy(self.object);
}

TEST(a_player_destroyed_from_its_own_callback_stops_and_calls_back_no_more) {
    SelfDestroy self;
    Engine engine;
    if (!engine.ok()) return;
    CHECK(create(engine, {.format = &mono_float}, &self.object) == SL_RESULT_SUCCESS);
    if (self.object == nullptr) return;
    CHECK((*self.object)->Realize(self.object, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    const Opened stream = last_opened();
    SLPlayItf play = nullptr;
    SLAndroidSimpleBufferQueueItf queue = nullptr;
    (*self.object)->GetInterface(self.object, SL_IID_PLAY, &play);
    (*self.object)->GetInterface(self.object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue);
    if (play == nullptr || queue == nullptr) return;
    (*queue)->RegisterCallback(queue, destroy_own_player, &self);
    const std::vector<float> frames(100, 0.5f);
    (*queue)->Enqueue(queue, frames.data(), 100 * sizeof(float));
    (*queue)->Enqueue(queue, frames.data(), 100 * sizeof(float));
    (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
    CHECK(within(kWait, [&] {
        render(stream.handle, 240, 2);
        return self.calls.load() > 0;
    }));
    drain(stream);  // the player stopped: what its stream held plays out, then nothing
    std::this_thread::sleep_for(20ms);
    CHECK(render(stream.handle, 240, 2).empty());
    CHECK(self.calls.load() == 1);  // the second buffer's end is never told
    // Like Android's, the player cannot be used after its Destroy; this one is left, not freed.
}

struct Slow {
    std::atomic<bool> inside{false};
    std::atomic<int> calls{0};
};

void slow_call(SLAndroidSimpleBufferQueueItf, void *context) {
    auto &slow = *static_cast<Slow *>(context);
    slow.calls.fetch_add(1);
    slow.inside.store(true);
    std::this_thread::sleep_for(50ms);
    slow.inside.store(false);
}

TEST(a_destroy_waits_for_a_callback_under_way_and_no_callback_starts_after_it_began) {
    Slow slow;
    Engine engine;
    Player player(engine, {.format = &mono_float, .buffers = 2});
    if (!player.ok()) return;
    (*player.queue)->RegisterCallback(player.queue, slow_call, &slow);
    const auto buffers = numbered_buffers(2, 100);  // the second ends in the same pull, after the slow callback
    for (const auto &buffer : buffers) player.enqueue(buffer.data(), 100 * sizeof(float));
    player.set(SL_PLAYSTATE_PLAYING);
    CHECK(within(kWait, [&] { return slow.inside.load(); }));
    player.destroy();
    CHECK(!slow.inside.load());  // Destroy returned after the callback
    CHECK(slow.calls.load() == 1);  // and none started once it had begun, as Android's CallbackProtector refuses them
}

struct PlayDestroy {
    SLObjectItf object = nullptr;
    std::atomic<int> events{0};
    std::atomic<SLuint32> first{0};
};

void destroy_at_the_first_event(SLPlayItf, void *context, SLuint32 event) {
    auto &destroy = *static_cast<PlayDestroy *>(context);
    if (destroy.events.fetch_add(1) != 0) return;
    destroy.first.store(event);
    (*destroy.object)->Destroy(destroy.object);
}

TEST(a_destroy_from_a_play_callback_ends_the_events_of_its_pull) {
    PlayDestroy destroy;
    Engine engine;
    if (!engine.ok()) return;
    CHECK(create(engine, {.format = &mono_float}, &destroy.object) == SL_RESULT_SUCCESS);
    if (destroy.object == nullptr) return;
    CHECK((*destroy.object)->Realize(destroy.object, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    const Opened stream = last_opened();
    SLPlayItf play = nullptr;
    SLAndroidSimpleBufferQueueItf queue = nullptr;
    (*destroy.object)->GetInterface(destroy.object, SL_IID_PLAY, &play);
    (*destroy.object)->GetInterface(destroy.object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue);
    if (play == nullptr || queue == nullptr) return;
    (*play)->RegisterCallback(play, destroy_at_the_first_event, &destroy);
    (*play)->SetCallbackEventsMask(play, SL_PLAYEVENT_HEADATMARKER | SL_PLAYEVENT_HEADATNEWPOS);
    (*play)->SetPositionUpdatePeriod(play, 5);
    (*play)->SetMarkerPosition(play, 5);  // the marker and a new position in one pull, the marker first
    const std::vector<float> frames(4'800, 0.5f);
    (*queue)->Enqueue(queue, frames.data(), 4'800 * sizeof(float));
    (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
    read_frames(stream, 480);
    CHECK(within(kWait, [&] { return destroy.events.load() > 0; }));
    std::this_thread::sleep_for(20ms);
    CHECK(destroy.events.load() == 1 && destroy.first.load() == SL_PLAYEVENT_HEADATMARKER);
    // Like Android's, the player cannot be used after its Destroy; this one is left, not freed.
}

TEST(a_destroy_of_an_output_mix_with_players_or_of_an_engine_with_objects_is_ignored_as_android_15s) {
    Engine engine;
    auto player = std::make_unique<Player>(engine);
    if (!player->ok()) return;
    const int before = log_lines("libOpenSLES");
    (*engine.mix)->Destroy(engine.mix);  // the player plays into it
    (*engine.object)->Destroy(engine.object);  // the mix and the player were made by it
    CHECK(log_lines("libOpenSLES") == before + 2);
    SLuint32 state = 0;
    CHECK((*engine.mix)->GetState(engine.mix, &state) == SL_RESULT_SUCCESS && state == SL_OBJECT_STATE_REALIZED);
    SLObjectItf another = nullptr;
    CHECK(create(engine, {}, &another) == SL_RESULT_SUCCESS);  // the mix and the engine still work
    if (another != nullptr) (*another)->Destroy(another);
    player.reset();  // then the Engine's destructor destroys the mix and the engine for good
}

struct ObjectCall {
    int calls = 0;
    SLuint32 event = 0, param = 0;
    SLresult result = 99;
};

void count_object_call(SLObjectItf, const void *context, SLuint32 event, SLresult result, SLuint32 param, void *) {
    auto &call = *static_cast<ObjectCall *>(const_cast<void *>(context));
    ++call.calls;
    call.event = event;
    call.result = result;
    call.param = param;
}

TEST(an_asynchronous_realize_realizes_at_once_and_calls_back) {
    Engine engine;
    if (!engine.ok()) return;
    SLObjectItf player = nullptr;
    CHECK(create(engine, {}, &player) == SL_RESULT_SUCCESS);
    if (player == nullptr) return;
    ObjectCall call;
    CHECK((*player)->RegisterCallback(player, count_object_call, &call) == SL_RESULT_SUCCESS);
    CHECK((*player)->Realize(player, SL_BOOLEAN_TRUE) == SL_RESULT_SUCCESS);
    SLuint32 state = 0;
    CHECK((*player)->GetState(player, &state) == SL_RESULT_SUCCESS && state == SL_OBJECT_STATE_REALIZED);
    CHECK(call.calls == 1 && call.event == SL_OBJECT_EVENT_ASYNC_TERMINATION && call.result == SL_RESULT_SUCCESS &&
          call.param == SL_OBJECT_STATE_REALIZED);
    (*player)->Destroy(player);
}

TEST(what_minato_leaves_out_answers_feature_unsupported_and_logs_once) {
    Engine engine;
    if (!engine.ok()) return;
    const int before = log_lines("libOpenSLES");
    SLObjectItf player = nullptr;
    const Spec playback_rate{.ids = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_PLAYBACKRATE}};
    CHECK(create(engine, playback_rate, &player) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK(create(engine, playback_rate, &player) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK(log_lines("libOpenSLES") == before + 1);
    SLObjectItf recorder = nullptr;
    SLDataLocator_IODevice device{SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT, SL_DEFAULTDEVICEID_AUDIOINPUT,
                                  nullptr};
    SLDataSource microphone{&device, nullptr};
    SLDataLocator_AndroidSimpleBufferQueue queue{kQueue, 2};
    SLDataFormat_PCM format = pcm(1, 16, 16);
    SLDataSink sink{&queue, &format};
    CHECK((*engine.engine)->CreateAudioRecorder(engine.engine, &recorder, &microphone, &sink, 0, nullptr, nullptr) ==
          SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK(recorder == nullptr);
    CHECK(log_lines("libOpenSLES") == before + 2);
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
