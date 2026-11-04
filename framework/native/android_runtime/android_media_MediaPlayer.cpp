// Ours in place of AOSP's media/jni/android_media_MediaPlayer.cpp and libmedia's mediaplayer.cpp (ADR-0060 e2e1):
// MediaPlayer's natives over the runtime's media players (guest_interface.h's media_player), which the host plays
// (AVPlayer), instead of the media server's NuPlayer over binder.
// - MediaPlayer below is mediaplayer.cpp of android-15.0.0_r1, its states, checks and answers as the tag has them, and
//   the natives are the tag's JNI, process_media_player_call's exceptions and error callbacks included: an answer
//   without an exception to throw is posted as MEDIA_ERROR, which a player without a source gets only while idle.
// - Player stands in for the media server's player (IMediaPlayer, NuPlayerDriver): its states, its cached duration,
//   position and playback rate, isPlaying while running and not at the end, a start at the end beginning again, a
//   stop that a prepare follows with a seek to 0, as NuPlayerDriver's. Its calls are media_player's operations; its
//   events come as records on the runtime's pipe and on one of its own (the media server's asynchronous
//   notifications: started, paused, stopped, a refused source's error), which a thread of the player's reads and
//   posts through MediaPlayer::notify to Java's postEventFromNative, as the media server's binder thread does.
// - Sources as the media server takes them: a descriptor checked against its file's size, its length clamped to it;
//   a MediaDataSource read on the player's thread when the host asks for bytes (READ); an http or https URL with the
//   app's INTERNET (checked here as MediaPlayerService checks it, and by the runtime again: setDataSource's
//   UNKNOWN_ERROR without it [verified: media-player-test-offline]), and cleartext http only as the app's network
//   security config permits, else the prepare fails as Android's MediaHTTPConnection makes it fail
//   (MEDIA_ERROR_UNKNOWN, UNKNOWN_ERROR) without the host ever loading it [verified: media-player-test]. Other schemes
//   (rtsp, the rtp parameters) fail at setDataSource.
// - Frames: the host's frames (FRAME records) are drawn into the surface of setVideoSurface, a queue's free slot (an
//   ImageReader's, a SurfaceTexture's; none free: the frame dropped, as a late one, never waiting for the consumer) at
//   the frame's time or a host window (a SurfaceView's), else dropped; the
//   player claims the window as its producer, as NuPlayer connects to it. The first frame drawn after a start is
//   MEDIA_INFO_RENDERING_START.
// - Tracks (invoke's track info): the host's, in libmedia's parcel; the selected video and audio tracks are the first
//   of each, which a select keeps and a deselect refuses (INVALID_OPERATION), as NuPlayer's GenericSource
//   [verified: media-player-test]. Metrics: NuPlayerDriver's key, the uid, the data source's kind, the duration and
//   the time played.
// - Named limits: no DRM (a device without a DRM scheme: no DRM info, prepareDrm refused), no volume shapers
//   (INVALID_OPERATION), no audio routing (no routed device), no next player's gapless start beyond a start of it at
//   the end, playback rates from 1/8 to 8 (BAD_VALUE outside), no metadata (getMetadata false).
#define LOG_TAG "MediaPlayer-JNI"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <APKRuntime/buffer_queue.h>
#include <APKRuntime/guest_interface.h>
#include <log/log.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include "core_jni_helpers.h"
#include "minato_parcel.h"
#include "vorbis_wav.h"

// android_view_Surface.cpp's: a Java Surface's window, acquired for the caller.
extern "C" ANativeWindow *minato_ANativeWindow_fromSurface(JNIEnv *env, jobject surface);

namespace android {

Parcel *parcelForJavaObject(JNIEnv *env, jobject obj);

namespace {

// mediaplayer.h's messages, errors and infos.
enum {
    MEDIA_NOP = 0,
    MEDIA_PREPARED = 1,
    MEDIA_PLAYBACK_COMPLETE = 2,
    MEDIA_SEEK_COMPLETE = 4,
    MEDIA_SET_VIDEO_SIZE = 5,
    MEDIA_STARTED = 6,
    MEDIA_PAUSED = 7,
    MEDIA_STOPPED = 8,
    MEDIA_NOTIFY_TIME = 98,
    MEDIA_ERROR = 100,
    MEDIA_INFO = 200,
};
constexpr int MEDIA_ERROR_UNKNOWN = 1;
constexpr int MEDIA_INFO_STARTED_AS_NEXT = 2, MEDIA_INFO_RENDERING_START = 3;

// mediaplayer.h's states.
enum media_player_states {
    MEDIA_PLAYER_STATE_ERROR = 0,
    MEDIA_PLAYER_IDLE = 1 << 0,
    MEDIA_PLAYER_INITIALIZED = 1 << 1,
    MEDIA_PLAYER_PREPARING = 1 << 2,
    MEDIA_PLAYER_PREPARED = 1 << 3,
    MEDIA_PLAYER_STARTED = 1 << 4,
    MEDIA_PLAYER_PAUSED = 1 << 5,
    MEDIA_PLAYER_STOPPED = 1 << 6,
    MEDIA_PLAYER_PLAYBACK_COMPLETE = 1 << 7,
};

// MediaPlayer.java's invoke IDs and TrackInfo's types.
enum {
    INVOKE_ID_GET_TRACK_INFO = 1,
    INVOKE_ID_ADD_EXTERNAL_SOURCE = 2,
    INVOKE_ID_ADD_EXTERNAL_SOURCE_FD = 3,
    INVOKE_ID_SELECT_TRACK = 4,
    INVOKE_ID_DESELECT_TRACK = 5,
    INVOKE_ID_SET_VIDEO_SCALE_MODE = 6,
    INVOKE_ID_GET_SELECTED_TRACK = 7,
    INVOKE_ID_SET_PLAYER_IID = 8,
};
constexpr int MEDIA_TRACK_TYPE_VIDEO = 1, MEDIA_TRACK_TYPE_AUDIO = 2;

constexpr int KEY_PARAMETER_AUDIO_ATTRIBUTES = 1400;
constexpr status_t ERROR_UNSUPPORTED = -1010;  // MediaErrors.h
constexpr int kAudioStreamMusic = 3;           // AUDIO_STREAM_MUSIC
constexpr int kSeekPreviousSync = 0, kSeekClosest = 3;

struct {
    jfieldID context;  // MediaPlayer.mNativeContext
    jmethodID postEvent;
    jclass playbackParamsClass, syncParamsClass, bundleClass;  // global
    jmethodID playbackParamsConstructor, syncParamsConstructor, bundleConstructor;
    jfieldID playbackSet, playbackFallback, playbackStretch, playbackPitch, playbackSpeed;
    jfieldID syncSet, syncAdjust, syncSource, syncTolerance, syncFrameRate;
    jmethodID putString, putInt, putLong;
    jclass dataSourceClass;  // MediaDataSource, global
    jmethodID readAt, getSize, close;
} gFields;

/// AudioPlaybackRate, as libmedia keeps it.
struct PlaybackRate {
    float speed = 1, pitch = 1;
    int fallbackMode = 0, stretchMode = 0;
};

/// AVSyncSettings and the video's frame rate hint, as NuPlayer keeps them.
struct SyncSettings {
    int source = 0, adjustMode = 0;
    float tolerance = 0.044f, frameRate = -1;
};

/// media_player's call, with EINTR retried: 0, or the negative errno.
long playerCall(apkrt_media_player_op op, apkrt_media_player *argument) {
    long result;
    do {
        result = syscall(APKRT_NR_MEDIA_PLAYER, op, argument);
    } while (result == -1 && errno == EINTR);
    return result == -1 ? -errno : result;
}

int64_t monotonicNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000LL + now.tv_nsec;
}

/// libnativewindow's private functions for a surface, looked up once.
struct WindowFunctions {
    decltype(&minato_ANativeWindow_isQueue) isQueue = nullptr;
    decltype(&minato_ANativeWindow_tryDequeue) dequeue = nullptr;
    decltype(&minato_ANativeWindow_queueWith) queue = nullptr;
    decltype(&minato_ANativeWindow_cancel) cancel = nullptr;
    decltype(&minato_ANativeWindow_claim) claim = nullptr;
    decltype(&minato_ANativeWindow_unclaim) unclaim = nullptr;
    decltype(&minato_ANativeWindow_getInfo) info = nullptr;
};

const WindowFunctions *windowFunctions() {
    static const WindowFunctions functions = [] {
        WindowFunctions found;
        if (void *library = dlopen("libnativewindow.so", RTLD_NOW)) {
#define FIND(field, name) found.field = reinterpret_cast<decltype(found.field)>(dlsym(library, #name))
            FIND(isQueue, minato_ANativeWindow_isQueue);
            FIND(dequeue, minato_ANativeWindow_tryDequeue);
            FIND(queue, minato_ANativeWindow_queueWith);
            FIND(cancel, minato_ANativeWindow_cancel);
            FIND(claim, minato_ANativeWindow_claim);
            FIND(unclaim, minato_ANativeWindow_unclaim);
            FIND(info, minato_ANativeWindow_getInfo);
#undef FIND
        }
        return found;
    }();
    const WindowFunctions &found = functions;
    return found.isQueue != nullptr && found.dequeue != nullptr && found.queue != nullptr &&
                   found.cancel != nullptr && found.claim != nullptr && found.unclaim != nullptr &&
                   found.info != nullptr
               ? &found
               : nullptr;
}

/// A whole microsecond time in milliseconds, rounded as NuPlayerDriver's divRound.
int roundedMs(int64_t us) {
    return static_cast<int>(us >= 0 ? (us + 500) / 1000 : (us - 500) / 1000);
}

class MediaPlayer;

/// What a player's records and notifications go to.
class Listener {
public:
    virtual ~Listener() = default;
    virtual void notifyFrom(const void *player, int msg, int ext1, int ext2, const Parcel *obj) = 0;
};

/// A track as the host gave it.
struct Track {
    int type;
    std::string mime, language;
};

// The media server's player for one source: media_player's player, or none for a source refused before the host
// (cleartext http the app does not permit), and NuPlayerDriver's states and caches.
class Player : public std::enable_shared_from_this<Player> {
public:
    enum class Kind { fd, url, callback, refused };

    Player(JavaVM *vm, std::weak_ptr<Listener> listener) : mVm(vm), mListener(std::move(listener)) {}
    ~Player() {
        if (mLocal[1] >= 0) close(mLocal[1]);
        if (mEvents >= 0) close(mEvents);
        if (mLocal[0] >= 0) close(mLocal[0]);
        if (mExtractFd >= 0) close(mExtractFd);
    }

    /// The runtime's player (none for `kind` refused) and the thread that reads its records.
    status_t create(Kind kind) {
        mKind = kind;
        if (pipe2(mLocal, O_CLOEXEC | O_NONBLOCK) != 0) return NO_MEMORY;
        if (kind != Kind::refused) {
            apkrt_media_player call{};
            if (playerCall(APKRT_MEDIA_PLAYER_CREATE, &call) != 0) return UNKNOWN_ERROR;
            mId = call.id;
            mEvents = call.events;
        }
        mThread = std::thread([self = shared_from_this()] { self->read(); });
        mThread.detach();  // it ends at its pipes' ends, holding the player until then
        return OK;
    }

    // setDataSource's: MediaPlayerService::Client's checks, then the runtime's.
    status_t setDataSource(int fd, int64_t offset, int64_t length) {
        struct stat status{};
        if (fstat(fd, &status) != 0 || offset < 0 || offset >= status.st_size) return UNKNOWN_ERROR;
        if (length < 0 || length > status.st_size - offset) length = status.st_size - offset;
        apkrt_media_player call{};
        call.id = mId;
        call.kind = APKRT_MEDIA_PLAYER_FD;
        call.fd = fd;
        call.offset = offset;
        call.length = length;
        mSourceName = "GenFD";
        if ((mVorbis = VorbisWav::open(fd, offset, length)) != nullptr) {
            // Ogg Vorbis, which the host does not read: a WAV of its samples, which the host reads from this player
            // (READ), and OggExtractor's track and duration.
            call.kind = APKRT_MEDIA_PLAYER_CALLBACK;
            call.fd = -1;
            call.offset = 0;
            call.length = mVorbis->size();
            return playerCall(APKRT_MEDIA_PLAYER_SOURCE, &call) == 0 ? OK : UNKNOWN_ERROR;
        }
        mExtractFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);  // the extractor's, as GenericSource keeps its own
        mExtractOffset = offset;
        mExtractLength = length;
        return playerCall(APKRT_MEDIA_PLAYER_SOURCE, &call) == 0 ? OK : UNKNOWN_ERROR;
    }

