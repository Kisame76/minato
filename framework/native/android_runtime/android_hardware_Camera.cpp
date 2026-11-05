// Ours in place of AOSP's core/jni/android_hardware_Camera.cpp (ADR-0073 step e): the legacy android.hardware.Camera
// on the host's cameras (guest_interface.h's camera, 4137), answering as AOSP's natives do over libcamera_client and
// cameraservice's Camera2Client [verified: android_hardware_Camera.cpp and Parameters.cpp at android-15.0.0_r1,
// camera-legacy-test on apk-runner-35]:
// - _getNumberOfCameras is the host's count (0 without host cameras, as on a device without them). _getCameraInfo
//   gives a camera's legacy facing (camera2's back is CAMERA_FACING_BACK 0) and orientation from the host's facts,
//   and canDisableShutterSound true (ro.camera.sound.forced is 0); an ID past the cameras throws
//   RuntimeException("Unknown camera ID") with AOSP's log line, facts that do not hold "Fail to get camera info".
// - native_setup opens the host's camera; without the guest's camera grant (ADR-0073 decision 8) or for an ID past the
//   cameras it answers -EACCES, as Camera::connect's failure, so that open throws RuntimeException("Fail to connect to
//   camera service"). A second open of a camera that is open takes it from the first Camera, as cameraservice evicts
//   the older client of the same app: the first one's calls then fail as a disconnected camera's
//   (getParameters "getParameters failed (empty parameters)", setParameters "setParameters failed", lock "lock
//   failed", unlock "unlock failed", reconnect IOException "reconnect failed"), and its release only lets it go
//   [verified: camera-legacy-test]. Its parameters are Parameters::initialize's (camera_parameters.h) from the
//   camera's facts, with Minato's video sizes up to 1920 x 1080 (a named limit: no encoders yet); native_release
//   closes it, also when the finalizer comes after release.
// - native_getParameters gives the parameters; native_setParameters takes what Parameters::set takes (the lists'
//   values) and keeps the app's string, as CameraParameters2 does, else throws RuntimeException("setParameters
//   failed"). lock, unlock and reconnect succeed. Each throws RuntimeException("Camera is being used after
//   Camera.release() was called") after a release, as get_native_camera does.
// - The preview (part 2): setPreviewTexture and setPreviewSurface claim the surface's window as its producer, as the
//   codec does; startPreview configures the host's camera with the preview size's PRIVATE stream (and, with a
//   preview callback, a YUV one of the same size) and a repeating request from the parameters, and a thread of the
//   Camera's takes each frame: into a queue window's free slot (a frame without one is dropped, as a camera's when
//   its consumer lags) at the frame's CLOCK_BOOTTIME time, or into a host window; with a preview callback, the YUV
//   frame drawn into a buffer of the thread's own and given to Java as NV21 (CAMERA_MSG_PREVIEW_FRAME), in the app's
//   buffers when it added them (setPreviewCallbackWithBuffer: none left, no frame). stopPreview stops the request
//   and drops what waits; previewEnabled says whether it runs. While it runs, setParameters refuses another preview
//   size or format, as Parameters::set does.
// Camera's other natives (focus, pictures, zoom, faces) stay unregistered until the next parts of step e.
#include <errno.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

#include <APKRuntime/apkrt.h>
#include <APKRuntime/buffer_queue.h>
#include <APKRuntime/guest_interface.h>

#include "camera_parameters.h"
#include "core_jni_helpers.h"
#include "minato_gles.h"

