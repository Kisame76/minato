// Ours in place of AOSP's core/jni/android_app_Activity.cpp (ADR-0039 step c2c3): Activity.getDlWarning, which
// Activity.performStart shows as a dialog for a debuggable app. As AOSP's, it asks the linker for its last warning
// about the app's native libraries (android_dlwarning), here the runtime's linker through libdl_android.so, opened
// at the first call as the other natives open libandroid; null when the linker has none to give, as for an app
// without warnings.
#include <dlfcn.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <string>

#include "core_jni_helpers.h"

namespace android {
namespace {

using DlWarning = void (*)(void *, void (*)(void *, const char *));

DlWarning dlwarning() {
    static const DlWarning function = [] {
        void *library = dlopen("libdl_android.so", RTLD_NOW);
        return library != nullptr ? reinterpret_cast<DlWarning>(dlsym(library, "android_dlwarning")) : nullptr;
    }();
    return function;
}

jstring GetDlWarning(JNIEnv *env, jclass) {
    std::string msg;
    if (DlWarning warning = dlwarning()) {
        warning(&msg, [](void *obj, const char *text) {
            if (text != nullptr) {
                *reinterpret_cast<std::string *>(obj) = text;
            }
        });
    }
    return msg.empty() ? nullptr : env->NewStringUTF(msg.c_str());
}

const JNINativeMethod gMethods[] = {
        {"getDlWarning", "()Ljava/lang/String;", reinterpret_cast<void *>(GetDlWarning)},
};

}  // namespace

int register_android_app_Activity(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/app/Activity", gMethods, NELEM(gMethods));
}

}  // namespace android