    status_t setDataSource(const std::string &url) {
        apkrt_media_player call{};
        call.id = mId;
        call.kind = APKRT_MEDIA_PLAYER_URL;
        call.data = reinterpret_cast<uintptr_t>(url.data());
        call.size = url.size();
        mSourceName = url.find(".m3u8") != std::string::npos ? "HTTPLive" : "GenHTTP";
        return playerCall(APKRT_MEDIA_PLAYER_SOURCE, &call) == 0 ? OK : UNKNOWN_ERROR;
    }

    status_t setDataSource(JNIEnv *env, jobject dataSource) {
        const jlong size = env->CallLongMethod(dataSource, gFields.getSize);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return UNKNOWN_ERROR;
        }
        mDataSource = env->NewGlobalRef(dataSource);
        mDataSourceSize = size;
        apkrt_media_player call{};
        call.id = mId;
        call.kind = APKRT_MEDIA_PLAYER_CALLBACK;
        call.length = size < 0 ? -1 : size;
        mSourceName = "Media";
        return playerCall(APKRT_MEDIA_PLAYER_SOURCE, &call) == 0 ? OK : UNKNOWN_ERROR;
    }

    status_t prepareAsync() {
        std::lock_guard hold(mMutex);
        if (mState == State::stopped) {  // NuPlayerDriver's STATE_STOPPED_AND_PREPARING: a seek to 0, then prepared
            mState = State::stoppedPreparing;
            mPositionUs = 0;
            return seekLocked(0, kSeekPreviousSync, Seek::prepares);
        }
        if (mState != State::idle) return INVALID_OPERATION;
        mState = State::preparing;
        if (mKind == Kind::refused) {
            // Android's MediaHTTPConnection refuses the cleartext connection: the prepare fails.
            post(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, UNKNOWN_ERROR);
            return OK;
        }
        apkrt_media_player call{};
        call.id = mId;
        return playerCall(APKRT_MEDIA_PLAYER_PREPARE, &call) == 0 ? OK : UNKNOWN_ERROR;
    }

    status_t start() {
        std::lock_guard hold(mMutex);
        switch (mState) {
            case State::prepared:
            case State::paused:
            case State::stoppedPrepared: break;
            case State::running:
                if (!mAtEnd) return OK;
                break;
            default: return INVALID_OPERATION;
        }
        if (mAtEnd) {  // NuPlayerDriver's start_l: from the beginning, without a seek complete
            mAtEnd = false;
            mPositionUs = -1;
            if (status_t sought = seekLocked(0, kSeekPreviousSync, Seek::quiet); sought != OK) return sought;
        }
        if (simple(APKRT_MEDIA_PLAYER_START) != OK) return UNKNOWN_ERROR;
        mState = State::running;
        mRenderingStarted = false;
        mPlayingSinceNs = monotonicNs();
        post(MEDIA_STARTED, 0, 0);
        return OK;
    }

    status_t stop() {
        std::lock_guard hold(mMutex);
        if (mState == State::stopped) return OK;
        if (mState != State::running && mState != State::paused && mState != State::prepared &&
            mState != State::stoppedPrepared) {
            return INVALID_OPERATION;
        }
        refreshPosition();
        simple(APKRT_MEDIA_PLAYER_PAUSE);
        countPlaying();
        mState = State::stopped;
        post(MEDIA_STOPPED, 0, 0);
        return OK;
    }

    status_t pause() {
        std::lock_guard hold(mMutex);
        if (mState == State::paused || mState == State::prepared || mState == State::stoppedPrepared) return OK;
        if (mState != State::running) return INVALID_OPERATION;
        refreshPosition();
        if (simple(APKRT_MEDIA_PLAYER_PAUSE) != OK) return UNKNOWN_ERROR;
        countPlaying();
        mState = State::paused;
        post(MEDIA_PAUSED, 0, 0);
        return OK;
    }

    bool isPlaying() {
        std::lock_guard hold(mMutex);
        return mState == State::running && !mAtEnd;
    }

    status_t seekTo(int msec, int mode) {
        std::lock_guard hold(mMutex);
        switch (mState) {
            case State::prepared:
            case State::stoppedPrepared:
            case State::paused:
            case State::running: break;
            default: return INVALID_OPERATION;
        }
        mAtEnd = false;
        mPositionUs = static_cast<int64_t>(msec) * 1000;
        post(MEDIA_PAUSED, 0, 0);  // NuPlayerDriver: seeks can take a while, so we essentially paused
        return seekLocked(mPositionUs, mode, Seek::notifies);
    }

    status_t getCurrentPosition(int *msec) {
        std::lock_guard hold(mMutex);
        // NuPlayerDriver's: the seek's time while it runs, while paused and before a start (no renderer yet); at the
        // end the renderer's last, the duration.
        if (mSeeking > 0 || mState != State::running || mAtEnd || mKind == Kind::refused) {
            *msec = roundedMs(mPositionUs <= 0 ? 0 : mPositionUs);
            return OK;
        }
        refreshPosition();
        *msec = roundedMs(mPositionUs <= 0 ? 0 : mPositionUs);
        return OK;
    }

    status_t getDuration(int *msec) {
        std::lock_guard hold(mMutex);
        if (mDurationUs < 0) return UNKNOWN_ERROR;
        *msec = static_cast<int>((mDurationUs + 500) / 1000);
        return OK;
    }

    status_t reset() {
        std::lock_guard hold(mMutex);
        countPlaying();
        mState = State::idle;
        return OK;
    }

    status_t setLooping(int loop) {
        std::lock_guard hold(mMutex);
        return set(APKRT_MEDIA_PLAYER_LOOPING, loop != 0 ? 1 : 0);
    }

    status_t setVolume(float left, float right) {
        if (!(left >= 0 && left <= 1 && right >= 0 && right <= 1)) return BAD_VALUE;
        uint32_t low = 0, high = 0;
        memcpy(&low, &left, sizeof low);
        memcpy(&high, &right, sizeof high);
        std::lock_guard hold(mMutex);
        return set(APKRT_MEDIA_PLAYER_VOLUME, uint64_t{low} | uint64_t{high} << 32);
    }

    status_t setPlaybackSettings(const PlaybackRate &rate) {
        if (rate.speed != 0 && (rate.speed < 0.125f || rate.speed > 8)) return BAD_VALUE;
        std::lock_guard hold(mMutex);
        if (rate.speed != 0) {
            uint32_t bits = 0;
            memcpy(&bits, &rate.speed, sizeof bits);
            if (status_t answer = set(APKRT_MEDIA_PLAYER_SPEED, bits); answer != OK) return answer;
        }
        mRate = rate;
        // NuPlayerDriver's: a speed of 0 pauses, any other starts a prepared or paused player.
        if (rate.speed == 0 && mState == State::running) {
            refreshPosition();
            simple(APKRT_MEDIA_PLAYER_PAUSE);
            countPlaying();
            mState = State::paused;
            post(MEDIA_PAUSED, 0, 0);
        } else if (rate.speed != 0 && (mState == State::paused || mState == State::prepared ||
                                       mState == State::stoppedPrepared)) {
            if (mAtEnd) {
                mAtEnd = false;
                seekLocked(0, kSeekPreviousSync, Seek::quiet);
            }
            if (simple(APKRT_MEDIA_PLAYER_START) != OK) return UNKNOWN_ERROR;
            mState = State::running;
            mPlayingSinceNs = monotonicNs();
            post(MEDIA_STARTED, 0, 0);
        }
        return OK;
    }

    status_t getPlaybackSettings(PlaybackRate *rate) {
        std::lock_guard hold(mMutex);
        *rate = mRate;
        return OK;
    }

    status_t setSyncSettings(const SyncSettings &sync) {
        std::lock_guard hold(mMutex);
        mSync = sync;
        return OK;
    }

    status_t getSyncSettings(SyncSettings *sync) {
        std::lock_guard hold(mMutex);
        *sync = mSync;
        return OK;
    }

    status_t invoke(const Parcel &request, Parcel *reply) {
        int32_t method = 0;
        if (request.readInt32(&method) != OK) return BAD_VALUE;
        std::lock_guard hold(mMutex);
        switch (method) {
            case INVOKE_ID_GET_TRACK_INFO: {
                // GenericSource's getTrackInfo: each track's type, mime and language, an audio track's haptic flag.
                reply->writeInt32(static_cast<int32_t>(mTracks.size()));
                for (const Track &track : mTracks) {
                    reply->writeInt32(2);  // something non-zero
                    reply->writeInt32(track.type);
                    writeString16(reply, track.mime);
                    writeString16(reply, track.language);
                    if (track.type == MEDIA_TRACK_TYPE_AUDIO) reply->writeInt32(0);  // no haptic channels
                }
                return OK;
            }
            case INVOKE_ID_SELECT_TRACK:
            case INVOKE_ID_DESELECT_TRACK: {
                int32_t index = 0;
                if (request.readInt32(&index) != OK) return BAD_VALUE;
                if (index < 0 || static_cast<size_t>(index) >= mTracks.size()) return BAD_INDEX;
                const bool selected = index == selectedTrack(mTracks[index].type);
                if (method == INVOKE_ID_DESELECT_TRACK || !selected) return INVALID_OPERATION;
                return OK;
            }
            case INVOKE_ID_GET_SELECTED_TRACK: {
                int32_t type = 0;
                if (request.readInt32(&type) != OK) return BAD_VALUE;
                reply->writeInt32(selectedTrack(type));
                return OK;
            }
            case INVOKE_ID_SET_VIDEO_SCALE_MODE: {
                int32_t mode = 0;
                if (request.readInt32(&mode) != OK) return BAD_VALUE;
                if (mode != 1 && mode != 2) return BAD_VALUE;
                mScaling = mode == 2 ? APKRT_VIDEO_DECODER_CROP : 0;
                return OK;
            }
            case INVOKE_ID_SET_PLAYER_IID: return OK;
            case INVOKE_ID_ADD_EXTERNAL_SOURCE:
            case INVOKE_ID_ADD_EXTERNAL_SOURCE_FD: return ERROR_UNSUPPORTED;
            default: return INVALID_OPERATION;
        }
    }

    /// The surface the frames are drawn into, claimed as its producer; null for none.
    void setVideoSurface(ANativeWindow *window) {
        const WindowFunctions *functions = windowFunctions();
        std::lock_guard hold(mWindowMutex);
        if (mWindow != nullptr) {
            if (functions != nullptr) functions->unclaim(mWindow);
            ANativeWindow_release(mWindow);
        }
        mWindow = nullptr;
        mHostWindow = 0;
        if (window == nullptr || functions == nullptr) return;
        if (functions->claim(window) != 0) {
            ALOGW("setVideoSurface: the surface has a producer already, no frames are drawn");
            return;
        }
        ANativeWindow_acquire(window);
        mWindow = window;
        if (functions->isQueue(window) == 0) {
            minato_window_info info{};
            if (functions->info(window, &info) == 0) mHostWindow = info.id;
        }
    }

    status_t notifyAt(int64_t mediaTimeUs) {
        std::lock_guard hold(mMutex);
        mNotifyAtUs = mediaTimeUs;
        return OK;
    }

    jobject metrics(JNIEnv *env) {
        std::string source;
        int64_t durationMs = 0, playingMs = 0;
        {
            std::lock_guard hold(mMutex);
            source = mSourceName;
            durationMs = mDurationUs < 0 ? 0 : (mDurationUs + 500) / 1000;
            playingMs = (mPlayedNs + (mState == State::running && !mAtEnd ? monotonicNs() - mPlayingSinceNs : 0)) /
                        1000000;
        }
        jobject bundle = env->NewObject(gFields.bundleClass, gFields.bundleConstructor);
        if (bundle == nullptr) return nullptr;
        const auto putString = [&](const char *key, const char *value) {
            jstring name = env->NewStringUTF(key), text = env->NewStringUTF(value);
            env->CallVoidMethod(bundle, gFields.putString, name, text);
            env->DeleteLocalRef(name);
            env->DeleteLocalRef(text);
        };
        const auto putLong = [&](const char *key, int64_t value) {
            jstring name = env->NewStringUTF(key);
            env->CallVoidMethod(bundle, gFields.putLong, name, static_cast<jlong>(value));
            env->DeleteLocalRef(name);
        };
        putString("_key", "nuplayer");
        jstring uid = env->NewStringUTF("_uid");
        env->CallVoidMethod(bundle, gFields.putInt, uid, static_cast<jint>(getuid()));
        env->DeleteLocalRef(uid);
        putString("android.media.mediaplayer.dataSource", source.c_str());
        putLong("android.media.mediaplayer.durationMs", durationMs);
        putLong("android.media.mediaplayer.playingMs", playingMs);
        return bundle;
    }

    /// The media server's disconnect: the runtime's player goes, the thread ends at its pipes' ends, no more records
    /// reach the listener, and a MediaDataSource is closed.
    void disconnect(JNIEnv *env) {
        {
            std::lock_guard hold(mMutex);
            mConnected = false;
            countPlaying();
        }
        setVideoSurface(nullptr);
        if (mId != 0) {
            apkrt_media_player call{};
            call.id = mId;
            playerCall(APKRT_MEDIA_PLAYER_DESTROY, &call);
        }
        // Closed while the thread, which lets go of it at its pipes' ends, still holds it: before the local end.
        if (mDataSource != nullptr && env != nullptr) {
            env->CallVoidMethod(mDataSource, gFields.close);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        int write = -1;
        {
            std::lock_guard hold(mMutex);
            write = mLocal[1];
            mLocal[1] = -1;
        }
        if (write >= 0) close(write);
    }

private:
    enum class State { idle, preparing, prepared, running, paused, stopped, stoppedPreparing, stoppedPrepared };
    enum class Seek { notifies, quiet, prepares };

    static void writeString16(Parcel *parcel, const std::string &text) {
        std::u16string wide(text.begin(), text.end());  // ASCII: MIME types and ISO 639-2 codes
        parcel->writeString16(wide.data(), wide.size());
    }

    int selectedTrack(int type) const {
        if (type != MEDIA_TRACK_TYPE_VIDEO && type != MEDIA_TRACK_TYPE_AUDIO) return -1;
        for (size_t index = 0; index < mTracks.size(); index++) {
            if (mTracks[index].type == type) return static_cast<int>(index);
        }
        return -1;
    }

    status_t simple(apkrt_media_player_op op) {
        apkrt_media_player call{};
        call.id = mId;
        return mKind == Kind::refused || playerCall(op, &call) == 0 ? OK : UNKNOWN_ERROR;
    }

    status_t set(uint32_t key, uint64_t value) {
        if (mKind == Kind::refused) return OK;
        apkrt_media_player call{};
        call.id = mId;
        call.kind = key;
        call.value = value;
        const long answer = playerCall(APKRT_MEDIA_PLAYER_SET, &call);
        return answer == 0 ? OK : answer == -EINVAL ? BAD_VALUE : UNKNOWN_ERROR;
    }

    status_t seekLocked(int64_t timeUs, int mode, Seek kind) {
        if (mKind == Kind::refused) return INVALID_OPERATION;
        apkrt_media_player call{};
        call.id = mId;
        call.time_us = timeUs < 0 ? 0 : timeUs;
        call.value = static_cast<uint64_t>(mode < kSeekPreviousSync || mode > kSeekClosest ? kSeekPreviousSync : mode);
        if (playerCall(APKRT_MEDIA_PLAYER_SEEK, &call) != 0) return UNKNOWN_ERROR;
        mSeeks.push_back(kind);
        mSeeking++;
        mRenderingStarted = false;
        return OK;
    }

    /// The host's position, kept for the answers while paused, as NuPlayerDriver keeps the renderer's.
    void refreshPosition() {
        if (mKind == Kind::refused) return;
        apkrt_media_player call{};
        call.id = mId;
        if (playerCall(APKRT_MEDIA_PLAYER_STATUS, &call) == 0) mPositionUs = call.time_us;
    }

    void countPlaying() {
        if (mState == State::running && mPlayingSinceNs != 0) mPlayedNs += monotonicNs() - mPlayingSinceNs;
        mPlayingSinceNs = 0;
    }

    /// A notification of the player's own, through its pipe as the host's records come, so that it reaches the
    /// listener after the call that made it, on the player's thread.
    void post(int msg, int ext1, int ext2) {
        const int32_t record[3] = {msg, ext1, ext2};
        if (mLocal[1] >= 0 && write(mLocal[1], record, sizeof record) != sizeof record) {
            ALOGW("a notification %d of the player's is lost", msg);
        }
    }

    void notify(int msg, int ext1 = 0, int ext2 = 0) {
        if (auto listener = mListener.lock()) listener->notifyFrom(this, msg, ext1, ext2, nullptr);
    }

    /// The player's thread: the runtime's records and the player's own, until both pipes end.
    void read() {
        JNIEnv *env = nullptr;
        JavaVMAttachArgs arguments{JNI_VERSION_1_6, "MediaPlayer", nullptr};
        if (mVm->AttachCurrentThread(&env, &arguments) != JNI_OK) env = nullptr;
        bool runtimeOpen = mEvents >= 0, localOpen = true;
        while (runtimeOpen || localOpen) {
            pollfd fds[2] = {{runtimeOpen ? mEvents : -1, POLLIN, 0}, {localOpen ? mLocal[0] : -1, POLLIN, 0}};
            const int timeout = waitsForTime() ? 50 : -1;
            const int ready = poll(fds, 2, timeout);
            if (ready < 0 && errno != EINTR) break;
            if (fds[0].revents != 0) runtimeOpen = readRuntime(env);
            if (fds[1].revents != 0) localOpen = readLocal();
            checkTime();
        }
        if (env != nullptr) {
            if (mDataSource != nullptr) env->DeleteGlobalRef(mDataSource);
            mDataSource = nullptr;
            mVm->DetachCurrentThread();
        }
    }

    bool connected() {
        std::lock_guard hold(mMutex);
        return mConnected;
    }

    /// The runtime's records: false at the pipe's end.
    bool readRuntime(JNIEnv *env) {
        for (;;) {
            apkrt_media_player_event event{};
            const ssize_t got = ::read(mEvents, &event, sizeof event);
            if (got == 0) return false;
            if (got < 0) return errno == EAGAIN || errno == EINTR;
            if (got != sizeof event) return false;
            handle(env, event);
        }
    }

    bool readLocal() {
        for (;;) {
            int32_t record[3] = {};
            const ssize_t got = ::read(mLocal[0], record, sizeof record);
            if (got == 0) return false;
            if (got < 0) return errno == EAGAIN || errno == EINTR;
            if (got != sizeof record) return false;
            if (record[0] == MEDIA_ERROR) {
                std::lock_guard hold(mMutex);
                if (mState == State::preparing) mState = State::idle;
            }
            if (connected()) notify(record[0], record[1], record[2]);
        }
    }

    void handle(JNIEnv *env, const apkrt_media_player_event &event) {
        switch (event.type) {
            case APKRT_MEDIA_PLAYER_PREPARED: {
                std::vector<Track> tracks;
                int64_t durationUs = event.value;
                if (mVorbis != nullptr) {
                    tracks.push_back({MEDIA_TRACK_TYPE_AUDIO, "audio/vorbis", "und"});
                    durationUs = mVorbis->durationUs();
                } else if (!extract(env, &tracks, &durationUs)) {
                    tracks = fetchTracks();
                }
                {
                    std::lock_guard hold(mMutex);
                    mDurationUs = durationUs;
                    mTracks = std::move(tracks);
                    if (mState == State::preparing) mState = State::prepared;
                    mPositionUs = 0;
                }
                if (!connected()) return;
                notify(MEDIA_SET_VIDEO_SIZE, event.ext1, event.ext2);  // NuPlayer's, 0 by 0 without a video
                notify(MEDIA_PREPARED);
                return;
            }
            case APKRT_MEDIA_PLAYER_ERROR: {
                {
                    std::lock_guard hold(mMutex);
                    countPlaying();
                    if (mState == State::preparing) mState = State::idle;
                }
                if (connected()) notify(MEDIA_ERROR, event.ext1, event.ext2);
                return;
            }
            case APKRT_MEDIA_PLAYER_SEEK_DONE: {
                Seek kind = Seek::quiet;
                {
                    std::lock_guard hold(mMutex);
                    if (!mSeeks.empty()) {
                        kind = mSeeks.front();
                        mSeeks.erase(mSeeks.begin());
                    }
                    if (mSeeking > 0) mSeeking--;
                    if (kind == Seek::prepares && mState == State::stoppedPreparing) mState = State::stoppedPrepared;
                }
                if (!connected() || kind == Seek::quiet) return;
                notify(kind == Seek::prepares ? MEDIA_PREPARED : MEDIA_SEEK_COMPLETE);
                return;
            }
            case APKRT_MEDIA_PLAYER_COMPLETED: {
                {
                    std::lock_guard hold(mMutex);
                    countPlaying();
                    mAtEnd = true;
                    if (mDurationUs >= 0) mPositionUs = mDurationUs;
                }
                if (connected()) notify(MEDIA_PLAYBACK_COMPLETE);
                return;
            }
            case APKRT_MEDIA_PLAYER_VIDEO_SIZE:
                if (connected()) notify(MEDIA_SET_VIDEO_SIZE, event.ext1, event.ext2);
                return;
            case APKRT_MEDIA_PLAYER_FRAME: drawFrame(event); return;
            case APKRT_MEDIA_PLAYER_READ: answerRead(env, event); return;
            default: return;
        }
    }

    /// A JMediaDataSource's read for the extractor, on the player's thread.
    struct JavaSource {
        JNIEnv *env;
        jobject source;
        jlong size;
    };

    static ssize_t javaReadAt(void *userdata, off64_t offset, void *buffer, size_t size) {
        auto *from = static_cast<JavaSource *>(userdata);
        const jint count = static_cast<jint>(size < 65536 ? size : 65536);  // JMediaDataSource's 64 KiB
        if (count == 0) return 0;
        JNIEnv *env = from->env;
        jbyteArray array = env->NewByteArray(count);
        if (array == nullptr) {
            env->ExceptionClear();
            return -1;
        }
        jint got = env->CallIntMethod(from->source, gFields.readAt, static_cast<jlong>(offset), array, 0, count);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            got = -1;
        }
        if (got > count) got = -1;
        if (got > 0) env->GetByteArrayRegion(array, 0, got, static_cast<jbyte *>(buffer));
        env->DeleteLocalRef(array);
        return got;
    }

    static ssize_t javaGetSize(void *userdata) {
        return static_cast<ssize_t>(static_cast<JavaSource *>(userdata)->size);
    }

    /// The tracks and the duration of an fd or callback source as GenericSource has them from its extractor (each
    /// track's MIME type and language, "und" without one; the longest track's duration), when libmediandk's extractor
    /// reads it: whether it did. A URL's, or a format it does not read, keeps the host's.
    bool extract(JNIEnv *env, std::vector<Track> *tracks, int64_t *durationUs) {
        if (mExtractFd < 0 && (mDataSource == nullptr || env == nullptr)) return false;
        AMediaExtractor *extractor = AMediaExtractor_new();
        if (extractor == nullptr) return false;
        JavaSource java{env, mDataSource, mDataSourceSize};
        AMediaDataSource *custom = nullptr;
        media_status_t status = AMEDIA_ERROR_UNKNOWN;
        if (mExtractFd >= 0) {
            status = AMediaExtractor_setDataSourceFd(extractor, mExtractFd, mExtractOffset, mExtractLength);
        } else if ((custom = AMediaDataSource_new()) != nullptr) {
            AMediaDataSource_setUserdata(custom, &java);
            AMediaDataSource_setReadAt(custom, javaReadAt);
            AMediaDataSource_setGetSize(custom, javaGetSize);
            status = AMediaExtractor_setDataSourceCustom(extractor, custom);
        }
        bool read = false;
        if (status == AMEDIA_OK) {
            const size_t count = AMediaExtractor_getTrackCount(extractor);
            int64_t longest = -1;
            for (size_t index = 0; index < count; index++) {
                AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, index);
                const char *mime = nullptr, *language = nullptr;
                int64_t duration = -1;
                AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime);
                if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_LANGUAGE, &language)) language = "und";
                if (AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration) && duration > longest) {
                    longest = duration;
                }
                const std::string type = mime != nullptr ? mime : "";
                const int trackType = strncasecmp(type.c_str(), "video/", 6) == 0   ? MEDIA_TRACK_TYPE_VIDEO
                                      : strncasecmp(type.c_str(), "audio/", 6) == 0 ? MEDIA_TRACK_TYPE_AUDIO
                                      : strcasecmp(type.c_str(), "text/3gpp-tt") == 0 ? 3  // timed text
                                                                                      : 0;
                tracks->push_back({trackType, type, language});
                AMediaFormat_delete(format);
            }
            if (longest >= 0) *durationUs = longest;
            read = count > 0;
        }
        AMediaExtractor_delete(extractor);
        if (custom != nullptr) AMediaDataSource_delete(custom);
        if (mExtractFd >= 0) close(mExtractFd);
        mExtractFd = -1;
        if (!read) tracks->clear();
        return read;
    }

    std::vector<Track> fetchTracks() {
        std::vector<apkrt_media_player_track> got(8);
        apkrt_media_player call{};
        call.id = mId;
        for (int round = 0; round < 2; round++) {
            call.data = reinterpret_cast<uintptr_t>(got.data());
            call.size = got.size() * sizeof got[0];
            const long answer = playerCall(APKRT_MEDIA_PLAYER_TRACKS, &call);
            if (answer == -ENOSPC) {
                got.resize(call.size / sizeof got[0]);
                continue;
            }
            if (answer != 0) return {};
            std::vector<Track> tracks;
            for (size_t index = 0; index < call.size / sizeof got[0]; index++) {
                tracks.push_back({static_cast<int>(got[index].type), got[index].mime, got[index].language});
            }
            return tracks;
        }
        return {};
    }

    /// A host frame drawn into the surface, at its time, or dropped without one.
    void drawFrame(const apkrt_media_player_event &event) {
        apkrt_media_player call{};
        call.id = mId;
        call.token = event.token;
        bool drawn = false;
        {
            std::lock_guard hold(mWindowMutex);
            const WindowFunctions *functions = windowFunctions();
            if (mWindow != nullptr && mHostWindow != 0) {
                call.size = mHostWindow;
                call.time_us = event.value > 0 ? event.value : 0;
                call.flags = mScaling;
                drawn = playerCall(APKRT_MEDIA_PLAYER_RENDER_WINDOW, &call) == 0;
            } else if (mWindow != nullptr && functions != nullptr) {
                AHardwareBuffer *buffer = nullptr;
                int32_t slot = -1;
                uint64_t id = 0;
                if (functions->dequeue(mWindow, &buffer, &slot) == 0 && AHardwareBuffer_getId(buffer, &id) == 0 &&
                    id != 0) {
                    call.size = id;
                    if (playerCall(APKRT_MEDIA_PLAYER_RENDER, &call) == 0) {
                        minato_BufferQueue_settings settings{};
                        settings.timestamp = event.value * 1000;
                        settings.exact_time = 1;
                        functions->queue(mWindow, slot, &settings);
                        drawn = true;
                    } else {
                        functions->cancel(mWindow, slot);
                    }
                } else {
                    if (slot >= 0) functions->cancel(mWindow, slot);
                    playerCall(APKRT_MEDIA_PLAYER_DISCARD, &call);
                }
            } else {
                playerCall(APKRT_MEDIA_PLAYER_DISCARD, &call);
            }
        }
        bool first = false;
        {
            std::lock_guard hold(mMutex);
            if (drawn && !mRenderingStarted && mState == State::running) first = mRenderingStarted = true;
        }
        if (first && connected()) notify(MEDIA_INFO, MEDIA_INFO_RENDERING_START, 0);
    }

    /// A callback source's read, from the app's MediaDataSource on this thread; the runtime drops an answer that
    /// lost to a seek or the player's end (-EINVAL), as the media server drops a late one.
    void answerRead(JNIEnv *env, const apkrt_media_player_event &event) {
        apkrt_media_player call{};
        call.id = mId;
        call.token = event.token;
        std::vector<jbyte> bytes;
        if (mVorbis != nullptr && event.ext1 >= 0) {
            bytes.resize(static_cast<size_t>(event.ext1));
            const ssize_t got = mVorbis->read(event.value, reinterpret_cast<uint8_t *>(bytes.data()), bytes.size());
            if (got < 0) {
                call.flags = APKRT_MEDIA_PLAYER_READ_FAILED;
            } else {
                call.data = reinterpret_cast<uintptr_t>(bytes.data());
                call.size = static_cast<uint64_t>(got);
            }
        } else if (env == nullptr || mDataSource == nullptr || event.ext1 < 0) {
            call.flags = APKRT_MEDIA_PLAYER_READ_FAILED;
        } else if (event.ext1 > 0) {
            jbyteArray array = env->NewByteArray(event.ext1);
            jint got = -1;
            if (array != nullptr) {
                got = env->CallIntMethod(mDataSource, gFields.readAt, static_cast<jlong>(event.value), array, 0,
                                         event.ext1);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    got = -1;
                }
            } else {
                env->ExceptionClear();
            }
            if (got < 0) {
                call.flags = APKRT_MEDIA_PLAYER_READ_FAILED;
            } else {
                if (got > event.ext1) got = event.ext1;
                bytes.resize(static_cast<size_t>(got));
                if (got > 0) env->GetByteArrayRegion(array, 0, got, bytes.data());
                call.data = reinterpret_cast<uintptr_t>(bytes.data());
                call.size = static_cast<uint64_t>(got);
            }
            if (array != nullptr) env->DeleteLocalRef(array);
        }
        playerCall(APKRT_MEDIA_PLAYER_DATA, &call);
    }

    bool waitsForTime() {
        std::lock_guard hold(mMutex);
        return mNotifyAtUs >= 0 && mState == State::running;
    }

    /// notifyAt's time reached while running: MEDIA_NOTIFY_TIME, once.
    void checkTime() {
        bool reached = false;
        {
            std::lock_guard hold(mMutex);
            if (mNotifyAtUs < 0 || mState != State::running || mKind == Kind::refused) return;
            apkrt_media_player call{};
            call.id = mId;
            if (playerCall(APKRT_MEDIA_PLAYER_STATUS, &call) == 0 && call.time_us >= mNotifyAtUs) {
                reached = true;
                mNotifyAtUs = -1;
            }
        }
        if (reached && connected()) notify(MEDIA_NOTIFY_TIME);
    }

    JavaVM *const mVm;
    const std::weak_ptr<Listener> mListener;
    Kind mKind = Kind::fd;
    uint64_t mId = 0;
    int mEvents = -1;
    int mLocal[2] = {-1, -1};
    std::thread mThread;
    jobject mDataSource = nullptr;  // a callback source's MediaDataSource, global, until the thread ends
    jlong mDataSourceSize = -1;
    int mExtractFd = -1;            // an fd source's for the extractor, the player's own
    int64_t mExtractOffset = 0, mExtractLength = 0;
    std::unique_ptr<VorbisWav> mVorbis;  // an Ogg Vorbis source's WAV, read on the player's thread

    std::mutex mMutex;  // for the fields below
    bool mConnected = true;
    State mState = State::idle;
    bool mAtEnd = false;
    int64_t mDurationUs = -1, mPositionUs = -1;
    int mSeeking = 0;
    std::vector<Seek> mSeeks;  // the seeks the host has not answered, in order
    std::vector<Track> mTracks;
    PlaybackRate mRate;
    SyncSettings mSync;
    std::string mSourceName = "Unknown";
    int64_t mPlayingSinceNs = 0, mPlayedNs = 0;
    int64_t mNotifyAtUs = -1;
    bool mRenderingStarted = false;
    uint32_t mScaling = 0;

    std::mutex mWindowMutex;  // for the fields below
    ANativeWindow *mWindow = nullptr;
    uint64_t mHostWindow = 0;
};