namespace android {
namespace {

constexpr jint kFacingBack = 0, kFacingFront = 1;  // Camera.CameraInfo's

constexpr jint kMessageShutter = 0x002, kMessageFocus = 0x004, kMessagePreviewFrame = 0x010;  // CAMERA_MSG_*
constexpr jint kMessageRawImage = 0x080, kMessageCompressedImage = 0x100;

struct {
    jfieldID context;  // Camera.mNativeContext
    jfieldID facing, orientation, can_disable_shutter_sound;
    jclass camera;  // global
    jmethodID post;  // Camera.postEventFromNative
} gFields;

/// libnativewindow's private functions for a surface, looked up once, as MediaPlayer's natives do.
struct WindowFunctions {
    decltype(&minato_ANativeWindow_isQueue) isQueue = nullptr;
    decltype(&minato_ANativeWindow_tryDequeue) dequeue = nullptr;
    decltype(&minato_ANativeWindow_queueWith) queue = nullptr;
    decltype(&minato_ANativeWindow_cancel) cancel = nullptr;
    decltype(&minato_ANativeWindow_claim) claim = nullptr;
    decltype(&minato_ANativeWindow_unclaim) unclaim = nullptr;
    decltype(&minato_ANativeWindow_getInfo) info = nullptr;
};

const WindowFunctions *window_functions() {
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
    return functions.isQueue != nullptr && functions.dequeue != nullptr && functions.queue != nullptr &&
                   functions.cancel != nullptr && functions.claim != nullptr && functions.unclaim != nullptr &&
                   functions.info != nullptr
               ? &functions
               : nullptr;
}

std::mutex gLock;  // around the contexts, as AOSP's sLock

long camera_call(uint64_t op, apkrt_camera *call) {
    long result;
    do {
        result = syscall(APKRT_NR_CAMERA, op, call);
    } while (result == -1 && errno == EINTR);
    return result == -1 ? -errno : result;
}

uint32_t camera_count() {
    apkrt_camera call{};
    return camera_call(APKRT_CAMERA_COUNT, &call) == 0 ? call.count : 0;
}

/// The host's facts of camera `index` read as a camera's; false where it has none that hold.
bool camera_facts(uint32_t index, minato::camera::Facts *facts) {
    std::vector<unsigned char> bytes(APKRT_CAMERA_MAX_FACTS);
    apkrt_camera call{};
    call.camera = index;
    call.data = reinterpret_cast<uintptr_t>(bytes.data());
    call.size = bytes.size();
    if (camera_call(APKRT_CAMERA_FACTS, &call) != 0) return false;
    return minato::camera::read_facts(bytes.data(), static_cast<size_t>(call.size), facts);
}

/// An open camera of a Java Camera.
struct Context {
    uint64_t id = 0;
    uint32_t index = 0;
    bool evicted = false;  // a second open took the camera: the host's camera is closed
    minato::camera::Facts facts;
    std::string initial, parameters;  // Parameters::initialize's; the ones in force
    JavaVM *vm = nullptr;
    jobject weak = nullptr;  // the Java Camera's weak reference, a global one, for its events
    // The preview.
    ANativeWindow *window = nullptr;  // claimed, with a reference of ours; NULL for none
    uint64_t host_window = 0;         // the window's ID when it is a host window
    bool previewing = false, stopping = false, callback = false, manual = false;
    std::vector<jobject> buffers;     // the app's callback buffers (global references), oldest first
    pthread_t thread{};
    bool thread_made = false;
    int32_t width = 0, height = 0;    // the preview's size
    bool focusing = false, focus_seen = false;  // an autoFocus waits; its frames have shown FOCUSING
    uint64_t focus_number = 0;        // its request's number
};

constexpr uint64_t kFocusRequest = 2;  // the request numbers of an autoFocus' trigger

std::vector<Context *> gOpen;  // the contexts that hold their camera, with gLock held

void close_camera(Context *context) {
    apkrt_camera call{};
    call.id = context->id;
    camera_call(APKRT_CAMERA_CLOSE, &call);
}

/// The value of `key` in flattened parameters, empty for none.
std::string value_of(const std::string &params, const char *key) {
    const std::string needle = std::string(key) + "=";
    for (size_t at = params.find(needle); at != std::string::npos; at = params.find(needle, at + 1)) {
        if (at != 0 && params[at - 1] != ';') continue;
        const size_t start = at + needle.size(), end = params.find(';', start);
        return params.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    return "";
}

/// The preview's repeating request's controls from the parameters in force, within the host's facts.
apkrt_camera_controls controls_of(const Context &context) {
    apkrt_camera_controls controls{};
    const std::string focus = value_of(context.parameters, "focus-mode");
    int32_t af = focus == "auto" ? 1 : focus == "macro" ? 2 : focus == "continuous-video" ? 3
               : focus == "continuous-picture" ? 4 : 0;
    const std::vector<int32_t> modes = context.facts.info.af_modes.value_or(std::vector<int32_t>{0});
    if (std::find(modes.begin(), modes.end(), af) == modes.end()) af = modes.empty() ? 0 : modes[0];
    controls.af_mode = static_cast<uint8_t>(af);
    controls.awb_mode = 1, controls.ae_mode = 1, controls.jpeg_quality = 90, controls.zoom_ratio = 1;
    int low = 0, high = 0;
    std::sscanf(value_of(context.parameters, "preview-fps-range").c_str(), "%d,%d", &low, &high);
    controls.fps_min = low / 1000, controls.fps_max = high / 1000;
    const std::vector<int32_t> &ranges = context.facts.info.fps_ranges;
    bool listed = false;
    for (size_t n = 0; n + 1 < ranges.size(); n += 2) {
        listed |= ranges[n] == controls.fps_min && ranges[n + 1] == controls.fps_max;
    }
    if (!listed && ranges.size() >= 2) controls.fps_min = ranges[0], controls.fps_max = ranges[1];
    return controls;
}

/// A frame's preview token drawn into the window, or dropped without one or without a free slot.
void show(const Context &context, uint64_t token, int64_t timestamp_ns) {
    apkrt_camera call{};
    call.id = context.id;
    call.data = token;
    const WindowFunctions *functions = window_functions();
    if (context.window != nullptr && context.host_window != 0) {
        call.size = context.host_window;
        if (camera_call(APKRT_CAMERA_RENDER_WINDOW, &call) == 0) return;
        return;  // the token ended either way
    }
    AHardwareBuffer *buffer = nullptr;
    int32_t slot = -1;
    uint64_t buffer_id = 0;
    if (context.window == nullptr || functions == nullptr || functions->dequeue(context.window, &buffer, &slot) != 0 ||
        AHardwareBuffer_getId(buffer, &buffer_id) != 0) {
        if (buffer != nullptr) functions->cancel(context.window, slot);
        camera_call(APKRT_CAMERA_DISCARD, &call);
        return;
    }
    call.size = buffer_id;
    if (camera_call(APKRT_CAMERA_RENDER, &call) == 0) {
        minato_BufferQueue_settings settings{};
        settings.timestamp = timestamp_ns;
        settings.exact_time = 1;
        functions->queue(context.window, slot, &settings);
    } else {
        functions->cancel(context.window, slot);
    }
}

/// The YUV frame of `token` drawn into `target` and copied as NV21 into `out` (width x height x 3 / 2 bytes).
bool nv21(const Context &context, uint64_t token, AHardwareBuffer *target, std::vector<uint8_t> &out) {
    uint64_t buffer_id = 0;
    apkrt_camera call{};
    call.id = context.id;
    call.data = token;
    if (target == nullptr || AHardwareBuffer_getId(target, &buffer_id) != 0) {
        camera_call(APKRT_CAMERA_DISCARD, &call);
        return false;
    }
    call.size = buffer_id;
    if (camera_call(APKRT_CAMERA_RENDER, &call) != 0) return false;
    AHardwareBuffer_Planes planes{};
    if (AHardwareBuffer_lockPlanes(target, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &planes) != 0 ||
        planes.planeCount < 3) {
        return false;
    }
    const auto width = static_cast<size_t>(context.width), height = static_cast<size_t>(context.height);
    out.resize(width * height * 3 / 2);
    for (size_t y = 0; y < height; y++) {
        const auto *row = static_cast<uint8_t *>(planes.planes[0].data) + y * planes.planes[0].rowStride;
        std::memcpy(out.data() + y * width, row, width);
    }
    uint8_t *vu = out.data() + width * height;
    for (size_t y = 0; y < height / 2; y++) {
        const auto *u = static_cast<uint8_t *>(planes.planes[1].data) + y * planes.planes[1].rowStride;
        const auto *v = static_cast<uint8_t *>(planes.planes[2].data) + y * planes.planes[2].rowStride;
        for (size_t x = 0; x < width / 2; x++) {
            vu[(y * width / 2 + x) * 2] = v[x * planes.planes[2].pixelStride];
            vu[(y * width / 2 + x) * 2 + 1] = u[x * planes.planes[1].pixelStride];
        }
    }
    AHardwareBuffer_unlock(target, nullptr);
    return true;
}

/// The preview's thread: each captured frame into the window, and with a preview callback its YUV copy to Java.
void *preview_thread(void *argument) {
    auto *context = static_cast<Context *>(argument);
    JNIEnv *env = nullptr;
    JavaVMAttachArgs attach{JNI_VERSION_1_6, "CameraPreview", nullptr};
    if (context->vm->AttachCurrentThread(&env, &attach) != JNI_OK) return nullptr;
    AHardwareBuffer_Desc description{};
    description.width = static_cast<uint32_t>(context->width);
    description.height = static_cast<uint32_t>(context->height);
    description.layers = 1;
    description.format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
    description.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    AHardwareBuffer *target = nullptr;
    if (AHardwareBuffer_allocate(&description, &target) != 0) target = nullptr;
    std::vector<uint8_t> bytes;
    for (;;) {
        bool stopping;
        {
            const std::lock_guard<std::mutex> hold(gLock);
            stopping = context->stopping;
        }
        apkrt_camera_frame frame{};
        apkrt_camera call{};
        call.id = context->id;
        call.data = reinterpret_cast<uintptr_t>(&frame);
        const long got = camera_call(APKRT_CAMERA_FRAME, &call);
        if (got == -EAGAIN) {
            if (stopping) break;
            const timespec wait{0, 2000000};
            nanosleep(&wait, nullptr);
            continue;
        }
        if (got != 0) break;
        if (frame.tokens[0] != 0) show(*context, frame.tokens[0], frame.timestamp_ns);
        {
            // autoFocus: done at the first frame that no longer focuses, after one that did (or the trigger's).
            bool done = false, success = false;
            {
                const std::lock_guard<std::mutex> hold(gLock);
                if (context->focusing) {
                    const bool now = (frame.states & APKRT_CAMERA_STATE_FOCUSING) != 0;
                    context->focus_seen |= now || frame.number == context->focus_number;
                    if (context->focus_seen && !now) {
                        done = true;
                        success = (frame.states & APKRT_CAMERA_STATE_FOCUSED) != 0;
                        context->focusing = false;
                    }
                }
            }
            if (done) {
                env->CallStaticVoidMethod(gFields.camera, gFields.post, context->weak, kMessageFocus, success ? 1 : 0,
                                          0, nullptr);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
        if (frame.tokens[1] == 0) continue;
        jobject array = nullptr;
        bool manual = false;
        {
            const std::lock_guard<std::mutex> hold(gLock);
            if (context->callback && !stopping) {
                manual = context->manual;
                if (manual && !context->buffers.empty()) {
                    array = context->buffers.front();  // a global reference, ours now
                    context->buffers.erase(context->buffers.begin());
                }
            }
        }
        const bool wanted = array != nullptr || (!manual && context->callback && !stopping);
        if (!wanted) {
            apkrt_camera discard{};
            discard.id = context->id;
            discard.data = frame.tokens[1];
            camera_call(APKRT_CAMERA_DISCARD, &discard);
            continue;
        }
        if (!nv21(*context, frame.tokens[1], target, bytes)) {
            if (array != nullptr) env->DeleteGlobalRef(array);
            continue;
        }
        const auto size = static_cast<jsize>(bytes.size());
        if (array == nullptr) array = env->NewByteArray(size);
        else if (env->GetArrayLength(static_cast<jbyteArray>(array)) < size) {
            env->DeleteGlobalRef(array);
            continue;  // a buffer too small: the frame is lost, as Android's
        }
        if (array == nullptr) continue;
        env->SetByteArrayRegion(static_cast<jbyteArray>(array), 0, size, reinterpret_cast<const jbyte *>(bytes.data()));
        env->CallStaticVoidMethod(gFields.camera, gFields.post, context->weak, kMessagePreviewFrame, 0, 0, array);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (manual) env->DeleteGlobalRef(array);
        else env->DeleteLocalRef(array);
    }
    if (target != nullptr) AHardwareBuffer_release(target);
    context->vm->DetachCurrentThread();
    return nullptr;
}

/// The preview stopped, its thread joined; gLock must not be held.
void stop_preview(Context *context) {
    {
        const std::lock_guard<std::mutex> hold(gLock);
        if (!context->previewing) return;
        context->stopping = true;
        context->previewing = false;
    }
    apkrt_camera call{};
    call.id = context->id;
    camera_call(APKRT_CAMERA_STOP_REPEATING, &call);
    if (context->thread_made) pthread_join(context->thread, nullptr);
    context->thread_made = false;
    camera_call(APKRT_CAMERA_FLUSH, &call);
}

/// The preview's surface: `window` claimed as its producer in place of the one before (NULL: none).
bool set_window(Context *context, ANativeWindow *window) {
    const WindowFunctions *functions = window_functions();
    if (window != nullptr && (functions == nullptr || functions->claim(window) != 0)) {
        ANativeWindow_release(window);
        return false;
    }
    if (context->window != nullptr) {
        functions->unclaim(context->window);
        ANativeWindow_release(context->window);
    }
    context->window = window;
    context->host_window = 0;
    minato_window_info info{};
    if (window != nullptr && functions->isQueue(window) == 0 && functions->info(window, &info) == 0) {
        context->host_window = info.id;
    }
    return true;
}

/// The open camera of `thiz` with gLock held by the caller, or NULL with AOSP's RuntimeException after a release.
Context *live(JNIEnv *env, jobject thiz) {
    auto *context = reinterpret_cast<Context *>(env->GetLongField(thiz, gFields.context));
    if (context == nullptr) {
        jniThrowRuntimeException(env, "Camera is being used after Camera.release() was called");
    }
    return context;
}

jint GetNumberOfCameras(JNIEnv *, jclass, jint, jint) {
    return static_cast<jint>(camera_count());
}

void GetCameraInfo(JNIEnv *env, jclass, jint cameraId, jint, jint, jint, jobject info) {
    if (cameraId < 0 || static_cast<uint32_t>(cameraId) >= camera_count()) {
        __android_log_print(ANDROID_LOG_ERROR, "Camera-JNI", "%s: Unknown camera ID %d",
                            "android_hardware_Camera_getCameraInfo", cameraId);
        jniThrowRuntimeException(env, "Unknown camera ID");
        return;
    }
    minato::camera::Facts facts;
    if (!camera_facts(static_cast<uint32_t>(cameraId), &facts)) {
        jniThrowRuntimeException(env, "Fail to get camera info");
        return;
    }
    env->SetIntField(info, gFields.facing, facts.facing == 1 ? kFacingBack : kFacingFront);
    env->SetIntField(info, gFields.orientation, facts.orientation);
    env->SetBooleanField(info, gFields.can_disable_shutter_sound, JNI_TRUE);
}

jint Setup(JNIEnv *env, jobject thiz, jobject weak_this, jint cameraId, jstring, jint, jboolean, jint, jint) {
    if (cameraId < 0 || static_cast<uint32_t>(cameraId) >= camera_count()) return -EACCES;
    auto *context = new Context;
    if (!camera_facts(static_cast<uint32_t>(cameraId), &context->facts)) {
        delete context;
        return -EACCES;
    }
    context->initial = minato::camera::legacy_parameters(context->facts.info, minato::camera::kVideoBounds);
    context->parameters = context->initial;
    context->index = static_cast<uint32_t>(cameraId);
    const std::lock_guard<std::mutex> hold(gLock);
    if (context->initial.empty()) {
        delete context;
        return -EACCES;
    }
    // cameraservice evicts the older client of the same app.
    for (Context *older : gOpen) {
        if (older->index != context->index) continue;
        close_camera(older);
        older->evicted = true;
    }
    gOpen.erase(std::remove_if(gOpen.begin(), gOpen.end(), [](Context *each) { return each->evicted; }), gOpen.end());
    apkrt_camera call{};
    call.camera = context->index;
    if (camera_call(APKRT_CAMERA_OPEN, &call) != 0) {
        delete context;
        return -EACCES;  // Camera::connect's failure
    }
    context->id = call.id;
    env->GetJavaVM(&context->vm);
    context->weak = env->NewGlobalRef(weak_this);
    gOpen.push_back(context);
    env->SetLongField(thiz, gFields.context, reinterpret_cast<jlong>(context));
    return 0;
}

void Release(JNIEnv *env, jobject thiz) {
    Context *context;
    {
        const std::lock_guard<std::mutex> hold(gLock);
        context = reinterpret_cast<Context *>(env->GetLongField(thiz, gFields.context));
        env->SetLongField(thiz, gFields.context, 0);
        if (context == nullptr) return;  // released before, or never opened
        gOpen.erase(std::remove(gOpen.begin(), gOpen.end(), context), gOpen.end());
    }
    stop_preview(context);
    const std::lock_guard<std::mutex> hold(gLock);
    if (!context->evicted) close_camera(context);
    set_window(context, nullptr);
    for (jobject buffer : context->buffers) env->DeleteGlobalRef(buffer);
    if (context->weak != nullptr) env->DeleteGlobalRef(context->weak);
    delete context;
}

jstring GetParameters(JNIEnv *env, jobject thiz) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr) return nullptr;
    if (context->evicted) {
        jniThrowRuntimeException(env, "getParameters failed (empty parameters)");
        return nullptr;
    }
    return env->NewStringUTF(context->parameters.c_str());
}

void SetParameters(JNIEnv *env, jobject thiz, jstring params) {
    std::string asked;
    if (params != nullptr) {
        const char *chars = env->GetStringUTFChars(params, nullptr);
        if (chars == nullptr) return;
        asked = chars;
        env->ReleaseStringUTFChars(params, chars);
    }
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr) return;
    // While the preview runs, its size and format must stay the ones in force.
    const std::string &base = context->previewing ? context->parameters : context->initial;
    if (context->evicted || !minato::camera::accept_parameters(base, asked, context->previewing)) {
        jniThrowRuntimeException(env, "setParameters failed");
        return;
    }
    context->parameters = asked;
}

/// lock, unlock and reconnect: the camera stays the app's alone, so each succeeds while the Camera holds it.
void Keep(JNIEnv *env, jobject thiz, const char *exception, const char *message) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context != nullptr && context->evicted) jniThrowException(env, exception, message);
}

