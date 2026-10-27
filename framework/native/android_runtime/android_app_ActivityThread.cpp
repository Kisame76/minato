// Ours (ADR-0039 b3.4c): ActivityThread's natives. The first hands our driver the session's configuration as
// NativeActivity gets it. libandroid's minato_AConfiguration_fromSession (version APKRT_PRIVATE) fills an
// AConfiguration with ADR-0028's values for the session that the host began. The native copies what Java's
// Configuration and DisplayMetrics hold in the same encoding: the AConfiguration getters' values, with the screen
// layout's and UI mode's parts at their Java bits (ResTable_config's encoding, which Configuration shares), and the
// session's size in pixels. ActivityThread adds what only Java has.
// Every libandroid function is looked up through dlopen at the first call, the getters included, so that
// libandroid_runtime does not need libandroid.so: on Android the dependency runs the other way (libandroid's
// AAssetManager_fromJava and ANativeWindow_fromSurface call into libandroid_runtime), and ours will too in step c.
// A function libandroid lacks throws UnsatisfiedLinkError with its name.
// Since ADR-0043 step c1 the others connect the driver to the host's session as activity-host connects (ADR-0028):
// the event pipe's descriptor, one record read at a time, each event answered, the FINISH request, and the end; since
// ADR-0043 step c4 also SurfaceView's surface request, whose struct the native fills, and since ADR-0045 step 7b2
// the input channel's connect, whose descriptor becomes an InputChannel (android_view_InputChannel.cpp).
#include <android/configuration.h>
#include <dlfcn.h>
#include <stddef.h>
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/JNIPlatformHelp.h>
#include <nativehelper/scoped_utf_chars.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include <APKRuntime/guest_interface.h>

#include "core_jni_helpers.h"
#include "minato_input_channel.h"

// The libandroid functions the native calls, with the NDK header's types.
#define LIBANDROID_FUNCTIONS(X)                                                                                     \
    X(AConfiguration_new) X(AConfiguration_delete) X(minato_AConfiguration_fromSession) X(AConfiguration_getDensity) \
    X(AConfiguration_getOrientation) X(AConfiguration_getTouchscreen) X(AConfiguration_getKeyboard)               \
    X(AConfiguration_getNavigation) X(AConfiguration_getScreenSize) X(AConfiguration_getScreenLong)               \
    X(AConfiguration_getLayoutDirection) X(AConfiguration_getScreenRound) X(AConfiguration_getUiModeType)         \
    X(AConfiguration_getUiModeNight) X(AConfiguration_getScreenWidthDp) X(AConfiguration_getScreenHeightDp)       \
    X(AConfiguration_getSmallestScreenWidthDp) X(AConfiguration_getLanguage) X(AConfiguration_getCountry)