/// The listener of the Java object: postEventFromNative with its weak reference.
class JNIMediaPlayerListener {
public:
    JNIMediaPlayerListener(JNIEnv *env, jobject thiz, jobject weak) {
        jclass clazz = env->GetObjectClass(thiz);
        mClass = static_cast<jclass>(env->NewGlobalRef(clazz));
        mObject = env->NewGlobalRef(weak);
        env->DeleteLocalRef(clazz);
        env->GetJavaVM(&mVm);
    }
    ~JNIMediaPlayerListener() {
        JNIEnv *env = nullptr;
        if (mVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) {
            env->DeleteGlobalRef(mObject);
            env->DeleteGlobalRef(mClass);
        }
    }

    void notify(int msg, int ext1, int ext2) {
        JNIEnv *env = nullptr;
        if (mVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) return;
        env->CallStaticVoidMethod(mClass, gFields.postEvent, mObject, msg, ext1, ext2, nullptr);
        if (env->ExceptionCheck()) {
            ALOGW("An exception occurred while notifying an event.");
            env->ExceptionClear();
        }
    }

private:
    JavaVM *mVm = nullptr;
    jclass mClass;
    jobject mObject;
};

// libmedia's MediaPlayer (mediaplayer.cpp), its player the media server's (Player above).
class MediaPlayer : public Listener, public std::enable_shared_from_this<MediaPlayer> {
public:
    explicit MediaPlayer(JavaVM *vm) : mVm(vm) {
        static std::atomic<int> sessions{0};
        mAudioSessionId = (++sessions << 3) | 1;  // AudioSystem::newAudioUniqueId(AUDIO_UNIQUE_ID_USE_SESSION)
    }