void Lock(JNIEnv *env, jobject thiz) { Keep(env, thiz, "java/lang/RuntimeException", "lock failed"); }
void Unlock(JNIEnv *env, jobject thiz) { Keep(env, thiz, "java/lang/RuntimeException", "unlock failed"); }
void Reconnect(JNIEnv *env, jobject thiz) { Keep(env, thiz, "java/io/IOException", "reconnect failed"); }

void SetPreviewWindow(JNIEnv *env, jobject thiz, ANativeWindow *window, const char *what) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr) {
        if (window != nullptr) ANativeWindow_release(window);
        return;
    }
    if (!set_window(context, window)) jniThrowException(env, "java/io/IOException", what);
}

void SetPreviewTexture(JNIEnv *env, jobject thiz, jobject texture) {
    ANativeWindow *window = texture != nullptr ? minato_surface_texture_window(env, texture) : nullptr;
    SetPreviewWindow(env, thiz, window, "setPreviewTexture failed");
}

void SetPreviewSurface(JNIEnv *env, jobject thiz, jobject surface) {
    ANativeWindow *window = surface != nullptr ? minato_surface_window(env, surface) : nullptr;
    SetPreviewWindow(env, thiz, window, "setPreviewDisplay failed");
}

void StartPreview(JNIEnv *env, jobject thiz) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr || context->previewing) return;
    int32_t width = 0, height = 0;
    std::sscanf(value_of(context->parameters, "preview-size").c_str(), "%dx%d", &width, &height);
    const apkrt_camera_stream streams[2] = {
            {APKRT_CAMERA_FORMAT_PRIVATE, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0},
            {APKRT_CAMERA_FORMAT_YUV_420_888, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0}};
    apkrt_camera call{};
    call.id = context->id;
    call.count = 2;
    call.data = reinterpret_cast<uintptr_t>(streams);
    const apkrt_camera_controls controls = controls_of(*context);
    apkrt_camera request{};
    request.id = context->id;
    request.data = reinterpret_cast<uintptr_t>(&controls);
    request.targets = 3;
    request.flags = APKRT_CAMERA_REPEATING;
    if (context->evicted || camera_call(APKRT_CAMERA_CONFIGURE, &call) != 0 ||
        camera_call(APKRT_CAMERA_REQUEST, &request) != 0) {
        jniThrowRuntimeException(env, "startPreview failed");
        return;
    }
    context->width = width, context->height = height;
    context->stopping = false;
    context->previewing = true;
    context->thread_made = pthread_create(&context->thread, nullptr, preview_thread, context) == 0;
}

