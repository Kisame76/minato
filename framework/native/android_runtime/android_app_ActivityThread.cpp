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
// the event pipe's descriptor, one record read at a time, each event answered, the FINISH request, and the end.
#include <android/configuration.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/JNIPlatformHelp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <APKRuntime/guest_interface.h>

#include "core_jni_helpers.h"

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