    void setListener(std::shared_ptr<JNIMediaPlayerListener> listener) {
        std::lock_guard hold(mLock);
        mListener = std::move(listener);
    }

    void disconnect(JNIEnv *env) {
        std::shared_ptr<Player> player;
        {
            std::lock_guard hold(mLock);
            player = std::move(mPlayer);
            mPlayer = nullptr;
        }
        if (player != nullptr) player->disconnect(env);
    }

    /// A new player for a source of `kind`, set up by `setSource`; attachNewPlayer's answers.
    template <typename SetSource>
    status_t setDataSource(JNIEnv *env, Player::Kind kind, SetSource setSource) {
        auto player = std::make_shared<Player>(mVm, weak_from_this());
        if (player->create(kind) != OK || setSource(*player) != OK) {
            player->disconnect(env);
            player = nullptr;
        }
        return attachNewPlayer(env, player);
    }

    status_t invoke(const Parcel &request, Parcel *reply) {
        std::lock_guard hold(mLock);
        const bool initialized =
            mCurrentState != MEDIA_PLAYER_STATE_ERROR && (mCurrentState & MEDIA_PLAYER_IDLE) != MEDIA_PLAYER_IDLE;
        if (mPlayer != nullptr && initialized) return mPlayer->invoke(request, reply);
        ALOGE("invoke failed: wrong state %X, mPlayer(%p)", mCurrentState, mPlayer.get());
        return INVALID_OPERATION;
    }

    status_t setVideoSurface(ANativeWindow *window) {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return NO_INIT;
        mPlayer->setVideoSurface(window);
        return OK;
    }

    status_t prepare() {
        std::unique_lock hold(mLock);
        if (mPrepareSync) return -EALREADY;
        mPrepareSync = true;
        const status_t ret = prepareAsync_l();
        if (ret != NO_ERROR) {
            mPrepareSync = false;  // ours: the tag leaves it set, and every later prepare answers -EALREADY
            return ret;
        }
        mSignal.wait(hold, [this] { return !mPrepareSync; });
        return mPrepareStatus;
    }

    status_t prepareAsync() {
        std::lock_guard hold(mLock);
        return prepareAsync_l();
    }

    status_t start() {
        std::lock_guard hold(mLock);
        if (mCurrentState & MEDIA_PLAYER_STARTED) return NO_ERROR;
        if (mPlayer != nullptr &&
            (mCurrentState & (MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_PLAYBACK_COMPLETE | MEDIA_PLAYER_PAUSED))) {
            mPlayer->setLooping(mLoop);
            mPlayer->setVolume(mLeftVolume, mRightVolume);
            mCurrentState = MEDIA_PLAYER_STARTED;
            const status_t ret = mPlayer->start();
            if (ret != NO_ERROR) mCurrentState = MEDIA_PLAYER_STATE_ERROR;
            return ret;
        }
        ALOGE("start called in state %d, mPlayer(%p)", mCurrentState, mPlayer.get());
        return INVALID_OPERATION;
    }