void StopPreview(JNIEnv *env, jobject thiz) {
    Context *context;
    {
        const std::lock_guard<std::mutex> hold(gLock);
        context = live(env, thiz);
    }
    if (context != nullptr) stop_preview(context);
}

jboolean PreviewEnabled(JNIEnv *env, jobject thiz) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    return context != nullptr && context->previewing ? JNI_TRUE : JNI_FALSE;
}

/// autoFocus: a fixed lens reports success at once (as Camera2Client's FOCUS_MODE_FIXED); else a trigger, whose
/// end the preview's frames show.
void AutoFocus(JNIEnv *env, jobject thiz) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr) return;
    const std::string focus = value_of(context->parameters, "focus-mode");
    if (focus == "fixed" || focus == "infinity" || !context->previewing) {
        env->CallStaticVoidMethod(gFields.camera, gFields.post, context->weak, kMessageFocus, 1, 0, nullptr);
        return;
    }
    apkrt_camera_controls controls = controls_of(*context);
    controls.af_trigger = 1;  // CONTROL_AF_TRIGGER_START
    apkrt_camera request{};
    request.id = context->id;
    request.data = reinterpret_cast<uintptr_t>(&controls);
    request.targets = 1;
    request.number = kFocusRequest;
    if (camera_call(APKRT_CAMERA_REQUEST, &request) != 0) {
        jniThrowRuntimeException(env, "autoFocus failed");
        return;
    }
    context->focusing = true;
    context->focus_seen = false;
    context->focus_number = kFocusRequest;
}