namespace android {
namespace {

struct Libandroid {
#define FIELD(name) decltype(&::name) name = nullptr;
    LIBANDROID_FUNCTIONS(FIELD)
#undef FIELD
    // What could not be found: libandroid.so's dlerror or a function's name; empty when everything was found.
    char missing[256] = "";
};

const Libandroid &libandroid() {
    static const Libandroid functions = [] {
        Libandroid found;
        void *library = dlopen("libandroid.so", RTLD_NOW);
        if (library == nullptr) {
            snprintf(found.missing, sizeof found.missing, "%s", dlerror());
            return found;
        }
#define LOOK_UP(name)                                                                     \
    found.name = reinterpret_cast<decltype(found.name)>(dlsym(library, #name));           \
    if (found.name == nullptr && found.missing[0] == 0) {                                 \
        snprintf(found.missing, sizeof found.missing, "libandroid.so has no %s", #name); \
    }
        LIBANDROID_FUNCTIONS(LOOK_UP)
#undef LOOK_UP
        return found;
    }();
    return functions;
}

struct {
    jfieldID densityDpi, orientation, touchscreen, keyboard, navigation, screenLayout, uiMode, screenWidthDp,
            screenHeightDp, smallestScreenWidthDp;
} gConfiguration;

struct {
    jfieldID widthPixels, heightPixels;
} gDisplayMetrics;

// Fills `config` and `metrics` from the session and returns its locale as a language tag ("en-US"), or null
// without a session; throws for any other error of the session's configuration.
jstring SessionConfiguration(JNIEnv *env, jclass, jobject config, jobject metrics) {
    const Libandroid &lib = libandroid();
    if (lib.missing[0] != 0) {
        jniThrowException(env, "java/lang/UnsatisfiedLinkError", lib.missing);
        return nullptr;
    }
    AConfiguration *values = lib.AConfiguration_new();
    if (values == nullptr) {
        jniThrowException(env, "java/lang/OutOfMemoryError", "AConfiguration_new");
        return nullptr;
    }
    apkrt_configuration session{};
    // ENOENT: the host began no session, and the defaults stay. Any other error is the runtime's and throws.
    if (const int error = lib.minato_AConfiguration_fromSession(values, &session); error != 0) {
        lib.AConfiguration_delete(values);
        if (error != ENOENT) {
            jniThrowExceptionFmt(env, "java/lang/IllegalStateException", "session_configuration: %s",
                                 strerror(error));
        }
        return nullptr;
    }
    env->SetIntField(config, gConfiguration.densityDpi, lib.AConfiguration_getDensity(values));
    env->SetIntField(config, gConfiguration.orientation, lib.AConfiguration_getOrientation(values));
    env->SetIntField(config, gConfiguration.touchscreen, lib.AConfiguration_getTouchscreen(values));
    env->SetIntField(config, gConfiguration.keyboard, lib.AConfiguration_getKeyboard(values));
    env->SetIntField(config, gConfiguration.navigation, lib.AConfiguration_getNavigation(values));
    env->SetIntField(config, gConfiguration.screenLayout,
                     lib.AConfiguration_getScreenSize(values) | lib.AConfiguration_getScreenLong(values) << 4 |
                             lib.AConfiguration_getLayoutDirection(values) << 6 |
                             lib.AConfiguration_getScreenRound(values) << 8);
    env->SetIntField(config, gConfiguration.uiMode,
                     lib.AConfiguration_getUiModeType(values) | lib.AConfiguration_getUiModeNight(values) << 4);
    env->SetIntField(config, gConfiguration.screenWidthDp, lib.AConfiguration_getScreenWidthDp(values));
    env->SetIntField(config, gConfiguration.screenHeightDp, lib.AConfiguration_getScreenHeightDp(values));
    env->SetIntField(config, gConfiguration.smallestScreenWidthDp,
                     lib.AConfiguration_getSmallestScreenWidthDp(values));
    env->SetIntField(metrics, gDisplayMetrics.widthPixels, session.width);
    env->SetIntField(metrics, gDisplayMetrics.heightPixels, session.height);

    char language[3] = {}, country[3] = {}, tag[8];
    lib.AConfiguration_getLanguage(values, language);
    lib.AConfiguration_getCountry(values, country);
    lib.AConfiguration_delete(values);
    snprintf(tag, sizeof tag, country[0] != 0 ? "%.2s-%.2s" : "%.2s", language, country);
    return env->NewStringUTF(tag);
}

// The event pipe as a FileDescriptor, for MessageQueue's listener, or null without a session (-ENOENT); throws
// IllegalStateException for any other error.
jobject SessionConnect(JNIEnv *env, jclass) {
    apkrt_configuration configuration{};
    const long events = syscall(APKRT_NR_SESSION_CONNECT, &configuration);
    if (events < 0) {
        if (errno != ENOENT) {
            jniThrowExceptionFmt(env, "java/lang/IllegalStateException", "session_connect: %s", strerror(errno));
        }
        return nullptr;
    }
    return jniCreateFileDescriptor(env, static_cast<int>(events));
}

// Reads one struct apkrt_event into event[0..4]: type, serial and the three arguments. False when the pipe broke.
jboolean SessionRead(JNIEnv *env, jclass, jobject fileDescriptor, jlongArray event) {
    const int fd = jniGetFDFromFileDescriptor(env, fileDescriptor);
    apkrt_event record;
    ssize_t got;
    do {
        got = read(fd, &record, sizeof record);
    } while (got < 0 && errno == EINTR);
    if (got != static_cast<ssize_t>(sizeof record)) return JNI_FALSE;
    const jlong values[5] = {record.type, record.serial, static_cast<jlong>(record.arguments[0]),
                             static_cast<jlong>(record.arguments[1]), static_cast<jlong>(record.arguments[2])};
    env->SetLongArrayRegion(event, 0, 5, values);
    return JNI_TRUE;
}

void SessionAnswer(JNIEnv *, jclass, jint serial) {
    syscall(APKRT_NR_SESSION_ANSWER, static_cast<uint32_t>(serial));
}

// session_request: 0, or the negative errno.
jint SessionRequest(JNIEnv *, jclass, jint type, jlong a0, jlong a1) {
    return syscall(APKRT_NR_SESSION_REQUEST, type, a0, a1) < 0 ? -errno : 0;
}

// The request's layout as the runtime reads it (guest_interface.h).
static_assert(sizeof(apkrt_surface_request) == 48 && offsetof(apkrt_surface_request, token) == 8 &&
                      offsetof(apkrt_surface_request, left) == 16 && offsetof(apkrt_surface_request, opaque) == 44,
              "struct apkrt_surface_request");

// APKRT_REQUEST_SURFACE (ADR-0043 step c4): SurfaceView's make, place or remove, its struct by address, which the
// runtime checks and hands the host; 0, or the negative errno.
jint SessionRequestSurface(JNIEnv *, jclass, jint op, jint z, jlong token, jint left, jint top, jint right,
                           jint bottom, jint width, jint height, jint format) {
    apkrt_surface_request request = {};
    request.op = static_cast<uint32_t>(op);
    request.z = z;
    request.token = static_cast<uint64_t>(token);
    request.left = left;
    request.top = top;
    request.right = right;
    request.bottom = bottom;
    request.width = width;
    request.height = height;
    request.format = format;
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_SURFACE, reinterpret_cast<uintptr_t>(&request), 0) < 0
                   ? -errno
                   : 0;
}

// The window request's layout as the runtime reads it (guest_interface.h, ADR-0041 gap 4).
static_assert(sizeof(apkrt_window_request) == 72 && offsetof(apkrt_window_request, token) == 8 &&
                      offsetof(apkrt_window_request, parent) == 16 && offsetof(apkrt_window_request, left) == 24 &&
                      offsetof(apkrt_window_request, width) == 40 && offsetof(apkrt_window_request, format) == 48 &&
                      offsetof(apkrt_window_request, opaque) == 52 && offsetof(apkrt_window_request, group) == 56 &&
                      offsetof(apkrt_window_request, alpha) == 60 && offsetof(apkrt_window_request, dim) == 64 &&
                      offsetof(apkrt_window_request, reserved) == 68,
              "struct apkrt_window_request");

// APKRT_REQUEST_WINDOW (ADR-0041 gap 4 (b)): a requested window's make, place or remove, its struct by address,
// which the runtime checks and hands the host; 0, or the negative errno. `alpha` and `dim` are floats' bits.
jint SessionRequestWindow(JNIEnv *, jclass, jint op, jint type, jlong token, jlong parent, jint left, jint top,
                          jint right, jint bottom, jint width, jint height, jint format, jint group, jint alpha,
                          jint dim) {
    apkrt_window_request request = {};
    request.op = static_cast<uint32_t>(op);
    request.type = type;
    request.token = static_cast<uint64_t>(token);
    request.parent = static_cast<uint64_t>(parent);
    request.left = left;
    request.top = top;
    request.right = right;
    request.bottom = bottom;
    request.width = width;
    request.height = height;
    request.format = format;
    request.group = static_cast<uint32_t>(group);
    request.alpha = static_cast<uint32_t>(alpha);
    request.dim = static_cast<uint32_t>(dim);
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW, reinterpret_cast<uintptr_t>(&request), 0) < 0
                   ? -errno
                   : 0;
}

// APKRT_REQUEST_PICK (ADR-0057 step a): a picker's request, its text (UTF-8 bytes, not JNI's modified UTF-8, which
// writes a character above U+FFFF as two surrogates) by address and its ID; 0, or the negative errno.
jint SessionRequestPick(JNIEnv *env, jclass, jlong id, jbyteArray text) {
    if (text == nullptr) return -EINVAL;
    const jsize length = env->GetArrayLength(text);
    std::vector<char> bytes(static_cast<size_t>(length) + 1, '\0');
    env->GetByteArrayRegion(text, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return -EFAULT;
    }
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, reinterpret_cast<uintptr_t>(bytes.data()),
                   static_cast<uint64_t>(id)) < 0
                   ? -errno
                   : 0;
}

// APKRT_REQUEST_PICK_CANCEL: the request `id` is not wanted any more; 0, or the negative errno.
jint SessionRequestPickCancel(JNIEnv *, jclass, jlong id) {
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, static_cast<uint64_t>(id), 0) < 0 ? -errno : 0;
}

// APKRT_REQUEST_OPEN (ADR-0062): the text of an open request (UTF-8 bytes, as the pick's) by address and its ID; 0, or
// the negative errno.
jint SessionRequestOpen(JNIEnv *env, jclass, jlong id, jbyteArray text) {
    if (text == nullptr) return -EINVAL;
    const jsize length = env->GetArrayLength(text);
    std::vector<char> bytes(static_cast<size_t>(length) + 1, '\0');
    env->GetByteArrayRegion(text, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return -EFAULT;
    }
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, reinterpret_cast<uintptr_t>(bytes.data()),
                   static_cast<uint64_t>(id)) < 0
                   ? -errno
                   : 0;
}