    status_t stop() {
        std::lock_guard hold(mLock);
        if (mCurrentState & MEDIA_PLAYER_STOPPED) return NO_ERROR;
        if (mPlayer != nullptr && (mCurrentState & (MEDIA_PLAYER_STARTED | MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_PAUSED |
                                                    MEDIA_PLAYER_PLAYBACK_COMPLETE))) {
            const status_t ret = mPlayer->stop();
            mCurrentState = ret != NO_ERROR ? MEDIA_PLAYER_STATE_ERROR : MEDIA_PLAYER_STOPPED;
            return ret;
        }
        ALOGE("stop called in state %d, mPlayer(%p)", mCurrentState, mPlayer.get());
        return INVALID_OPERATION;
    }

    status_t pause() {
        std::lock_guard hold(mLock);
        if (mCurrentState & (MEDIA_PLAYER_PAUSED | MEDIA_PLAYER_PLAYBACK_COMPLETE)) return NO_ERROR;
        if (mPlayer != nullptr && (mCurrentState & MEDIA_PLAYER_STARTED)) {
            const status_t ret = mPlayer->pause();
            mCurrentState = ret != NO_ERROR ? MEDIA_PLAYER_STATE_ERROR : MEDIA_PLAYER_PAUSED;
            return ret;
        }
        ALOGE("pause called in state %d, mPlayer(%p)", mCurrentState, mPlayer.get());
        return INVALID_OPERATION;
    }

    bool isPlaying() {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return false;
        const bool playing = mPlayer->isPlaying();
        if ((mCurrentState & MEDIA_PLAYER_STARTED) && !playing) {
            ALOGE("internal/external state mismatch corrected");
            mCurrentState = MEDIA_PLAYER_PAUSED;
        } else if ((mCurrentState & MEDIA_PLAYER_PAUSED) && playing) {
            ALOGE("internal/external state mismatch corrected");
            mCurrentState = MEDIA_PLAYER_STARTED;
        }
        return playing;
    }

    status_t setPlaybackSettings(const PlaybackRate &rate) {
        if (rate.speed < 0.f || rate.pitch < 0.f) return BAD_VALUE;
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr || (mCurrentState & MEDIA_PLAYER_STOPPED)) return INVALID_OPERATION;
        if (rate.speed != 0.f && !(mCurrentState & MEDIA_PLAYER_STARTED) &&
            (mCurrentState & (MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_PAUSED | MEDIA_PLAYER_PLAYBACK_COMPLETE))) {
            mPlayer->setLooping(mLoop);
            mPlayer->setVolume(mLeftVolume, mRightVolume);
        }
        const status_t err = mPlayer->setPlaybackSettings(rate);
        if (err == OK) {
            if (rate.speed == 0.f && mCurrentState == MEDIA_PLAYER_STARTED) {
                mCurrentState = MEDIA_PLAYER_PAUSED;
            } else if (rate.speed != 0.f &&
                       (mCurrentState &
                        (MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_PAUSED | MEDIA_PLAYER_PLAYBACK_COMPLETE))) {
                mCurrentState = MEDIA_PLAYER_STARTED;
            }
        }
        return err;
    }

    status_t getPlaybackSettings(PlaybackRate *rate) {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return INVALID_OPERATION;
        return mPlayer->getPlaybackSettings(rate);
    }

    status_t setSyncSettings(const SyncSettings &sync) {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return INVALID_OPERATION;
        return mPlayer->setSyncSettings(sync);
    }

    status_t getSyncSettings(SyncSettings *sync) {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return INVALID_OPERATION;
        return mPlayer->getSyncSettings(sync);
    }

    status_t getVideoWidth(int *w) {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return INVALID_OPERATION;
        *w = mVideoWidth;
        return NO_ERROR;
    }

    status_t getVideoHeight(int *h) {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return INVALID_OPERATION;
        *h = mVideoHeight;
        return NO_ERROR;
    }

    status_t getCurrentPosition(int *msec) {
        std::lock_guard hold(mLock);
        if (mPlayer != nullptr) {
            if (mCurrentPosition >= 0) {
                *msec = mCurrentPosition;
                return NO_ERROR;
            }
            return mPlayer->getCurrentPosition(msec);
        }
        return INVALID_OPERATION;
    }

    status_t getDuration(int *msec) {
        std::lock_guard hold(mLock);
        return getDuration_l(msec);
    }

    status_t seekTo(int msec, int mode) {
        std::lock_guard hold(mLock);
        return seekTo_l(msec, mode);
    }

    status_t notifyAt(int64_t mediaTimeUs) {
        std::lock_guard hold(mLock);
        if (mPlayer != nullptr) return mPlayer->notifyAt(mediaTimeUs);
        return INVALID_OPERATION;
    }

    status_t reset(JNIEnv *env) {
        std::shared_ptr<Player> gone;
        status_t ret = NO_ERROR;
        {
            std::lock_guard hold(mLock);
            mLoop = false;
            if (mCurrentState == MEDIA_PLAYER_IDLE) return NO_ERROR;
            mPrepareSync = false;
            if (mPlayer != nullptr) {
                ret = mPlayer->reset();
                if (ret != NO_ERROR) {
                    ALOGE("reset() failed with return code (%d)", ret);
                    mCurrentState = MEDIA_PLAYER_STATE_ERROR;
                } else {
                    mCurrentState = MEDIA_PLAYER_IDLE;
                }
                // setDataSource has to be called again to create a new mediaplayer.
                gone = std::move(mPlayer);
                mPlayer = nullptr;
            } else {
                clear_l();
            }
        }
        if (gone != nullptr) gone->disconnect(env);  // the media server's disconnect, outside the lock
        return ret;
    }

    status_t setAudioStreamType(int type) {
        std::lock_guard hold(mLock);
        if (mStreamType == type) return NO_ERROR;
        if (mCurrentState &
            (MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_STARTED | MEDIA_PLAYER_PAUSED | MEDIA_PLAYER_PLAYBACK_COMPLETE)) {
            ALOGE("setAudioStream called in state %d", mCurrentState);
            return INVALID_OPERATION;
        }
        mStreamType = type;
        return OK;
    }

    int getAudioStreamType() {
        std::lock_guard hold(mLock);
        return mStreamType;
    }

    status_t setLooping(int loop) {
        std::lock_guard hold(mLock);
        mLoop = loop != 0;
        if (mPlayer != nullptr) return mPlayer->setLooping(loop);
        return OK;
    }

    bool isLooping() {
        std::lock_guard hold(mLock);
        return mPlayer != nullptr && mLoop;
    }

    status_t setVolume(float left, float right) {
        std::lock_guard hold(mLock);
        mLeftVolume = left;
        mRightVolume = right;
        if (mPlayer != nullptr) return mPlayer->setVolume(left, right);
        return OK;
    }

    status_t setAudioSessionId(int sessionId) {
        std::lock_guard hold(mLock);
        if (!(mCurrentState & MEDIA_PLAYER_IDLE)) {
            ALOGE("setAudioSessionId called in state %d", mCurrentState);
            return INVALID_OPERATION;
        }
        if (sessionId < 0) return BAD_VALUE;
        mAudioSessionId = sessionId;
        return NO_ERROR;
    }

    int getAudioSessionId() {
        std::lock_guard hold(mLock);
        return mAudioSessionId;
    }

    status_t setAuxEffectSendLevel(float level) {
        std::lock_guard hold(mLock);
        mSendLevel = level;
        return OK;
    }

    status_t attachAuxEffect(int) {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr || (mCurrentState & MEDIA_PLAYER_IDLE) || mCurrentState == MEDIA_PLAYER_STATE_ERROR) {
            ALOGE("attachAuxEffect called in state %d, mPlayer(%p)", mCurrentState, mPlayer.get());
            return INVALID_OPERATION;
        }
        return OK;
    }

    status_t setParameter(int key) {
        std::lock_guard hold(mLock);
        if (key == KEY_PARAMETER_AUDIO_ATTRIBUTES &&
            (mCurrentState &
             (MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_STARTED | MEDIA_PLAYER_PAUSED | MEDIA_PLAYER_PLAYBACK_COMPLETE))) {
            ALOGE("trying to set audio attributes called in state %d", mCurrentState);
            return INVALID_OPERATION;
        }
        if (key == KEY_PARAMETER_AUDIO_ATTRIBUTES || mPlayer != nullptr) return OK;
        return INVALID_OPERATION;
    }

    status_t setRetransmitEndpoint(bool none) {
        std::lock_guard hold(mLock);
        if (mPlayer != nullptr || mCurrentState != MEDIA_PLAYER_IDLE) return INVALID_OPERATION;
        return none ? OK : INVALID_OPERATION;  // no retransmission (a named limit)
    }

    status_t setNextMediaPlayer(const std::shared_ptr<MediaPlayer> &next) {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return NO_INIT;
        if (next.get() == this) return INVALID_OPERATION;
        mNext = next;
        return OK;
    }

    status_t prepareDrm() {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return NO_INIT;
        if (!(mCurrentState & (MEDIA_PLAYER_PREPARING | MEDIA_PLAYER_PREPARED))) {
            ALOGE("prepareDrm is called in the wrong state (%d).", mCurrentState);
            return INVALID_OPERATION;
        }
        return ERROR_UNSUPPORTED;  // no DRM scheme
    }

    status_t releaseDrm() {
        std::lock_guard hold(mLock);
        if (mPlayer == nullptr) return NO_INIT;
        if (mCurrentState &
            (MEDIA_PLAYER_STARTED | MEDIA_PLAYER_PAUSED | MEDIA_PLAYER_PLAYBACK_COMPLETE | MEDIA_PLAYER_STATE_ERROR)) {
            ALOGE("releaseDrm Unexpected state %d. Can only be called in stopped/idle.", mCurrentState);
            return INVALID_OPERATION;
        }
        return OK;
    }

    status_t enableAudioDeviceCallback() {
        std::lock_guard hold(mLock);
        return mPlayer == nullptr ? NO_INIT : OK;
    }

    jobject metrics(JNIEnv *env) {
        std::shared_ptr<Player> player;
        {
            std::lock_guard hold(mLock);
            player = mPlayer;
        }
        return player != nullptr ? player->metrics(env) : nullptr;
    }

    void notify(int msg, int ext1, int ext2) { notifyFrom(nullptr, msg, ext1, ext2, nullptr); }

    /// mediaplayer.cpp's notify; a record of a player that is no longer this one's (`player`, null for the JNI's
    /// own) is dropped, as a callback on a disconnected player.
    void notifyFrom(const void *player, int msg, int ext1, int ext2, const Parcel *) override {
        bool send = true;
        std::shared_ptr<JNIMediaPlayerListener> listener;
        std::shared_ptr<MediaPlayer> startNext;
        {
            std::lock_guard hold(mLock);
            if (player != nullptr && player != mPlayer.get()) return;
            // Allows calls from JNI in idle state to notify errors
            if (!(msg == MEDIA_ERROR && mCurrentState == MEDIA_PLAYER_IDLE) && mPlayer == nullptr) {
                ALOGV("notify(%d, %d, %d) callback on disconnected mediaplayer", msg, ext1, ext2);
                return;
            }
            switch (msg) {
                case MEDIA_PREPARED:
                    mCurrentState = MEDIA_PLAYER_PREPARED;
                    if (mPrepareSync) {
                        mPrepareSync = false;
                        mPrepareStatus = NO_ERROR;
                        mSignal.notify_all();
                    }
                    break;
                case MEDIA_PLAYBACK_COMPLETE:
                    if (mCurrentState == MEDIA_PLAYER_IDLE) ALOGE("playback complete in idle state");
                    if (!mLoop) {
                        mCurrentState = MEDIA_PLAYER_PLAYBACK_COMPLETE;
                        startNext = mNext.lock();
                    }
                    break;
                case MEDIA_ERROR:
                    ALOGE("error (%d, %d)", ext1, ext2);
                    mCurrentState = MEDIA_PLAYER_STATE_ERROR;
                    if (mPrepareSync) {
                        mPrepareSync = false;
                        mPrepareStatus = ext1;
                        mSignal.notify_all();
                        send = false;
                    }
                    break;
                case MEDIA_SEEK_COMPLETE:
                    if (mSeekPosition != mCurrentPosition || mSeekMode != mCurrentSeekMode) {
                        mSeekPosition = -1;
                        mSeekMode = kSeekPreviousSync;
                        seekTo_l(mCurrentPosition, mCurrentSeekMode);
                    } else {
                        mCurrentPosition = mSeekPosition = -1;
                        mCurrentSeekMode = mSeekMode = kSeekPreviousSync;
                    }
                    break;
                case MEDIA_SET_VIDEO_SIZE:
                    mVideoWidth = ext1;
                    mVideoHeight = ext2;
                    break;
                default: break;
            }
            listener = mListener;
        }
        if (listener != nullptr && send) {
            std::lock_guard hold(mNotifyLock);
            listener->notify(msg, ext1, ext2);
        }
        // NuPlayer's next player starts as this one ends (MEDIA_INFO_STARTED_AS_NEXT, which Java's MediaPlayer
        // answers with a start of it).
        if (startNext != nullptr) startNext->notify(MEDIA_INFO, MEDIA_INFO_STARTED_AS_NEXT, 0);
    }

