// Ours in place of libinput's InputEventLabels.cpp (ADR-0045 step 7b1), which the fetch leaves out: its tables need a
// header generated from the kernel's input codes. libinput's Input.cpp asks it for two things:
//   - an axis's label and an axis by its label, for MotionEvent.axisToString and axisFromString: kAxisLabels is
//     libinput's AXES_SEQUENCE at android-15.0.0_r1, every AMOTION_EVENT_AXIS_ without its prefix at the index of
//     its value, with RESERVED_29 to RESERVED_31 between RELATIVE_Y and GENERIC_1 as libinput has them
//     (framework/tests/axis_labels_test.py checks the table against MotionEvent.java's AXIS_ constants). A label
//     answers by value as libinput's vector does, and a value by its exact label as libinput's map does;
//   - a key code's label and a key code by its label, for KeyEvent::getLabel, which nothing on Minato's path calls:
//     KeyEvent's natives are ours (android_view_KeyEvent.cpp) with their own table. Both answer none.
#include <input/InputEventLabels.h>
#include <string.h>

namespace android {
namespace {

constexpr const char *kAxisLabels[] = {
        "X", "Y", "PRESSURE", "SIZE", "TOUCH_MAJOR", "TOUCH_MINOR", "TOOL_MAJOR", "TOOL_MINOR", "ORIENTATION",
        "VSCROLL", "HSCROLL", "Z", "RX", "RY", "RZ", "HAT_X", "HAT_Y", "LTRIGGER", "RTRIGGER", "THROTTLE", "RUDDER",
        "WHEEL", "GAS", "BRAKE", "DISTANCE", "TILT", "SCROLL", "RELATIVE_X", "RELATIVE_Y", "RESERVED_29",
        "RESERVED_30", "RESERVED_31", "GENERIC_1", "GENERIC_2", "GENERIC_3", "GENERIC_4", "GENERIC_5", "GENERIC_6",
        "GENERIC_7", "GENERIC_8", "GENERIC_9", "GENERIC_10", "GENERIC_11", "GENERIC_12", "GENERIC_13", "GENERIC_14",
        "GENERIC_15", "GENERIC_16", "GESTURE_X_OFFSET", "GESTURE_Y_OFFSET", "GESTURE_SCROLL_X_DISTANCE",
        "GESTURE_SCROLL_Y_DISTANCE", "GESTURE_PINCH_SCALE_FACTOR", "GESTURE_SWIPE_FINGER_COUNT",
};

}  // namespace

const char *InputEventLookup::getAxisLabel(int32_t axisId) {
    if (static_cast<size_t>(axisId) < std::size(kAxisLabels)) {
        return kAxisLabels[axisId];
    }
    return nullptr;
}

std::optional<int> InputEventLookup::getAxisByLabel(const char *label) {
    for (size_t axis = 0; axis < std::size(kAxisLabels); axis++) {
        if (strcmp(kAxisLabels[axis], label) == 0) {
            return static_cast<int>(axis);
        }
    }
    return std::nullopt;
}

const char *InputEventLookup::getLabelByKeyCode(int32_t) {
    return nullptr;
}

std::optional<int> InputEventLookup::getKeyCodeByLabel(const char *) {
    return std::nullopt;
}

}  // namespace android