// APKRT_REQUEST_OPEN_CANCEL: the request `id` is not wanted any more; 0, or the negative errno.
jint SessionRequestOpenCancel(JNIEnv *, jclass, jlong id) {
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, static_cast<uint64_t>(id), 0) < 0 ? -errno : 0;
}

// APKRT_REQUEST_PERMISSION (ADR-0063): the text "<group>\t<detail>" of a permission request by address and its ID;
// 0, or the negative errno.
jint SessionRequestPermission(JNIEnv *env, jclass, jlong id, jbyteArray text) {
    if (text == nullptr) return -EINVAL;
    const jsize length = env->GetArrayLength(text);
    std::vector<char> bytes(static_cast<size_t>(length) + 1, '\0');
    env->GetByteArrayRegion(text, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return -EFAULT;
    }
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, reinterpret_cast<uintptr_t>(bytes.data()),
                   static_cast<uint64_t>(id)) < 0
                   ? -errno
                   : 0;
}

// APKRT_REQUEST_PERMISSION_CANCEL: the request `id` is not wanted any more; 0, or the negative errno.
jint SessionRequestPermissionCancel(JNIEnv *, jclass, jlong id) {
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, static_cast<uint64_t>(id), 0) < 0 ? -errno
                                                                                                         : 0;
}