private:
    status_t attachNewPlayer(JNIEnv *env, const std::shared_ptr<Player> &player) {
        status_t err = UNKNOWN_ERROR;
        std::shared_ptr<Player> previous;
        {
            std::lock_guard hold(mLock);
            if (!((mCurrentState & MEDIA_PLAYER_IDLE) || mCurrentState == MEDIA_PLAYER_STATE_ERROR)) {
                ALOGE("attachNewPlayer called in state %d", mCurrentState);
                if (player != nullptr) player->disconnect(env);
                return INVALID_OPERATION;
            }
            clear_l();
            previous = std::move(mPlayer);
            mPlayer = player;
            if (player != nullptr) {
                mCurrentState = MEDIA_PLAYER_INITIALIZED;
                err = NO_ERROR;
            } else {
                ALOGE("Unable to create media player");
            }
        }
        if (previous != nullptr) previous->disconnect(env);
        return err;
    }

    void clear_l() {
        mCurrentPosition = -1;
        mCurrentSeekMode = kSeekPreviousSync;
        mSeekPosition = -1;
        mSeekMode = kSeekPreviousSync;
        mVideoWidth = mVideoHeight = 0;
    }

    status_t prepareAsync_l() {
        if (mPlayer != nullptr && (mCurrentState & (MEDIA_PLAYER_INITIALIZED | MEDIA_PLAYER_STOPPED))) {
            mCurrentState = MEDIA_PLAYER_PREPARING;
            return mPlayer->prepareAsync();
        }
        ALOGE("prepareAsync called in state %d, mPlayer(%p)", mCurrentState, mPlayer.get());
        return INVALID_OPERATION;
    }

    status_t getDuration_l(int *msec) {
        const bool valid = mCurrentState & (MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_STARTED | MEDIA_PLAYER_PAUSED |
                                            MEDIA_PLAYER_STOPPED | MEDIA_PLAYER_PLAYBACK_COMPLETE);
        if (mPlayer != nullptr && valid) {
            int durationMs = 0;
            // Do not enter error state just because no duration was available.
            if (mPlayer->getDuration(&durationMs) != OK) durationMs = -1;
            if (msec) *msec = durationMs;
            return OK;
        }
        ALOGE("Attempt to call getDuration in wrong state: mPlayer=%p, mCurrentState=%u", mPlayer.get(),
              mCurrentState);
        return INVALID_OPERATION;
    }

    status_t seekTo_l(int msec, int mode) {
        if (mPlayer != nullptr && (mCurrentState & (MEDIA_PLAYER_STARTED | MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_PAUSED |
                                                    MEDIA_PLAYER_PLAYBACK_COMPLETE))) {
            if (msec < 0) {
                ALOGW("Attempt to seek to invalid position: %d", msec);
                msec = 0;
            }
            int durationMs = 0;
            if (mPlayer->getDuration(&durationMs) != OK) {
                ALOGW("Stream has no duration and is therefore not seekable.");
                return UNKNOWN_ERROR;
            }
            if (msec > durationMs) {
                ALOGW("Attempt to seek to past end of file: request = %d, durationMs = %d", msec, durationMs);
                msec = durationMs;
            }
            // cache duration
            mCurrentPosition = msec;
            mCurrentSeekMode = mode;
            if (mSeekPosition < 0) {
                mSeekPosition = msec;
                mSeekMode = mode;
                return mPlayer->seekTo(msec, mode);
            }
            return NO_ERROR;  // a seek in progress: queued
        }
        ALOGE("Attempt to perform seekTo in wrong state: mPlayer=%p, mCurrentState=%u", mPlayer.get(), mCurrentState);
        return INVALID_OPERATION;
    }

    JavaVM *const mVm;
    std::mutex mLock;
    std::mutex mNotifyLock;
    std::condition_variable mSignal;
    std::shared_ptr<JNIMediaPlayerListener> mListener;
    std::shared_ptr<Player> mPlayer;
    std::weak_ptr<MediaPlayer> mNext;
    int mCurrentPosition = -1, mSeekPosition = -1;
    int mCurrentSeekMode = kSeekPreviousSync, mSeekMode = kSeekPreviousSync;
    unsigned mCurrentState = MEDIA_PLAYER_IDLE;
    bool mPrepareSync = false;
    status_t mPrepareStatus = NO_ERROR;
    int mStreamType = kAudioStreamMusic;
    bool mLoop = false;
    float mLeftVolume = 1, mRightVolume = 1;
    int mVideoWidth = 0, mVideoHeight = 0;
    int mAudioSessionId = 0;
    float mSendLevel = 0;
};

// MARK: - The natives

/// The tag's sLock: mNativeContext's hold, which a release on another thread ends.
std::mutex sLock;

std::shared_ptr<MediaPlayer> getMediaPlayer(JNIEnv *env, jobject thiz) {
    std::lock_guard hold(sLock);
    auto *held = reinterpret_cast<std::shared_ptr<MediaPlayer> *>(env->GetLongField(thiz, gFields.context));
    return held != nullptr ? *held : nullptr;
}

/// Holds `player` in mNativeContext; the previous one's hold ends.
void setMediaPlayer(JNIEnv *env, jobject thiz, std::shared_ptr<MediaPlayer> player) {
    auto *held = player != nullptr ? new std::shared_ptr<MediaPlayer>(std::move(player)) : nullptr;
    std::shared_ptr<MediaPlayer> *old = nullptr;
    {
        std::lock_guard hold(sLock);
        old = reinterpret_cast<std::shared_ptr<MediaPlayer> *>(env->GetLongField(thiz, gFields.context));
        env->SetLongField(thiz, gFields.context, reinterpret_cast<jlong>(held));
    }
    delete old;
}

/// process_media_player_call: an exception to throw for a failure, or without one an error callback.
void process_media_player_call(JNIEnv *env, jobject thiz, status_t opStatus, const char *exception,
                               const char *message) {
    if (exception == nullptr) {  // Don't throw exception. Instead, send an event.
        if (opStatus != OK) {
            if (auto mp = getMediaPlayer(env, thiz)) mp->notify(MEDIA_ERROR, opStatus, 0);
        }
        return;
    }
    if (opStatus == INVALID_OPERATION) {
        jniThrowException(env, "java/lang/IllegalStateException", nullptr);
    } else if (opStatus == BAD_VALUE) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
    } else if (opStatus == PERMISSION_DENIED) {
        jniThrowException(env, "java/lang/SecurityException", nullptr);
    } else if (opStatus != OK) {
        if (strlen(message) > 230) {
            jniThrowException(env, exception, message);
        } else {
            char text[256];
            snprintf(text, sizeof text, "%s: status=0x%X", message, static_cast<unsigned>(opStatus));
            jniThrowException(env, exception, text);
        }
    }
}

#define GET_PLAYER_OR(value)                                              \
    auto mp = getMediaPlayer(env, thiz);                                  \
    if (mp == nullptr) {                                                  \
        jniThrowException(env, "java/lang/IllegalStateException", nullptr); \
        return value;                                                     \
    }