void CancelAutoFocus(JNIEnv *env, jobject thiz) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context != nullptr) context->focusing = false;
}

/// A picture's capture: its callbacks posted in Android's order, shutter, raw (no data) and the JPEG.
struct Picture {
    Context *context;
    jint messages;
};

void *picture_thread(void *argument) {
    auto *picture = static_cast<Picture *>(argument);
    Context *context = picture->context;
    JNIEnv *env = nullptr;
    JavaVMAttachArgs attach{JNI_VERSION_1_6, "CameraPicture", nullptr};
    if (context->vm->AttachCurrentThread(&env, &attach) != JNI_OK) {
        delete picture;
        return nullptr;
    }
    const auto post = [&](jint message, jobject data) {
        if ((picture->messages & message) == 0) return;
        env->CallStaticVoidMethod(gFields.camera, gFields.post, context->weak, message, 0, 0, data);
        if (env->ExceptionCheck()) env->ExceptionClear();
    };
    post(kMessageShutter, nullptr);
    std::vector<uint8_t> jpeg;
    for (int tries = 0; tries < 5000; tries++) {  // at most 10 s
        apkrt_camera_frame frame{};
        frame.jpeg = reinterpret_cast<uintptr_t>(jpeg.data());
        frame.jpeg_capacity = jpeg.size();
        apkrt_camera call{};
        call.id = context->id;
        call.data = reinterpret_cast<uintptr_t>(&frame);
        const long got = camera_call(APKRT_CAMERA_FRAME, &call);
        if (got == -ENOSPC) {
            jpeg.resize(static_cast<size_t>(frame.jpeg_size));
            continue;
        }
        if (got == -EAGAIN) {
            const timespec wait{0, 2000000};
            nanosleep(&wait, nullptr);
            continue;
        }
        if (got != 0) break;
        for (const uint64_t token : frame.tokens) {
            if (token == 0) continue;
            apkrt_camera discard{};
            discard.id = context->id;
            discard.data = token;
            camera_call(APKRT_CAMERA_DISCARD, &discard);
        }
        if (frame.jpeg_size == 0) continue;
        post(kMessageRawImage, nullptr);  // no raw image: its callback gets null, as Camera2Client's
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(frame.jpeg_size));
        if (bytes != nullptr) {
            env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(frame.jpeg_size),
                                    reinterpret_cast<const jbyte *>(jpeg.data()));
            post(kMessageCompressedImage, bytes);
            env->DeleteLocalRef(bytes);
        }
        break;
    }
    delete picture;
    context->vm->DetachCurrentThread();
    return nullptr;
}

