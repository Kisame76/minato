// Stand-in for libinput's input/Keyboard.h (ADR-0045 step 7c1), which the fetch leaves out: the rest of it declares
// KeyMap, a device's key layout and key character map files, which need KeyLayoutMap, PropertyMap and
// InputDeviceIdentifier. libinput's KeyCharacterMap.cpp takes two of its meta-state functions, normalizeMetaState
// and updateMetaState, which keyboard_meta_state.cpp defines as libinput's Keyboard.cpp does at android-15.0.0_r1,
// with isMetaKey beside them.
#pragma once

#include <stdint.h>

namespace android {

// The meta state after a key's press or release: a modifier's left or right bit while it is held, a lock's bit
// toggled at its release, and the universal bits of the modifiers that stay held.
extern int32_t updateMetaState(int32_t keyCode, bool down, int32_t oldMetaState);

// The meta state with the universal bit of each modifier whose left or right bit is set.
extern int32_t normalizeMetaState(int32_t oldMetaState);

// Whether a key is a modifier or a lock, such as ALT or CAPS_LOCK.
extern bool isMetaKey(int32_t keyCode);

}  // namespace android