/// Whether the app's network security config permits cleartext to the URL's host, as its HttpURLConnection asks.
bool cleartextPermitted(JNIEnv *env, jstring url) {
    jclass uriClass = env->FindClass("android/net/Uri");
    jclass policyClass = env->FindClass("android/security/NetworkSecurityPolicy");
    bool permitted = false;
    if (uriClass != nullptr && policyClass != nullptr) {
        jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        jmethodID getHost = env->GetMethodID(uriClass, "getHost", "()Ljava/lang/String;");
        jmethodID getInstance =
            env->GetStaticMethodID(policyClass, "getInstance", "()Landroid/security/NetworkSecurityPolicy;");
        jmethodID permits = env->GetMethodID(policyClass, "isCleartextTrafficPermitted", "(Ljava/lang/String;)Z");
        if (parse != nullptr && getHost != nullptr && getInstance != nullptr && permits != nullptr) {
            jobject uri = env->CallStaticObjectMethod(uriClass, parse, url);
            jobject host = uri != nullptr ? env->CallObjectMethod(uri, getHost) : nullptr;
            jobject policy = env->CallStaticObjectMethod(policyClass, getInstance);
            if (host != nullptr && policy != nullptr && !env->ExceptionCheck()) {
                permitted = env->CallBooleanMethod(policy, permits, host);
            }
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        permitted = false;
    }
    return permitted;
}

/// Whether the app holds INTERNET (its process's ActivityThread application), as the media server checks it.
bool internetGranted(JNIEnv *env) {
    jclass threadClass = env->FindClass("android/app/ActivityThread");
    bool granted = true;  // no application yet: the runtime's check alone
    if (threadClass != nullptr) {
        jmethodID current = env->GetStaticMethodID(threadClass, "currentApplication", "()Landroid/app/Application;");
        jobject application = current != nullptr ? env->CallStaticObjectMethod(threadClass, current) : nullptr;
        if (application != nullptr) {
            jclass contextClass = env->FindClass("android/content/Context");
            jmethodID check = contextClass != nullptr
                                  ? env->GetMethodID(contextClass, "checkSelfPermission", "(Ljava/lang/String;)I")
                                  : nullptr;
            jstring name = env->NewStringUTF("android.permission.INTERNET");
            if (check != nullptr && name != nullptr) granted = env->CallIntMethod(application, check, name) == 0;
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    return granted;
}

void android_media_MediaPlayer_setDataSourceAndHeaders(JNIEnv *env, jobject thiz, jobject, jstring path, jobjectArray,
                                                       jobjectArray) {
    GET_PLAYER_OR()
    if (path == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    const char *chars = env->GetStringUTFChars(path, nullptr);
    if (chars == nullptr) return;  // OutOfMemoryError
    const std::string url(chars);
    env->ReleaseStringUTFChars(path, chars);
    std::string scheme;
    for (const char c : url.substr(0, url.find(':'))) scheme += static_cast<char>(tolower(c));
    status_t status = UNKNOWN_ERROR;
    if (url.find(':') != std::string::npos && (scheme == "http" || scheme == "https")) {
        if (!internetGranted(env)) {
            // MediaPlayerService's checkPermission, as the runtime's own check of the app's INTERNET.
            status = mp->setDataSource(env, Player::Kind::refused, [](Player &) { return UNKNOWN_ERROR; });
        } else if (scheme == "http" && !cleartextPermitted(env, path)) {
            // Never to the host: the app's own connection would be refused (its prepare fails).
            status = mp->setDataSource(env, Player::Kind::refused, [](Player &) { return OK; });
        } else {
            status = mp->setDataSource(env, Player::Kind::url, [&](Player &player) {
                return player.setDataSource(url);
            });
        }
    } else {
        status = mp->setDataSource(env, Player::Kind::url, [](Player &) { return UNKNOWN_ERROR; });
    }
    process_media_player_call(env, thiz, status, "java/io/IOException", "setDataSource failed.");
}

void android_media_MediaPlayer_setDataSourceFD(JNIEnv *env, jobject thiz, jobject fileDescriptor, jlong offset,
                                               jlong length) {
    GET_PLAYER_OR()
    if (fileDescriptor == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    const int fd = jniGetFDFromFileDescriptor(env, fileDescriptor);
    const status_t status = mp->setDataSource(env, Player::Kind::fd, [&](Player &player) {
        return player.setDataSource(fd, offset, length);
    });
    process_media_player_call(env, thiz, status, "java/io/IOException", "setDataSourceFD failed.");
}

void android_media_MediaPlayer_setDataSourceCallback(JNIEnv *env, jobject thiz, jobject dataSource) {
    GET_PLAYER_OR()
    if (dataSource == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    const status_t status = mp->setDataSource(env, Player::Kind::callback, [&](Player &player) {
        return player.setDataSource(env, dataSource);
    });
    process_media_player_call(env, thiz, status, "java/lang/RuntimeException", "setDataSourceCallback failed.");
}

void android_media_MediaPlayer_setVideoSurface(JNIEnv *env, jobject thiz, jobject surface) {
    GET_PLAYER_OR()
    ANativeWindow *window = nullptr;
    if (surface != nullptr) {
        window = minato_ANativeWindow_fromSurface(env, surface);
        if (window == nullptr) {
            jniThrowException(env, "java/lang/IllegalArgumentException", "The surface has been released");
            return;
        }
    }
    mp->setVideoSurface(window);
    if (window != nullptr) ANativeWindow_release(window);
}

jint android_media_MediaPlayer_prepare(JNIEnv *env, jobject thiz, jobject) {
    GET_PLAYER_OR(0)
    process_media_player_call(env, thiz, mp->prepare(), "java/io/IOException", "Prepare failed.");
    return 0;
}

jint android_media_MediaPlayer_prepareAsync(JNIEnv *env, jobject thiz, jobject) {
    GET_PLAYER_OR(0)
    process_media_player_call(env, thiz, mp->prepareAsync(), "java/io/IOException", "Prepare Async failed.");
    return 0;
}

void android_media_MediaPlayer_start(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->start(), nullptr, nullptr);
}

void android_media_MediaPlayer_stop(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->stop(), nullptr, nullptr);
}

void android_media_MediaPlayer_pause(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->pause(), nullptr, nullptr);
}

jboolean android_media_MediaPlayer_isPlaying(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(JNI_FALSE)
    return mp->isPlaying() ? JNI_TRUE : JNI_FALSE;
}

void android_media_MediaPlayer_setPlaybackParams(JNIEnv *env, jobject thiz, jobject params) {
    GET_PLAYER_OR()
    PlaybackRate rate;
    status_t err = mp->getPlaybackSettings(&rate);
    if (err == OK) {
        const jint set = env->GetIntField(params, gFields.playbackSet);
        bool updated = false;
        if (set & 1) rate.speed = env->GetFloatField(params, gFields.playbackSpeed), updated = true;
        if (set & 2) rate.pitch = env->GetFloatField(params, gFields.playbackPitch), updated = true;
        if (set & 4) rate.fallbackMode = env->GetIntField(params, gFields.playbackFallback), updated = true;
        if (set & 8) rate.stretchMode = env->GetIntField(params, gFields.playbackStretch), updated = true;
        if (updated) err = mp->setPlaybackSettings(rate);
    }
    process_media_player_call(env, thiz, err, "java/lang/IllegalStateException", "unexpected error");
}

jobject android_media_MediaPlayer_getPlaybackParams(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(nullptr)
    PlaybackRate rate;
    const status_t err = mp->getPlaybackSettings(&rate);
    process_media_player_call(env, thiz, err, "java/lang/IllegalStateException", "unexpected error");
    if (err != OK) return nullptr;
    jobject params = env->NewObject(gFields.playbackParamsClass, gFields.playbackParamsConstructor);
    if (params == nullptr) return nullptr;
    env->SetFloatField(params, gFields.playbackSpeed, rate.speed);
    env->SetFloatField(params, gFields.playbackPitch, rate.pitch);
    env->SetIntField(params, gFields.playbackFallback, rate.fallbackMode);
    env->SetIntField(params, gFields.playbackStretch, rate.stretchMode);
    env->SetIntField(params, gFields.playbackSet, 15);
    return params;
}

void android_media_MediaPlayer_setSyncParams(JNIEnv *env, jobject thiz, jobject params) {
    GET_PLAYER_OR()
    SyncSettings sync;
    status_t err = mp->getSyncSettings(&sync);
    if (err == OK) {
        const jint set = env->GetIntField(params, gFields.syncSet);
        if (set & 1) sync.source = env->GetIntField(params, gFields.syncSource);
        if (set & 2) sync.adjustMode = env->GetIntField(params, gFields.syncAdjust);
        if (set & 4) sync.tolerance = env->GetFloatField(params, gFields.syncTolerance);
        if (set & 8) sync.frameRate = env->GetFloatField(params, gFields.syncFrameRate);
        if (sync.tolerance < 0 || sync.tolerance >= 1) err = BAD_VALUE;
        if (err == OK) err = mp->setSyncSettings(sync);
    }
    process_media_player_call(env, thiz, err, "java/lang/IllegalStateException", "unexpected error");
}

jobject android_media_MediaPlayer_getSyncParams(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(nullptr)
    SyncSettings sync;
    const status_t err = mp->getSyncSettings(&sync);
    process_media_player_call(env, thiz, err, "java/lang/IllegalStateException", "unexpected error");
    if (err != OK) return nullptr;
    jobject params = env->NewObject(gFields.syncParamsClass, gFields.syncParamsConstructor);
    if (params == nullptr) return nullptr;
    env->SetIntField(params, gFields.syncSource, sync.source);
    env->SetIntField(params, gFields.syncAdjust, sync.adjustMode);
    env->SetFloatField(params, gFields.syncTolerance, sync.tolerance);
    env->SetFloatField(params, gFields.syncFrameRate, sync.frameRate);
    env->SetIntField(params, gFields.syncSet, 7 | (sync.frameRate >= 0 ? 8 : 0));
    return params;
}

void android_media_MediaPlayer_seekTo(JNIEnv *env, jobject thiz, jlong msec, jint mode) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->seekTo(static_cast<int>(msec), mode), nullptr, nullptr);
}

void android_media_MediaPlayer_notifyAt(JNIEnv *env, jobject thiz, jlong mediaTimeUs) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->notifyAt(mediaTimeUs), nullptr, nullptr);
}

jint android_media_MediaPlayer_getVideoWidth(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(0)
    int w = 0;
    if (mp->getVideoWidth(&w) != OK) ALOGE("getVideoWidth failed");
    return w;
}

jint android_media_MediaPlayer_getVideoHeight(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(0)
    int h = 0;
    if (mp->getVideoHeight(&h) != OK) ALOGE("getVideoHeight failed");
    return h;
}

jobject android_media_MediaPlayer_native_getMetrics(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(nullptr)
    return mp->metrics(env);
}

jint android_media_MediaPlayer_getCurrentPosition(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(0)
    int msec = 0;
    process_media_player_call(env, thiz, mp->getCurrentPosition(&msec), nullptr, nullptr);
    return msec;
}

jint android_media_MediaPlayer_getDuration(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(0)
    int msec = 0;
    process_media_player_call(env, thiz, mp->getDuration(&msec), nullptr, nullptr);
    return msec;
}

void android_media_MediaPlayer_reset(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->reset(env), nullptr, nullptr);
}

void android_media_MediaPlayer_release(JNIEnv *env, jobject thiz) {
    auto mp = getMediaPlayer(env, thiz);
    setMediaPlayer(env, thiz, nullptr);
    if (mp != nullptr) {
        mp->setListener(nullptr);
        mp->disconnect(env);
    }
}

void android_media_MediaPlayer_setAudioStreamType(JNIEnv *env, jobject thiz, jint streamtype) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->setAudioStreamType(streamtype), nullptr, nullptr);
}

jint android_media_MediaPlayer_getAudioStreamType(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(0)
    return mp->getAudioStreamType();
}

jboolean android_media_MediaPlayer_setParameter(JNIEnv *env, jobject thiz, jint key, jobject) {
    GET_PLAYER_OR(JNI_FALSE)
    return mp->setParameter(key) == OK ? JNI_TRUE : JNI_FALSE;
}

void android_media_MediaPlayer_setLooping(JNIEnv *env, jobject thiz, jboolean looping) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->setLooping(looping), nullptr, nullptr);
}

jboolean android_media_MediaPlayer_isLooping(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(JNI_FALSE)
    return mp->isLooping() ? JNI_TRUE : JNI_FALSE;
}

void android_media_MediaPlayer_setVolume(JNIEnv *env, jobject thiz, jfloat left, jfloat right) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->setVolume(left, right), nullptr, nullptr);
}

jint android_media_MediaPlayer_invoke(JNIEnv *env, jobject thiz, jobject javaRequest, jobject javaReply) {
    auto mp = getMediaPlayer(env, thiz);
    if (mp == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", nullptr);
        return UNKNOWN_ERROR;
    }
    Parcel *request = parcelForJavaObject(env, javaRequest);
    Parcel *reply = parcelForJavaObject(env, javaReply);
    if (request == nullptr || reply == nullptr) return UNKNOWN_ERROR;
    request->setDataPosition(0);
    static const char16_t kInterface[] = u"android.media.IMediaPlayer";
    int32_t policy = 0;
    if (!request->enforceInterface(kInterface, sizeof kInterface / sizeof kInterface[0] - 1, &policy)) {
        return BAD_VALUE;
    }
    // Don't use process_media_player_call which use the async loop to report errors, instead returns the status.
    return mp->invoke(*request, reply);
}

jint android_media_MediaPlayer_setMetadataFilter(JNIEnv *env, jobject thiz, jobject) {
    GET_PLAYER_OR(UNKNOWN_ERROR)
    return OK;
}

jboolean android_media_MediaPlayer_getMetadata(JNIEnv *env, jobject thiz, jboolean, jboolean, jobject) {
    GET_PLAYER_OR(JNI_FALSE)
    return JNI_FALSE;  // no metadata (a named limit)
}

void android_media_MediaPlayer_native_init(JNIEnv *env) {
    jclass clazz = env->FindClass("android/media/MediaPlayer");
    if (clazz == nullptr) return;
    gFields.context = env->GetFieldID(clazz, "mNativeContext", "J");
    gFields.postEvent =
        env->GetStaticMethodID(clazz, "postEventFromNative", "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    env->DeleteLocalRef(clazz);

    clazz = env->FindClass("android/media/PlaybackParams");
    if (clazz == nullptr) return;
    gFields.playbackParamsClass = static_cast<jclass>(env->NewGlobalRef(clazz));
    gFields.playbackParamsConstructor = env->GetMethodID(clazz, "<init>", "()V");
    gFields.playbackSet = env->GetFieldID(clazz, "mSet", "I");
    gFields.playbackFallback = env->GetFieldID(clazz, "mAudioFallbackMode", "I");
    gFields.playbackStretch = env->GetFieldID(clazz, "mAudioStretchMode", "I");
    gFields.playbackPitch = env->GetFieldID(clazz, "mPitch", "F");
    gFields.playbackSpeed = env->GetFieldID(clazz, "mSpeed", "F");
    env->DeleteLocalRef(clazz);

    clazz = env->FindClass("android/media/SyncParams");
    if (clazz == nullptr) return;
    gFields.syncParamsClass = static_cast<jclass>(env->NewGlobalRef(clazz));
    gFields.syncParamsConstructor = env->GetMethodID(clazz, "<init>", "()V");
    gFields.syncSet = env->GetFieldID(clazz, "mSet", "I");
    gFields.syncAdjust = env->GetFieldID(clazz, "mAudioAdjustMode", "I");
    gFields.syncSource = env->GetFieldID(clazz, "mSyncSource", "I");
    gFields.syncTolerance = env->GetFieldID(clazz, "mTolerance", "F");
    gFields.syncFrameRate = env->GetFieldID(clazz, "mFrameRate", "F");
    env->DeleteLocalRef(clazz);

    clazz = env->FindClass("android/os/PersistableBundle");
    if (clazz == nullptr) return;
    gFields.bundleClass = static_cast<jclass>(env->NewGlobalRef(clazz));
    gFields.bundleConstructor = env->GetMethodID(clazz, "<init>", "()V");
    gFields.putString = env->GetMethodID(clazz, "putString", "(Ljava/lang/String;Ljava/lang/String;)V");
    gFields.putInt = env->GetMethodID(clazz, "putInt", "(Ljava/lang/String;I)V");
    gFields.putLong = env->GetMethodID(clazz, "putLong", "(Ljava/lang/String;J)V");
    env->DeleteLocalRef(clazz);

    clazz = env->FindClass("android/media/MediaDataSource");
    if (clazz == nullptr) return;
    gFields.dataSourceClass = static_cast<jclass>(env->NewGlobalRef(clazz));
    gFields.readAt = env->GetMethodID(clazz, "readAt", "(J[BII)I");
    gFields.getSize = env->GetMethodID(clazz, "getSize", "()J");
    gFields.close = env->GetMethodID(clazz, "close", "()V");
    env->DeleteLocalRef(clazz);
}

void android_media_MediaPlayer_native_setup(JNIEnv *env, jobject thiz, jobject weak, jobject, jint sessionId) {
    JavaVM *vm = nullptr;
    env->GetJavaVM(&vm);
    auto mp = std::make_shared<MediaPlayer>(vm);
    if (sessionId != 0) mp->setAudioSessionId(sessionId);
    mp->setListener(std::make_shared<JNIMediaPlayerListener>(env, thiz, weak));
    setMediaPlayer(env, thiz, std::move(mp));
}

void android_media_MediaPlayer_native_finalize(JNIEnv *env, jobject thiz) {
    if (getMediaPlayer(env, thiz) != nullptr) ALOGW("MediaPlayer finalized without being released");
    android_media_MediaPlayer_release(env, thiz);
}

jint android_media_MediaPlayer_get_audio_session_id(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(0)
    return mp->getAudioSessionId();
}

void android_media_MediaPlayer_set_audio_session_id(JNIEnv *env, jobject thiz, jint sessionId) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->setAudioSessionId(sessionId), nullptr, nullptr);
}

