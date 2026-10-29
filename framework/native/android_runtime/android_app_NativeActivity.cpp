// Ours in place of AOSP's core/jni/android_app_NativeActivity.cpp (ADR-0069 step 2): the natives of AOSP's
// NativeActivity.java, which starts a NativeActivity app's library on the Java path, with ART's JavaVM, the main
// thread's JNIEnv and the activity's object in its ANativeActivity, so that the app's JNI calls reach the framework.
// As AOSP's: loadNativeCode opens the library through libnativeloader's OpenNativeLibrary with the activity's class
// loader and library path, makes the NativeCode (an ANativeActivity with the callbacks, the paths, the SDK version and
// the asset manager of the Java AssetManager) and calls the library's create function on the main thread; the
// lifecycle's, the focus's, the content rectangle's and the surface's natives call the app's callbacks, the surface's
// with the ANativeWindow of the Java Surface (libandroid's ANativeWindow_fromSurface, android_view_Surface.cpp's
// minato_ANativeWindow_fromSurface), resized only when its size changed. libandroid's ANativeActivity_finish,
// _setWindowFormat, _setWindowFlags, _showSoftInput and _hideSoftInput, from any thread, come here through the
// android_NativeActivity_* functions this library exports: they write a command to a pipe that the main MessageQueue's
// looper watches, whose callback calls the activity's finish, setWindowFormat, setWindowFlags, showIme and hideIme,
// and leaves an exception of those to the looper's Java caller. unloadNativeCode calls onDestroy and never closes the
// library, as Android, whose process ends with the app (one app per process). The input queue's two natives are
// step 3's.
#define LOG_TAG "NativeActivity"

#include <android/asset_manager_jni.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedUtfChars.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>

#include <memory>
#include <string>

#include "core_jni_helpers.h"
#include "minato_message_queue.h"

#define LIBANDROID_FUNCTIONS(X) \
    X(ALooper_acquire) X(ALooper_release) X(ALooper_addFd) X(ALooper_removeFd) X(AAssetManager_fromJava)

namespace android {

ANativeWindow *minato_surface_window(JNIEnv *env, jobject surface);

namespace {

/// libandroid's functions, through dlopen, as the other natives of ours take them (android_hardware_SensorManager.cpp).
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
    jmethodID finish;
    jmethodID setWindowFlags;
    jmethodID setWindowFormat;
    jmethodID showIme;
    jmethodID hideIme;
} gNativeActivityClassInfo;

enum Command : int32_t {
    CMD_FINISH = 1,
    CMD_SET_WINDOW_FORMAT,
    CMD_SET_WINDOW_FLAGS,
    CMD_SHOW_SOFT_INPUT,
    CMD_HIDE_SOFT_INPUT,
};

struct ActivityWork {
    int32_t cmd;
    int32_t arg1;
    int32_t arg2;
};

void write_work(int fd, int32_t cmd, int32_t arg1 = 0, int32_t arg2 = 0) {
    const ActivityWork work{cmd, arg1, arg2};
    ssize_t written;
    do {
        written = write(fd, &work, sizeof work);
    } while (written < 0 && errno == EINTR);
    if (written < 0) {
        ALOGW("Failed writing to work fd: %s", strerror(errno));
    } else if (written != sizeof work) {
        ALOGW("Truncated writing to work fd: %zd", written);
    }
}

/// The native state of one NativeActivity, as AOSP's NativeCode.
struct NativeCode : public ANativeActivity {
    NativeCode(void *handle, ANativeActivity_createFunc *create) : dlhandle(handle), createActivityFunc(create) {
        memset(static_cast<ANativeActivity *>(this), 0, sizeof(ANativeActivity));
        memset(&callbacks, 0, sizeof callbacks);
    }