// The location hub (ADR-0063): APKRT_NR_LOCATION's operations, 0 or the negative errno.
jint LocationStart(JNIEnv *, jclass, jint accuracy, jint distance) {
    apkrt_location_control control = {accuracy, distance};
    return syscall(APKRT_NR_LOCATION, APKRT_LOCATION_START, reinterpret_cast<uintptr_t>(&control), 0) < 0 ? -errno : 0;
}

// APKRT_LOCATION_PERMISSION: the stored outcome of a permission group (0 denied or unanswered, 1 granted, 2 granted
// coarse), or the negative errno.
jint LocationGrant(JNIEnv *, jclass, jint group) {
    const long result = syscall(APKRT_NR_LOCATION, APKRT_LOCATION_PERMISSION, static_cast<uint64_t>(group), 0);
    return result < 0 ? -errno : static_cast<jint>(result);
}

jint LocationStop(JNIEnv *, jclass) {
    return syscall(APKRT_NR_LOCATION, APKRT_LOCATION_STOP, 0, 0) < 0 ? -errno : 0;
}

jint LocationCurrent(JNIEnv *, jclass, jlong token) {
    return syscall(APKRT_NR_LOCATION, APKRT_LOCATION_CURRENT, static_cast<uint64_t>(token), 0) < 0 ? -errno : 0;
}

