// Ours in place of AOSP's media/jni/android_media_PublicFormatUtils.cpp (the graphics breadth's G3a):
// android.media.PublicFormatUtils' natives, which ImageReader and ImageWriter map their formats with, on libui's
// PublicFormat.cpp itself (built into libandroid_runtime with framework/native/include's Dataspace values).
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <ui/PublicFormat.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

jint getHalFormat(JNIEnv *, jclass, jint imageFormat) {
    return mapPublicFormatToHalFormat(static_cast<PublicFormat>(imageFormat));
}

jint getHalDataspace(JNIEnv *, jclass, jint imageFormat) {
    return static_cast<jint>(mapPublicFormatToHalDataspace(static_cast<PublicFormat>(imageFormat)));
}

jint getPublicFormat(JNIEnv *, jclass, jint hardwareBufferFormat, jint dataspace) {
    return static_cast<jint>(
            mapHalFormatDataspaceToPublicFormat(hardwareBufferFormat, static_cast<android_dataspace>(dataspace)));
}

const JNINativeMethod gMethods[] = {
        {"nativeGetHalFormat", "(I)I", (void *)getHalFormat},
        {"nativeGetHalDataspace", "(I)I", (void *)getHalDataspace},
        {"nativeGetPublicFormat", "(II)I", (void *)getPublicFormat},
};

}  // namespace

int register_android_media_PublicFormatUtils(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/media/PublicFormatUtils", gMethods, NELEM(gMethods));
}

}  // namespace android