    ~NativeCode() {
        if (callbacks.onDestroy != nullptr) callbacks.onDestroy(this);
        if (env != nullptr) {
            if (clazz != nullptr) env->DeleteGlobalRef(clazz);
            if (javaAssetManager != nullptr) env->DeleteGlobalRef(javaAssetManager);
        }
        if (looper != nullptr && mainWorkRead >= 0) libandroid().ALooper_removeFd(looper, mainWorkRead);
        if (looper != nullptr) libandroid().ALooper_release(looper);
        setSurface(nullptr);
        if (mainWorkRead >= 0) close(mainWorkRead);
        if (mainWorkWrite >= 0) close(mainWorkWrite);
        // The library stays open, as on Android (ADR-0069, as NativeActivity's unloadNativeCode).
    }

    /// The window of `surface` (acquired), or none.
    void setSurface(jobject surface) {
        ANativeWindow *window = surface != nullptr ? minato_surface_window(env, surface) : nullptr;
        if (nativeWindow != nullptr) ANativeWindow_release(nativeWindow);
        nativeWindow = window;
    }

    ANativeActivityCallbacks callbacks;
    void *dlhandle;
    ANativeActivity_createFunc *createActivityFunc;
    std::string internalDataPathObj;
    std::string externalDataPathObj;
    std::string obbPathObj;
    ANativeWindow *nativeWindow = nullptr;
    int32_t lastWindowWidth = 0;
    int32_t lastWindowHeight = 0;
    int mainWorkRead = -1;
    int mainWorkWrite = -1;
    ALooper *looper = nullptr;
    jobject javaAssetManager = nullptr;
};

/// The main thread's commands from the app's threads, as AOSP's mainWorkCallback.
int mainWorkCallback(int, int events, void *data) {
    auto *code = static_cast<NativeCode *>(data);
    if ((events & ALOOPER_EVENT_INPUT) == 0) return 1;
    ActivityWork work;
    const ssize_t got = read(code->mainWorkRead, &work, sizeof work);
    if (got != sizeof work) {
        if (got < 0) {
            ALOGW("Failed reading work fd: %s", strerror(errno));
        } else {
            ALOGW("Truncated reading work fd: %zd", got);
        }
        return 1;
    }
    JNIEnv *env = code->env;
    switch (work.cmd) {
        case CMD_FINISH:
            env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.finish);
            break;
        case CMD_SET_WINDOW_FORMAT:
            env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.setWindowFormat, work.arg1);
            break;
        case CMD_SET_WINDOW_FLAGS:
            env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.setWindowFlags, work.arg1, work.arg2);
            break;
        case CMD_SHOW_SOFT_INPUT:
            env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.showIme, work.arg1);
            break;
        case CMD_HIDE_SOFT_INPUT:
            env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.hideIme, work.arg1);
            break;
        default:
            ALOGW("Unknown work command: %d", work.cmd);
            break;
    }
    // As AOSP's raiseAndClearException: an exception stays for the looper's Java caller, which throws it.
    return 1;
}

// libnativeloader's, which ART has loaded: OpenNativeLibrary and NativeLoaderFreeErrorMessage.
using OpenNativeLibraryFunc = void *(*)(JNIEnv *, int32_t, const char *, jobject, const char *, jstring, bool *,
                                        char **);
using FreeErrorFunc = void (*)(char *);

template <typename Function>
Function from_library(const char *library, const char *name) {
    void *handle = dlopen(library, RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) handle = dlopen(library, RTLD_NOW);
    return handle != nullptr ? reinterpret_cast<Function>(dlsym(handle, name)) : nullptr;
}

thread_local std::string g_error_msg;