jint LocationCancel(JNIEnv *, jclass, jlong token) {
    return syscall(APKRT_NR_LOCATION, APKRT_LOCATION_CANCEL, static_cast<uint64_t>(token), 0) < 0 ? -errno : 0;
}

// GET or LAST into nine doubles (latitude, longitude, altitude, the horizontal and vertical accuracy, the speed, its
// accuracy, the bearing and its accuracy) and four longs (time in ms, elapsed realtime in ns, provider, the has-bits);
// 1 when filled, 0 for none, or the negative errno.
jint LocationFix(JNIEnv *env, jclass, jboolean last, jlong sequence, jdoubleArray doubles, jlongArray longs) {
    if (doubles == nullptr || longs == nullptr || env->GetArrayLength(doubles) < 9 || env->GetArrayLength(longs) < 4) {
        return -EINVAL;
    }
    apkrt_location_fix fix{};
    const long result = syscall(APKRT_NR_LOCATION, last ? APKRT_LOCATION_LAST : APKRT_LOCATION_GET,
                                reinterpret_cast<uintptr_t>(&fix), static_cast<uint64_t>(sequence));
    if (result < 0) return -errno;
    if (result != 1) return 0;
    const jdouble d[9] = {fix.latitude, fix.longitude, fix.altitude, fix.horizontal_accuracy_m, fix.vertical_accuracy_m,
                          fix.speed_mps, fix.speed_accuracy_mps, fix.bearing_deg, fix.bearing_accuracy_deg};
    const jlong l[4] = {fix.time_ms, fix.elapsed_realtime_ns, fix.provider, fix.valid};
    env->SetDoubleArrayRegion(doubles, 0, 9, d);
    env->SetLongArrayRegion(longs, 0, 4, l);
    return env->ExceptionCheck() ? -EFAULT : 1;
}

// APKRT_REQUEST_EXPORT_MEDIA (ADR-0057 step e): the text of an export request (UTF-8 bytes, as the pick's) by address;
// 0, or the negative errno.
jint SessionRequestExportMedia(JNIEnv *env, jclass, jbyteArray text) {
    if (text == nullptr) return -EINVAL;
    const jsize length = env->GetArrayLength(text);
    std::vector<char> bytes(static_cast<size_t>(length) + 1, '\0');
    env->GetByteArrayRegion(text, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return -EFAULT;
    }
    return syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_EXPORT_MEDIA, reinterpret_cast<uintptr_t>(bytes.data()),
                   0) < 0
                   ? -errno
                   : 0;
}

// input_connect of the session's input channel `id` (ADR-0045 step 7b2), as an InputChannel named `name`; null
// when the runtime refuses the ID, or with the exception pending when the name cannot be read, which is read
// first, as AOSP's natives read theirs before they open a channel.
jobject SessionConnectInput(JNIEnv *env, jclass, jlong id, jstring name) {
    ScopedUtfChars label(env, name);
    if (label.c_str() == nullptr) return nullptr;
    const long fd = syscall(APKRT_NR_INPUT_CONNECT, static_cast<uint64_t>(id));
    if (fd < 0) return nullptr;
    return android_view_InputChannel_createJavaObject(env, static_cast<int>(fd), label.c_str());
}

[[noreturn]] void Exit(JNIEnv *, jclass, jint status) {
    _exit(status);
}