void android_media_MediaPlayer_setAuxEffectSendLevel(JNIEnv *env, jobject thiz, jfloat level) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->setAuxEffectSendLevel(level), nullptr, nullptr);
}

void android_media_MediaPlayer_attachAuxEffect(JNIEnv *env, jobject thiz, jint effectId) {
    GET_PLAYER_OR()
    process_media_player_call(env, thiz, mp->attachAuxEffect(effectId), nullptr, nullptr);
}

jint android_media_MediaPlayer_pullBatteryData(JNIEnv *, jclass, jobject) {
    return INVALID_OPERATION;  // the media server's battery data, which an app cannot read either
}

jint android_media_MediaPlayer_setRetransmitEndpoint(JNIEnv *env, jobject thiz, jstring address, jint) {
    GET_PLAYER_OR(INVALID_OPERATION)
    return mp->setRetransmitEndpoint(address == nullptr);
}

void android_media_MediaPlayer_setNextMediaPlayer(JNIEnv *env, jobject thiz, jobject java_player) {
    GET_PLAYER_OR()
    std::shared_ptr<MediaPlayer> next;
    if (java_player != nullptr) {
        next = getMediaPlayer(env, java_player);
        if (next == nullptr) {
            jniThrowException(env, "java/lang/IllegalStateException", "next player not initialized");
            return;
        }
    }
    if (mp->setNextMediaPlayer(next) != OK) {
        jniThrowException(env, "java/lang/IllegalStateException", "failed to set next player");
    }
}

jint android_media_MediaPlayer_applyVolumeShaper(JNIEnv *env, jobject thiz, jobject, jobject) {
    GET_PLAYER_OR(INVALID_OPERATION)
    return INVALID_OPERATION;  // no volume shapers (a named limit)
}

jobject android_media_MediaPlayer_getVolumeShaperState(JNIEnv *env, jobject thiz, jint) {
    GET_PLAYER_OR(nullptr)
    return nullptr;
}

void android_media_MediaPlayer_prepareDrm(JNIEnv *env, jobject thiz, jbyteArray uuid, jbyteArray sessionId) {
    GET_PLAYER_OR()
    if (uuid == nullptr || sessionId == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    const status_t err = mp->prepareDrm();
    if (err == INVALID_OPERATION) {
        jniThrowException(env, "java/lang/IllegalStateException", "Failed to prepare DRM scheme");
    } else if (err != OK) {
        jniThrowException(env, "android/media/MediaDrm$MediaDrmStateException", "Failed to prepare DRM scheme");
    }
}

void android_media_MediaPlayer_releaseDrm(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR()
    if (mp->releaseDrm() == INVALID_OPERATION) {
        jniThrowException(env, "java/lang/IllegalStateException", "Can not release DRM in an active player state.");
    }
}

jboolean android_media_MediaPlayer_setOutputDevice(JNIEnv *env, jobject thiz, jint) {
    GET_PLAYER_OR(JNI_FALSE)
    return JNI_FALSE;  // no audio routing (a named limit)
}

jint android_media_MediaPlayer_getRoutedDeviceId(JNIEnv *env, jobject thiz) {
    GET_PLAYER_OR(0)
    return 0;  // AUDIO_PORT_HANDLE_NONE
}

void android_media_MediaPlayer_enableDeviceCallback(JNIEnv *env, jobject thiz, jboolean) {
    GET_PLAYER_OR()
    const status_t status = mp->enableAudioDeviceCallback();
    if (status != NO_ERROR) {
        jniThrowException(env, "java/lang/IllegalStateException", nullptr);
        ALOGE("enable device callback failed: %d", status);
    }
}

const JNINativeMethod gMethods[] = {
    {"nativeSetDataSource", "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
     reinterpret_cast<void *>(android_media_MediaPlayer_setDataSourceAndHeaders)},
    {"_setDataSource", "(Ljava/io/FileDescriptor;JJ)V",
     reinterpret_cast<void *>(android_media_MediaPlayer_setDataSourceFD)},
    {"_setDataSource", "(Landroid/media/MediaDataSource;)V",
     reinterpret_cast<void *>(android_media_MediaPlayer_setDataSourceCallback)},
    {"_setVideoSurface", "(Landroid/view/Surface;)V",
     reinterpret_cast<void *>(android_media_MediaPlayer_setVideoSurface)},
    {"_prepare", "(Landroid/os/Parcel;)I", reinterpret_cast<void *>(android_media_MediaPlayer_prepare)},
    {"_prepareAsync", "(Landroid/os/Parcel;)I", reinterpret_cast<void *>(android_media_MediaPlayer_prepareAsync)},
    {"_start", "()V", reinterpret_cast<void *>(android_media_MediaPlayer_start)},
    {"_stop", "()V", reinterpret_cast<void *>(android_media_MediaPlayer_stop)},
    {"getVideoWidth", "()I", reinterpret_cast<void *>(android_media_MediaPlayer_getVideoWidth)},
    {"getVideoHeight", "()I", reinterpret_cast<void *>(android_media_MediaPlayer_getVideoHeight)},
    {"native_getMetrics", "()Landroid/os/PersistableBundle;",
     reinterpret_cast<void *>(android_media_MediaPlayer_native_getMetrics)},
    {"setPlaybackParams", "(Landroid/media/PlaybackParams;)V",
     reinterpret_cast<void *>(android_media_MediaPlayer_setPlaybackParams)},
    {"getPlaybackParams", "()Landroid/media/PlaybackParams;",
     reinterpret_cast<void *>(android_media_MediaPlayer_getPlaybackParams)},
    {"setSyncParams", "(Landroid/media/SyncParams;)V",
     reinterpret_cast<void *>(android_media_MediaPlayer_setSyncParams)},
    {"getSyncParams", "()Landroid/media/SyncParams;",
     reinterpret_cast<void *>(android_media_MediaPlayer_getSyncParams)},
    {"_seekTo", "(JI)V", reinterpret_cast<void *>(android_media_MediaPlayer_seekTo)},
    {"_notifyAt", "(J)V", reinterpret_cast<void *>(android_media_MediaPlayer_notifyAt)},
    {"_pause", "()V", reinterpret_cast<void *>(android_media_MediaPlayer_pause)},
    {"isPlaying", "()Z", reinterpret_cast<void *>(android_media_MediaPlayer_isPlaying)},
    {"getCurrentPosition", "()I", reinterpret_cast<void *>(android_media_MediaPlayer_getCurrentPosition)},
    {"getDuration", "()I", reinterpret_cast<void *>(android_media_MediaPlayer_getDuration)},
    {"_release", "()V", reinterpret_cast<void *>(android_media_MediaPlayer_release)},
    {"_reset", "()V", reinterpret_cast<void *>(android_media_MediaPlayer_reset)},
    {"_setAudioStreamType", "(I)V", reinterpret_cast<void *>(android_media_MediaPlayer_setAudioStreamType)},
    {"_getAudioStreamType", "()I", reinterpret_cast<void *>(android_media_MediaPlayer_getAudioStreamType)},
    {"setParameter", "(ILandroid/os/Parcel;)Z", reinterpret_cast<void *>(android_media_MediaPlayer_setParameter)},
    {"setLooping", "(Z)V", reinterpret_cast<void *>(android_media_MediaPlayer_setLooping)},
    {"isLooping", "()Z", reinterpret_cast<void *>(android_media_MediaPlayer_isLooping)},
    {"_setVolume", "(FF)V", reinterpret_cast<void *>(android_media_MediaPlayer_setVolume)},
    {"native_invoke", "(Landroid/os/Parcel;Landroid/os/Parcel;)I",
     reinterpret_cast<void *>(android_media_MediaPlayer_invoke)},
    {"native_setMetadataFilter", "(Landroid/os/Parcel;)I",
     reinterpret_cast<void *>(android_media_MediaPlayer_setMetadataFilter)},
    {"native_getMetadata", "(ZZLandroid/os/Parcel;)Z", reinterpret_cast<void *>(android_media_MediaPlayer_getMetadata)},
    {"native_init", "()V", reinterpret_cast<void *>(android_media_MediaPlayer_native_init)},
    {"native_setup", "(Ljava/lang/Object;Landroid/os/Parcel;I)V",
     reinterpret_cast<void *>(android_media_MediaPlayer_native_setup)},
    {"native_finalize", "()V", reinterpret_cast<void *>(android_media_MediaPlayer_native_finalize)},
    {"getAudioSessionId", "()I", reinterpret_cast<void *>(android_media_MediaPlayer_get_audio_session_id)},
    {"native_setAudioSessionId", "(I)V", reinterpret_cast<void *>(android_media_MediaPlayer_set_audio_session_id)},
    {"_setAuxEffectSendLevel", "(F)V", reinterpret_cast<void *>(android_media_MediaPlayer_setAuxEffectSendLevel)},
    {"attachAuxEffect", "(I)V", reinterpret_cast<void *>(android_media_MediaPlayer_attachAuxEffect)},
    {"native_pullBatteryData", "(Landroid/os/Parcel;)I",
     reinterpret_cast<void *>(android_media_MediaPlayer_pullBatteryData)},
    {"native_setRetransmitEndpoint", "(Ljava/lang/String;I)I",
     reinterpret_cast<void *>(android_media_MediaPlayer_setRetransmitEndpoint)},
    {"setNextMediaPlayer", "(Landroid/media/MediaPlayer;)V",
     reinterpret_cast<void *>(android_media_MediaPlayer_setNextMediaPlayer)},
    {"native_applyVolumeShaper",
     "(Landroid/media/VolumeShaper$Configuration;Landroid/media/VolumeShaper$Operation;)I",
     reinterpret_cast<void *>(android_media_MediaPlayer_applyVolumeShaper)},
    {"native_getVolumeShaperState", "(I)Landroid/media/VolumeShaper$State;",
     reinterpret_cast<void *>(android_media_MediaPlayer_getVolumeShaperState)},
    {"_prepareDrm", "([B[B)V", reinterpret_cast<void *>(android_media_MediaPlayer_prepareDrm)},
    {"_releaseDrm", "()V", reinterpret_cast<void *>(android_media_MediaPlayer_releaseDrm)},
    {"native_setOutputDevice", "(I)Z", reinterpret_cast<void *>(android_media_MediaPlayer_setOutputDevice)},
    {"native_getRoutedDeviceId", "()I", reinterpret_cast<void *>(android_media_MediaPlayer_getRoutedDeviceId)},
    {"native_enableDeviceCallback", "(Z)V", reinterpret_cast<void *>(android_media_MediaPlayer_enableDeviceCallback)},
};

}  // namespace

int register_android_media_MediaPlayer(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/media/MediaPlayer", gMethods, NELEM(gMethods));
}

}  // namespace android