jlong loadNativeCode_native(JNIEnv *env, jobject clazz, jstring path, jstring funcName, jobject messageQueue,
                            jstring internalDataDir, jstring obbDir, jstring externalDataDir, jint sdkVersion,
                            jobject jAssetMgr, jbyteArray savedState, jobject classLoader, jstring libraryPath) {
    static const auto open_library = from_library<OpenNativeLibraryFunc>("libnativeloader.so", "OpenNativeLibrary");
    static const auto free_error = from_library<FreeErrorFunc>("libnativeloader.so", "NativeLoaderFreeErrorMessage");
    if (open_library == nullptr || libandroid().missing[0] != 0) {
        g_error_msg = open_library == nullptr ? "libnativeloader.so has no OpenNativeLibrary" : libandroid().missing;
        ALOGW("%s", g_error_msg.c_str());
        return 0;
    }
    ScopedUtfChars pathStr(env, path);
    bool needs_native_bridge = false;
    char *nativeloader_error_msg = nullptr;
    void *handle = open_library(env, sdkVersion, pathStr.c_str(), classLoader, nullptr, libraryPath,
                                &needs_native_bridge, &nativeloader_error_msg);
    if (handle == nullptr) {
        g_error_msg = nativeloader_error_msg != nullptr ? nativeloader_error_msg : "unknown error";
        if (nativeloader_error_msg != nullptr && free_error != nullptr) free_error(nativeloader_error_msg);
        ALOGW("NativeActivity LoadNativeLibrary(\"%s\") failed: %s", pathStr.c_str(), g_error_msg.c_str());
        return 0;
    }
    ScopedUtfChars funcStr(env, funcName);
    auto *create = reinterpret_cast<ANativeActivity_createFunc *>(dlsym(handle, funcStr.c_str()));
    if (create == nullptr) {
        const char *error = dlerror();
        g_error_msg = error != nullptr ? error : "unknown error";
        ALOGW("ANativeActivity_onCreate not found: %s", g_error_msg.c_str());
        return 0;
    }
    auto code = std::make_unique<NativeCode>(handle, create);

    code->looper = android_os_MessageQueue_getALooper(env, messageQueue);
    if (code->looper == nullptr) {
        g_error_msg = "Unable to retrieve native MessageQueue";
        ALOGW("%s", g_error_msg.c_str());
        return 0;
    }
    libandroid().ALooper_acquire(code->looper);
    int msgpipe[2];
    if (pipe(msgpipe) != 0) {
        g_error_msg = std::string("could not create pipe: ") + strerror(errno);
        ALOGW("%s", g_error_msg.c_str());
        return 0;
    }
    code->mainWorkRead = msgpipe[0];
    code->mainWorkWrite = msgpipe[1];
    if (fcntl(code->mainWorkRead, F_SETFL, O_NONBLOCK) != 0) {
        ALOGW("Could not make main work read pipe non-blocking: %s", strerror(errno));
    }
    if (fcntl(code->mainWorkWrite, F_SETFL, O_NONBLOCK) != 0) {
        ALOGW("Could not make main work write pipe non-blocking: %s", strerror(errno));
    }
    libandroid().ALooper_addFd(code->looper, code->mainWorkRead, 0, ALOOPER_EVENT_INPUT, mainWorkCallback,
                               code.get());

    code->ANativeActivity::callbacks = &code->callbacks;
    if (env->GetJavaVM(&code->vm) < 0) {
        g_error_msg = "NativeActivity GetJavaVM failed";
        ALOGW("%s", g_error_msg.c_str());
        return 0;
    }
    code->env = env;
    code->clazz = env->NewGlobalRef(clazz);
    code->internalDataPathObj = ScopedUtfChars(env, internalDataDir).c_str();
    code->internalDataPath = code->internalDataPathObj.c_str();
    if (externalDataDir != nullptr) code->externalDataPathObj = ScopedUtfChars(env, externalDataDir).c_str();
    code->externalDataPath = code->externalDataPathObj.c_str();
    code->sdkVersion = sdkVersion;
    code->javaAssetManager = env->NewGlobalRef(jAssetMgr);
    code->assetManager = libandroid().AAssetManager_fromJava(env, jAssetMgr);
    if (obbDir != nullptr) code->obbPathObj = ScopedUtfChars(env, obbDir).c_str();
    code->obbPath = code->obbPathObj.c_str();

    jbyte *rawSavedState = nullptr;
    jsize rawSavedSize = 0;
    if (savedState != nullptr) {
        rawSavedState = env->GetByteArrayElements(savedState, nullptr);
        rawSavedSize = env->GetArrayLength(savedState);
    }
    code->createActivityFunc(code.get(), rawSavedState, static_cast<size_t>(rawSavedSize));
    if (rawSavedState != nullptr) env->ReleaseByteArrayElements(savedState, rawSavedState, 0);
    return reinterpret_cast<jlong>(code.release());
}