const JNINativeMethod gActivityThreadMethods[] = {
        {"nativeSessionConfiguration",
         "(Landroid/content/res/Configuration;Landroid/util/DisplayMetrics;)Ljava/lang/String;",
         reinterpret_cast<void *>(SessionConfiguration)},
        {"nativeSessionConnect", "()Ljava/io/FileDescriptor;", reinterpret_cast<void *>(SessionConnect)},
        {"nativeSessionRead", "(Ljava/io/FileDescriptor;[J)Z", reinterpret_cast<void *>(SessionRead)},
        {"nativeSessionAnswer", "(I)V", reinterpret_cast<void *>(SessionAnswer)},
        {"nativeSessionRequest", "(IJJ)I", reinterpret_cast<void *>(SessionRequest)},
        {"nativeSessionRequestSurface", "(IIJIIIIIII)I", reinterpret_cast<void *>(SessionRequestSurface)},
        {"nativeSessionRequestWindow", "(IIJJIIIIIIIIII)I", reinterpret_cast<void *>(SessionRequestWindow)},
        {"nativeSessionRequestPick", "(J[B)I", reinterpret_cast<void *>(SessionRequestPick)},
        {"nativeSessionRequestPickCancel", "(J)I", reinterpret_cast<void *>(SessionRequestPickCancel)},
        {"nativeSessionRequestOpen", "(J[B)I", reinterpret_cast<void *>(SessionRequestOpen)},
        {"nativeSessionRequestOpenCancel", "(J)I", reinterpret_cast<void *>(SessionRequestOpenCancel)},
        {"nativeSessionRequestPermission", "(J[B)I", reinterpret_cast<void *>(SessionRequestPermission)},
        {"nativeSessionRequestPermissionCancel", "(J)I", reinterpret_cast<void *>(SessionRequestPermissionCancel)},
        {"nativeLocationStart", "(II)I", reinterpret_cast<void *>(LocationStart)},
        {"nativeLocationGrant", "(I)I", reinterpret_cast<void *>(LocationGrant)},
        {"nativeLocationStop", "()I", reinterpret_cast<void *>(LocationStop)},
        {"nativeLocationCurrent", "(J)I", reinterpret_cast<void *>(LocationCurrent)},
        {"nativeLocationCancel", "(J)I", reinterpret_cast<void *>(LocationCancel)},
        {"nativeLocationFix", "(ZJ[D[J)I", reinterpret_cast<void *>(LocationFix)},
        {"nativeSessionRequestExportMedia", "([B)I", reinterpret_cast<void *>(SessionRequestExportMedia)},
        {"nativeSessionConnectInput", "(JLjava/lang/String;)Landroid/view/InputChannel;",
         reinterpret_cast<void *>(SessionConnectInput)},
        {"nativeExit", "(I)V", reinterpret_cast<void *>(Exit)},
};

}  // namespace

int register_android_app_ActivityThread(JNIEnv *env) {
    jclass configuration = FindClassOrDie(env, "android/content/res/Configuration");
    gConfiguration.densityDpi = GetFieldIDOrDie(env, configuration, "densityDpi", "I");
    gConfiguration.orientation = GetFieldIDOrDie(env, configuration, "orientation", "I");
    gConfiguration.touchscreen = GetFieldIDOrDie(env, configuration, "touchscreen", "I");
    gConfiguration.keyboard = GetFieldIDOrDie(env, configuration, "keyboard", "I");
    gConfiguration.navigation = GetFieldIDOrDie(env, configuration, "navigation", "I");
    gConfiguration.screenLayout = GetFieldIDOrDie(env, configuration, "screenLayout", "I");
    gConfiguration.uiMode = GetFieldIDOrDie(env, configuration, "uiMode", "I");
    gConfiguration.screenWidthDp = GetFieldIDOrDie(env, configuration, "screenWidthDp", "I");
    gConfiguration.screenHeightDp = GetFieldIDOrDie(env, configuration, "screenHeightDp", "I");
    gConfiguration.smallestScreenWidthDp = GetFieldIDOrDie(env, configuration, "smallestScreenWidthDp", "I");
    jclass metrics = FindClassOrDie(env, "android/util/DisplayMetrics");
    gDisplayMetrics.widthPixels = GetFieldIDOrDie(env, metrics, "widthPixels", "I");
    gDisplayMetrics.heightPixels = GetFieldIDOrDie(env, metrics, "heightPixels", "I");
    return RegisterMethodsOrDie(env, "android/app/ActivityThread", gActivityThreadMethods,
                                NELEM(gActivityThreadMethods));
}

}  // namespace android