/// takePicture: only while the preview runs, which stops for the picture, as Camera2Client's.
void TakePicture(JNIEnv *env, jobject thiz, jint messages) {
    Context *context;
    {
        const std::lock_guard<std::mutex> hold(gLock);
        context = live(env, thiz);
        if (context == nullptr) return;
        if (!context->previewing || context->evicted) {
            jniThrowRuntimeException(env, "takePicture failed");
            return;
        }
    }
    stop_preview(context);
    const std::lock_guard<std::mutex> hold(gLock);
    int32_t width = 0, height = 0;
    std::sscanf(value_of(context->parameters, "picture-size").c_str(), "%dx%d", &width, &height);
    const apkrt_camera_stream streams[1] = {
            {APKRT_CAMERA_FORMAT_JPEG, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0}};
    apkrt_camera call{};
    call.id = context->id;
    call.count = 1;
    call.data = reinterpret_cast<uintptr_t>(streams);
    apkrt_camera_controls controls = controls_of(*context);
    int quality = 90, rotation = 0;
    std::sscanf(value_of(context->parameters, "jpeg-quality").c_str(), "%d", &quality);
    std::sscanf(value_of(context->parameters, "rotation").c_str(), "%d", &rotation);
    controls.jpeg_quality = static_cast<uint8_t>(quality);
    controls.jpeg_orientation = rotation;
    apkrt_camera request{};
    request.id = context->id;
    request.data = reinterpret_cast<uintptr_t>(&controls);
    request.targets = 1;
    request.number = 3;
    pthread_t thread;
    auto *picture = new Picture{context, messages};
    if (camera_call(APKRT_CAMERA_CONFIGURE, &call) != 0 || camera_call(APKRT_CAMERA_REQUEST, &request) != 0 ||
        pthread_create(&thread, nullptr, picture_thread, picture) != 0) {
        delete picture;
        jniThrowRuntimeException(env, "takePicture failed");
        return;
    }
    pthread_detach(thread);
}