jstring getDlError_native(JNIEnv *env, jobject) {
    jstring result = env->NewStringUTF(g_error_msg.c_str());
    g_error_msg.clear();
    return result;
}

NativeCode *native_code(jlong handle) {
    return reinterpret_cast<NativeCode *>(handle);
}

void unloadNativeCode_native(JNIEnv *, jobject, jlong handle) {
    delete native_code(handle);
}

void onStart_native(JNIEnv *, jobject, jlong handle) {
    if (NativeCode *code = native_code(handle); code != nullptr && code->callbacks.onStart != nullptr) {
        code->callbacks.onStart(code);
    }
}

void onResume_native(JNIEnv *, jobject, jlong handle) {
    if (NativeCode *code = native_code(handle); code != nullptr && code->callbacks.onResume != nullptr) {
        code->callbacks.onResume(code);
    }
}

jbyteArray onSaveInstanceState_native(JNIEnv *env, jobject, jlong handle) {
    NativeCode *code = native_code(handle);
    if (code == nullptr || code->callbacks.onSaveInstanceState == nullptr) return nullptr;
    size_t length = 0;
    auto *state = static_cast<jbyte *>(code->callbacks.onSaveInstanceState(code, &length));
    jbyteArray array = nullptr;
    if (length > 0) {
        array = env->NewByteArray(static_cast<jsize>(length));
        if (array != nullptr) env->SetByteArrayRegion(array, 0, static_cast<jsize>(length), state);
    }
    free(state);
    return array;
}

void onPause_native(JNIEnv *, jobject, jlong handle) {
    if (NativeCode *code = native_code(handle); code != nullptr && code->callbacks.onPause != nullptr) {
        code->callbacks.onPause(code);
    }
}

void onStop_native(JNIEnv *, jobject, jlong handle) {
    if (NativeCode *code = native_code(handle); code != nullptr && code->callbacks.onStop != nullptr) {
        code->callbacks.onStop(code);
    }
}

void onConfigurationChanged_native(JNIEnv *, jobject, jlong handle) {
    if (NativeCode *code = native_code(handle); code != nullptr && code->callbacks.onConfigurationChanged != nullptr) {
        code->callbacks.onConfigurationChanged(code);
    }
}

void onLowMemory_native(JNIEnv *, jobject, jlong handle) {
    if (NativeCode *code = native_code(handle); code != nullptr && code->callbacks.onLowMemory != nullptr) {
        code->callbacks.onLowMemory(code);
    }
}

void onWindowFocusChanged_native(JNIEnv *, jobject, jlong handle, jboolean focused) {
    if (NativeCode *code = native_code(handle); code != nullptr && code->callbacks.onWindowFocusChanged != nullptr) {
        code->callbacks.onWindowFocusChanged(code, focused ? 1 : 0);
    }
}

void onSurfaceCreated_native(JNIEnv *, jobject, jlong handle, jobject surface) {
    NativeCode *code = native_code(handle);
    if (code == nullptr) return;
    code->setSurface(surface);
    if (code->nativeWindow != nullptr && code->callbacks.onNativeWindowCreated != nullptr) {
        code->callbacks.onNativeWindowCreated(code, code->nativeWindow);
    }
}

