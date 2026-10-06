// Ours in place of the header aidl --lang=cpp writes from frameworks/native's
// libs/input/android/os/PointerIconType.aidl at android-15.0.0_r1 (ADR-0045 step 7b1), which libinput's Input.h
// includes: the enum with the AIDL's values, without the generated toString and enum range.
#pragma once

#include <stdint.h>

namespace android::os {

enum class PointerIconType : int32_t {
    CUSTOM = -1,
    TYPE_NULL = 0,
    NOT_SPECIFIED = 1,
    ARROW = 1000,
    CONTEXT_MENU = 1001,
    HAND = 1002,
    HELP = 1003,
    WAIT = 1004,
    CELL = 1006,
    CROSSHAIR = 1007,
    TEXT = 1008,
    VERTICAL_TEXT = 1009,
    ALIAS = 1010,
    COPY = 1011,
    NO_DROP = 1012,
    ALL_SCROLL = 1013,
    HORIZONTAL_DOUBLE_ARROW = 1014,
    VERTICAL_DOUBLE_ARROW = 1015,
    TOP_RIGHT_DOUBLE_ARROW = 1016,
    TOP_LEFT_DOUBLE_ARROW = 1017,
    ZOOM_IN = 1018,
    ZOOM_OUT = 1019,
    GRAB = 1020,
    GRABBING = 1021,
    HANDWRITING = 1022,
    SPOT_HOVER = 2000,
    SPOT_TOUCH = 2001,
    SPOT_ANCHOR = 2002,
};

}  // namespace android::os