void SetHasPreviewCallback(JNIEnv *env, jobject thiz, jboolean installed, jboolean manual) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr) return;
    context->callback = installed;
    context->manual = manual;
    if (!installed) {  // JNICameraContext::setCallbackMode clears the buffers
        for (jobject buffer : context->buffers) env->DeleteGlobalRef(buffer);
        context->buffers.clear();
    }
}

void AddCallbackBuffer(JNIEnv *env, jobject thiz, jbyteArray bytes, jint message) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr || bytes == nullptr || message != kMessagePreviewFrame) return;
    context->buffers.push_back(env->NewGlobalRef(bytes));
}

const JNINativeMethod gCameraMethods[] = {
        {"_getNumberOfCameras", "(II)I", reinterpret_cast<void *>(GetNumberOfCameras)},
        {"_getCameraInfo", "(IIIILandroid/hardware/Camera$CameraInfo;)V", reinterpret_cast<void *>(GetCameraInfo)},
        {"native_setup", "(Ljava/lang/Object;ILjava/lang/String;IZII)I", reinterpret_cast<void *>(Setup)},
        {"native_release", "()V", reinterpret_cast<void *>(Release)},
        {"native_getParameters", "()Ljava/lang/String;", reinterpret_cast<void *>(GetParameters)},
        {"native_setParameters", "(Ljava/lang/String;)V", reinterpret_cast<void *>(SetParameters)},
        {"setPreviewSurface", "(Landroid/view/Surface;)V", reinterpret_cast<void *>(SetPreviewSurface)},
        {"setPreviewTexture", "(Landroid/graphics/SurfaceTexture;)V", reinterpret_cast<void *>(SetPreviewTexture)},
        {"startPreview", "()V", reinterpret_cast<void *>(StartPreview)},
        {"_stopPreview", "()V", reinterpret_cast<void *>(StopPreview)},
        {"previewEnabled", "()Z", reinterpret_cast<void *>(PreviewEnabled)},
        {"setHasPreviewCallback", "(ZZ)V", reinterpret_cast<void *>(SetHasPreviewCallback)},
        {"_addCallbackBuffer", "([BI)V", reinterpret_cast<void *>(AddCallbackBuffer)},
        {"native_autoFocus", "()V", reinterpret_cast<void *>(AutoFocus)},
        {"native_cancelAutoFocus", "()V", reinterpret_cast<void *>(CancelAutoFocus)},
        {"native_takePicture", "(I)V", reinterpret_cast<void *>(TakePicture)},
        {"lock", "()V", reinterpret_cast<void *>(Lock)},
        {"unlock", "()V", reinterpret_cast<void *>(Unlock)},
        {"reconnect", "()V", reinterpret_cast<void *>(Reconnect)},
};

}  // namespace

int register_android_hardware_Camera(JNIEnv *env) {
    jclass camera = FindClassOrDie(env, "android/hardware/Camera");
    gFields.context = GetFieldIDOrDie(env, camera, "mNativeContext", "J");
    gFields.camera = MakeGlobalRefOrDie(env, camera);
    gFields.post = GetStaticMethodIDOrDie(env, camera, "postEventFromNative",
                                          "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    jclass info = FindClassOrDie(env, "android/hardware/Camera$CameraInfo");
    gFields.facing = GetFieldIDOrDie(env, info, "facing", "I");
    gFields.orientation = GetFieldIDOrDie(env, info, "orientation", "I");
    gFields.can_disable_shutter_sound = GetFieldIDOrDie(env, info, "canDisableShutterSound", "Z");
    return RegisterMethodsOrDie(env, "android/hardware/Camera", gCameraMethods, NELEM(gCameraMethods));
}

}  // namespace android