void onSurfaceChanged_native(JNIEnv *, jobject, jlong handle, jobject surface, jint, jint, jint) {
    NativeCode *code = native_code(handle);
    if (code == nullptr) return;
    ANativeWindow *old = code->nativeWindow;
    if (old != nullptr) ANativeWindow_acquire(old);
    code->setSurface(surface);
    if (old != code->nativeWindow) {
        if (old != nullptr && code->callbacks.onNativeWindowDestroyed != nullptr) {
            code->callbacks.onNativeWindowDestroyed(code, old);
        }
        if (code->nativeWindow != nullptr) {
            if (code->callbacks.onNativeWindowCreated != nullptr) {
                code->callbacks.onNativeWindowCreated(code, code->nativeWindow);
            }
            code->lastWindowWidth = ANativeWindow_getWidth(code->nativeWindow);
            code->lastWindowHeight = ANativeWindow_getHeight(code->nativeWindow);
        }
    } else if (code->nativeWindow != nullptr) {
        const int32_t width = ANativeWindow_getWidth(code->nativeWindow);
        const int32_t height = ANativeWindow_getHeight(code->nativeWindow);
        if (width != code->lastWindowWidth || height != code->lastWindowHeight) {
            if (code->callbacks.onNativeWindowResized != nullptr) {
                code->callbacks.onNativeWindowResized(code, code->nativeWindow);
            }
        }
    }
    if (old != nullptr) ANativeWindow_release(old);
}

void onSurfaceRedrawNeeded_native(JNIEnv *, jobject, jlong handle, jobject) {
    NativeCode *code = native_code(handle);
    if (code != nullptr && code->nativeWindow != nullptr && code->callbacks.onNativeWindowRedrawNeeded != nullptr) {
        code->callbacks.onNativeWindowRedrawNeeded(code, code->nativeWindow);
    }
}

void onSurfaceDestroyed_native(JNIEnv *, jobject, jlong handle) {
    NativeCode *code = native_code(handle);
    if (code == nullptr) return;
    if (code->nativeWindow != nullptr && code->callbacks.onNativeWindowDestroyed != nullptr) {
        code->callbacks.onNativeWindowDestroyed(code, code->nativeWindow);
    }
    code->setSurface(nullptr);
}

void onInputQueueCreated_native(JNIEnv *, jobject, jlong handle, jlong queuePtr) {
    NativeCode *code = native_code(handle);
    if (code != nullptr && code->callbacks.onInputQueueCreated != nullptr) {
        code->callbacks.onInputQueueCreated(code, reinterpret_cast<AInputQueue *>(queuePtr));
    }
}

void onInputQueueDestroyed_native(JNIEnv *, jobject, jlong handle, jlong queuePtr) {
    NativeCode *code = native_code(handle);
    if (code != nullptr && code->callbacks.onInputQueueDestroyed != nullptr) {
        code->callbacks.onInputQueueDestroyed(code, reinterpret_cast<AInputQueue *>(queuePtr));
    }
}

void onContentRectChanged_native(JNIEnv *, jobject, jlong handle, jint x, jint y, jint w, jint h) {
    NativeCode *code = native_code(handle);
    if (code != nullptr && code->callbacks.onContentRectChanged != nullptr) {
        const ARect rect{x, y, x + w, y + h};
        code->callbacks.onContentRectChanged(code, &rect);
    }
}

