/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// libinput's Keyboard.cpp at android-15.0.0_r1, its meta-state functions alone (ADR-0045 step 7c1): libinput's
// KeyCharacterMap.cpp asks normalizeMetaState and updateMetaState when it makes the events that type a string. The
// rest of Keyboard.cpp loads a device's key layout and key character map files, which Minato's input service does
// not (input/Keyboard.h, our stand-in).
#include <android/input.h>
#include <android/keycodes.h>
#include <input/Keyboard.h>

namespace android {

static int32_t setEphemeralMetaState(int32_t mask, bool down, int32_t oldMetaState) {
    int32_t newMetaState;
    if (down) {
        newMetaState = oldMetaState | mask;
    } else {
        newMetaState = oldMetaState &
                ~(mask | AMETA_ALT_ON | AMETA_SHIFT_ON | AMETA_CTRL_ON | AMETA_META_ON);
    }

    return normalizeMetaState(newMetaState);
}

int32_t normalizeMetaState(int32_t oldMetaState) {
    int32_t newMetaState = oldMetaState;
    if (newMetaState & (AMETA_ALT_LEFT_ON | AMETA_ALT_RIGHT_ON)) {
        newMetaState |= AMETA_ALT_ON;
    }

    if (newMetaState & (AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_RIGHT_ON)) {
        newMetaState |= AMETA_SHIFT_ON;
    }

    if (newMetaState & (AMETA_CTRL_LEFT_ON | AMETA_CTRL_RIGHT_ON)) {
        newMetaState |= AMETA_CTRL_ON;
    }

    if (newMetaState & (AMETA_META_LEFT_ON | AMETA_META_RIGHT_ON)) {
        newMetaState |= AMETA_META_ON;
    }
    return newMetaState;
}

static int32_t toggleLockedMetaState(int32_t mask, bool down, int32_t oldMetaState) {
    if (down) {
        return oldMetaState;
    } else {
        return oldMetaState ^ mask;
    }
}

int32_t updateMetaState(int32_t keyCode, bool down, int32_t oldMetaState) {
    switch (keyCode) {
    case AKEYCODE_ALT_LEFT:
        return setEphemeralMetaState(AMETA_ALT_LEFT_ON, down, oldMetaState);
    case AKEYCODE_ALT_RIGHT:
        return setEphemeralMetaState(AMETA_ALT_RIGHT_ON, down, oldMetaState);
    case AKEYCODE_SHIFT_LEFT:
        return setEphemeralMetaState(AMETA_SHIFT_LEFT_ON, down, oldMetaState);
    case AKEYCODE_SHIFT_RIGHT:
        return setEphemeralMetaState(AMETA_SHIFT_RIGHT_ON, down, oldMetaState);
    case AKEYCODE_SYM:
        return setEphemeralMetaState(AMETA_SYM_ON, down, oldMetaState);
    case AKEYCODE_FUNCTION:
        return setEphemeralMetaState(AMETA_FUNCTION_ON, down, oldMetaState);
    case AKEYCODE_CTRL_LEFT:
        return setEphemeralMetaState(AMETA_CTRL_LEFT_ON, down, oldMetaState);
    case AKEYCODE_CTRL_RIGHT:
        return setEphemeralMetaState(AMETA_CTRL_RIGHT_ON, down, oldMetaState);
    case AKEYCODE_META_LEFT:
        return setEphemeralMetaState(AMETA_META_LEFT_ON, down, oldMetaState);
    case AKEYCODE_META_RIGHT:
        return setEphemeralMetaState(AMETA_META_RIGHT_ON, down, oldMetaState);
    case AKEYCODE_CAPS_LOCK:
        return toggleLockedMetaState(AMETA_CAPS_LOCK_ON, down, oldMetaState);
    case AKEYCODE_NUM_LOCK:
        return toggleLockedMetaState(AMETA_NUM_LOCK_ON, down, oldMetaState);
    case AKEYCODE_SCROLL_LOCK:
        return toggleLockedMetaState(AMETA_SCROLL_LOCK_ON, down, oldMetaState);
    default:
        return oldMetaState;
    }
}

bool isMetaKey(int32_t keyCode) {
    switch (keyCode) {
    case AKEYCODE_ALT_LEFT:
    case AKEYCODE_ALT_RIGHT:
    case AKEYCODE_SHIFT_LEFT:
    case AKEYCODE_SHIFT_RIGHT:
    case AKEYCODE_SYM:
    case AKEYCODE_FUNCTION:
    case AKEYCODE_CTRL_LEFT:
    case AKEYCODE_CTRL_RIGHT:
    case AKEYCODE_META_LEFT:
    case AKEYCODE_META_RIGHT:
    case AKEYCODE_CAPS_LOCK:
    case AKEYCODE_NUM_LOCK:
    case AKEYCODE_SCROLL_LOCK:
        return true;
    default:
        return false;
    }
}

} // namespace android
