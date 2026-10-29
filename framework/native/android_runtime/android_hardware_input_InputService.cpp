// The input service's native (ADR-0045 step 7c1), ours: the virtual keyboard's key character map. Android's EventHub
// loads it from /system/usr/keychars/Virtual.kcm with libinput's KeyCharacterMap, and InputDevice's parcel brings it to
// the app. Minato builds that file into this library instead (kVirtualKcm, which framework/CMakeLists.txt makes from
// frameworks/base's data/keyboards/Virtual.kcm at android-15.0.0_r1) and parses it with libinput's KeyCharacterMap
// under the same name, so the map and its parcel are Android's without a file in the guest's /system: a departure
// from Android's place, not from its map.
#include <input/InputDevice.h>
#include <input/KeyCharacterMap.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <memory>

#include "android_view_KeyCharacterMap.h"
#include "core_jni_helpers.h"

// Virtual.kcm's and Generic.kcm's texts with their terminating NUL (framework/CMakeLists.txt).
extern const char kVirtualKcm[];
extern const char kGenericKcm[];

namespace android {
namespace {

constexpr const char *kVirtualKcmPath = "/system/usr/keychars/Virtual.kcm";
constexpr const char *kGenericKcmPath = "/system/usr/keychars/Generic.kcm";

jobject NativeVirtualKeyCharacterMap(JNIEnv *env, jclass) {
    base::Result<std::shared_ptr<KeyCharacterMap>> map =
            KeyCharacterMap::loadContents(kVirtualKcmPath, kVirtualKcm, KeyCharacterMap::Format::BASE);
    if (!map.ok()) {
        jniThrowExceptionFmt(env, "java/lang/IllegalStateException", "%s: %s", kVirtualKcmPath,
                             map.error().message().c_str());
        return nullptr;
    }
    return android_view_KeyCharacterMap_create(env, VIRTUAL_KEYBOARD_ID, std::make_unique<KeyCharacterMap>(**map));
}

// A hardware keyboard's map (ADR-0068 step 7c2): Generic.kcm, which EventHub loads for a keyboard without a map of
// its own.
jobject NativeKeyboardKeyCharacterMap(JNIEnv *env, jclass, jint deviceId) {
    base::Result<std::shared_ptr<KeyCharacterMap>> map =
            KeyCharacterMap::loadContents(kGenericKcmPath, kGenericKcm, KeyCharacterMap::Format::BASE);
    if (!map.ok()) {
        jniThrowExceptionFmt(env, "java/lang/IllegalStateException", "%s: %s", kGenericKcmPath,
                             map.error().message().c_str());
        return nullptr;
    }
    return android_view_KeyCharacterMap_create(env, deviceId, std::make_unique<KeyCharacterMap>(**map));
}

const JNINativeMethod gMethods[] = {
        {"nativeVirtualKeyCharacterMap", "()Landroid/view/KeyCharacterMap;",
         reinterpret_cast<void *>(NativeVirtualKeyCharacterMap)},
        {"nativeKeyboardKeyCharacterMap", "(I)Landroid/view/KeyCharacterMap;",
         reinterpret_cast<void *>(NativeKeyboardKeyCharacterMap)},
};

}  // namespace

int register_android_hardware_input_InputService(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/hardware/input/InputService", gMethods, NELEM(gMethods));
}

}  // namespace android