const JNINativeMethod g_methods[] = {
        {"loadNativeCode",
         "(Ljava/lang/String;Ljava/lang/String;Landroid/os/MessageQueue;Ljava/lang/String;Ljava/lang/String;"
         "Ljava/lang/String;ILandroid/content/res/AssetManager;[BLjava/lang/ClassLoader;Ljava/lang/String;)J",
         reinterpret_cast<void *>(loadNativeCode_native)},
        {"getDlError", "()Ljava/lang/String;", reinterpret_cast<void *>(getDlError_native)},
        {"unloadNativeCode", "(J)V", reinterpret_cast<void *>(unloadNativeCode_native)},
        {"onStartNative", "(J)V", reinterpret_cast<void *>(onStart_native)},
        {"onResumeNative", "(J)V", reinterpret_cast<void *>(onResume_native)},
        {"onSaveInstanceStateNative", "(J)[B", reinterpret_cast<void *>(onSaveInstanceState_native)},
        {"onPauseNative", "(J)V", reinterpret_cast<void *>(onPause_native)},
        {"onStopNative", "(J)V", reinterpret_cast<void *>(onStop_native)},
        {"onConfigurationChangedNative", "(J)V", reinterpret_cast<void *>(onConfigurationChanged_native)},
        {"onLowMemoryNative", "(J)V", reinterpret_cast<void *>(onLowMemory_native)},
        {"onWindowFocusChangedNative", "(JZ)V", reinterpret_cast<void *>(onWindowFocusChanged_native)},
        {"onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", reinterpret_cast<void *>(onSurfaceCreated_native)},
        {"onSurfaceChangedNative", "(JLandroid/view/Surface;III)V",
         reinterpret_cast<void *>(onSurfaceChanged_native)},
        {"onSurfaceRedrawNeededNative", "(JLandroid/view/Surface;)V",
         reinterpret_cast<void *>(onSurfaceRedrawNeeded_native)},
        {"onSurfaceDestroyedNative", "(J)V", reinterpret_cast<void *>(onSurfaceDestroyed_native)},
        {"onInputQueueCreatedNative", "(JJ)V", reinterpret_cast<void *>(onInputQueueCreated_native)},
        {"onInputQueueDestroyedNative", "(JJ)V", reinterpret_cast<void *>(onInputQueueDestroyed_native)},
        {"onContentRectChangedNative", "(JIIII)V", reinterpret_cast<void *>(onContentRectChanged_native)},
};

}  // namespace

int register_android_app_NativeActivity(JNIEnv *env) {
    jclass clazz = FindClassOrDie(env, "android/app/NativeActivity");
    gNativeActivityClassInfo.finish = GetMethodIDOrDie(env, clazz, "finish", "()V");
    gNativeActivityClassInfo.setWindowFlags = GetMethodIDOrDie(env, clazz, "setWindowFlags", "(II)V");
    gNativeActivityClassInfo.setWindowFormat = GetMethodIDOrDie(env, clazz, "setWindowFormat", "(I)V");
    gNativeActivityClassInfo.showIme = GetMethodIDOrDie(env, clazz, "showIme", "(I)V");
    gNativeActivityClassInfo.hideIme = GetMethodIDOrDie(env, clazz, "hideIme", "(I)V");
    return RegisterMethodsOrDie(env, "android/app/NativeActivity", g_methods, NELEM(g_methods));
}

}  // namespace android

// libandroid's ANativeActivity_* on the Java path (runtime/android/native_activity.cpp looks these up), as AOSP's
// libandroid_runtime exports them.
using android::CMD_FINISH;
using android::CMD_HIDE_SOFT_INPUT;
using android::CMD_SET_WINDOW_FLAGS;
using android::CMD_SET_WINDOW_FORMAT;
using android::CMD_SHOW_SOFT_INPUT;

extern "C" void android_NativeActivity_finish(ANativeActivity *activity) {
    android::write_work(static_cast<android::NativeCode *>(activity)->mainWorkWrite, CMD_FINISH);
}

extern "C" void android_NativeActivity_setWindowFormat(ANativeActivity *activity, int32_t format) {
    android::write_work(static_cast<android::NativeCode *>(activity)->mainWorkWrite, CMD_SET_WINDOW_FORMAT, format);
}

extern "C" void android_NativeActivity_setWindowFlags(ANativeActivity *activity, int32_t values, int32_t mask) {
    android::write_work(static_cast<android::NativeCode *>(activity)->mainWorkWrite, CMD_SET_WINDOW_FLAGS, values,
                        mask);
}

extern "C" void android_NativeActivity_showSoftInput(ANativeActivity *activity, int32_t flags) {
    android::write_work(static_cast<android::NativeCode *>(activity)->mainWorkWrite, CMD_SHOW_SOFT_INPUT, flags);
}

extern "C" void android_NativeActivity_hideSoftInput(ANativeActivity *activity, int32_t flags) {
    android::write_work(static_cast<android::NativeCode *>(activity)->mainWorkWrite, CMD_HIDE_SOFT_INPUT, flags);
}
