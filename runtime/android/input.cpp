// AInputEvent_*, AMotionEvent_* and AKeyEvent_* of libandroid.so (ADR-0036, decision 3), on the records of the input
// queue (internal.h). A key (ADR-0068 step 2e) gives its record's fields; its source is 0, as Android 15's
// NativeActivity reports a key's (native-ime-test on the AVD, where Java's KeyEvent of the same key has 0x101). Every
// other event is a motion event of the touchscreen from one device, each pointer a finger. What a
// record does not carry is fixed: no flags, meta state, buttons, edge flags, classification or action button,
// no offset, a precision of 1, the touch minor and the tool's sizes as the touch major, size and orientation 0.
// The raw places are the places, since the window fills the screen. An index past the pointers or the samples
// is fatal, as in Android 15's MotionEvent; the historical index of the event's own sample, the history's size,
// gives that sample, as there.
#include <android/input.h>
#include <android/log.h>
#include <stddef.h>
#include <stdint.h>

#include "internal.h"

namespace {

constexpr int32_t kDevice = 1;

[[noreturn]] void fatal(const char *getter, const char *which, size_t index) {
    __android_log_assert(nullptr, "Input", "%s: Invalid %s index %zu", getter, which, index);
}

bool is_key(const AInputEvent *event) {
    return event->motion.kind == APKRT_INPUT_KEY;
}

size_t own_sample(const AInputEvent *event) {
    return event->motion.sample_count - 1;
}

size_t checked_pointer(const AInputEvent *event, size_t pointer, const char *getter) {
    if (pointer >= event->motion.pointer_count) fatal(getter, "pointer", pointer);
    return pointer;
}

/// The place of pointer `pointer` in sample `sample`.
const apkrt_pointer_coords &place(const AInputEvent *event, size_t pointer, size_t sample, const char *getter) {
    checked_pointer(event, pointer, getter);
    if (sample >= event->motion.sample_count) fatal(getter, "historical", sample);
    return event->motion.coords[sample][pointer];
}

float axis_value(const apkrt_pointer_coords &at, int32_t axis) {
    switch (axis) {
        case AMOTION_EVENT_AXIS_X: return at.x;
        case AMOTION_EVENT_AXIS_Y: return at.y;
        case AMOTION_EVENT_AXIS_PRESSURE: return at.pressure;
        case AMOTION_EVENT_AXIS_TOUCH_MAJOR:
        case AMOTION_EVENT_AXIS_TOUCH_MINOR:
        case AMOTION_EVENT_AXIS_TOOL_MAJOR:
        case AMOTION_EVENT_AXIS_TOOL_MINOR: return at.touch_major;
        default: return 0;  // AXIS_SIZE, AXIS_ORIENTATION and the axes a touchscreen does not have
    }
}

}  // namespace

extern "C" {

int32_t AInputEvent_getType(const AInputEvent *event) {
    return is_key(event) ? AINPUT_EVENT_TYPE_KEY : AINPUT_EVENT_TYPE_MOTION;
}

int32_t AInputEvent_getDeviceId(const AInputEvent *event) {
    return is_key(event) ? event->key.device_id : kDevice;
}

int32_t AInputEvent_getSource(const AInputEvent *event) {
    return is_key(event) ? 0 : AINPUT_SOURCE_TOUCHSCREEN;
}

int32_t AKeyEvent_getAction(const AInputEvent *key_event) {
    return key_event->key.action;
}

int32_t AKeyEvent_getFlags(const AInputEvent *key_event) {
    return key_event->key.flags;
}

int32_t AKeyEvent_getKeyCode(const AInputEvent *key_event) {
    return key_event->key.key_code;
}

int32_t AKeyEvent_getScanCode(const AInputEvent *key_event) {
    return key_event->key.scan_code;
}

int32_t AKeyEvent_getMetaState(const AInputEvent *key_event) {
    return key_event->key.meta_state;
}

int32_t AKeyEvent_getRepeatCount(const AInputEvent *key_event) {
    return key_event->key.repeat_count;
}

int64_t AKeyEvent_getDownTime(const AInputEvent *key_event) {
    return key_event->key.down_time;
}

int64_t AKeyEvent_getEventTime(const AInputEvent *key_event) {
    return key_event->key.event_time;
}

int32_t AMotionEvent_getAction(const AInputEvent *motion_event) {
    return motion_event->motion.action;
}

int32_t AMotionEvent_getFlags(const AInputEvent *) {
    return 0;
}

int32_t AMotionEvent_getMetaState(const AInputEvent *) {
    return 0;
}

int32_t AMotionEvent_getButtonState(const AInputEvent *) {
    return 0;
}

int32_t AMotionEvent_getEdgeFlags(const AInputEvent *) {
    return 0;
}

int32_t AMotionEvent_getClassification(const AInputEvent *) {
    return AMOTION_EVENT_CLASSIFICATION_NONE;
}

int32_t AMotionEvent_getActionButton(const AInputEvent *) {
    return 0;
}

int64_t AMotionEvent_getDownTime(const AInputEvent *motion_event) {
    return motion_event->motion.down_time;
}

int64_t AMotionEvent_getEventTime(const AInputEvent *motion_event) {
    return motion_event->motion.sample_times[own_sample(motion_event)];
}

float AMotionEvent_getXOffset(const AInputEvent *) {
    return 0;
}

float AMotionEvent_getYOffset(const AInputEvent *) {
    return 0;
}

float AMotionEvent_getXPrecision(const AInputEvent *) {
    return 1;
}

float AMotionEvent_getYPrecision(const AInputEvent *) {
    return 1;
}

size_t AMotionEvent_getPointerCount(const AInputEvent *motion_event) {
    return motion_event->motion.pointer_count;
}

int32_t AMotionEvent_getPointerId(const AInputEvent *motion_event, size_t pointer_index) {
    return motion_event->motion.pointer_ids[checked_pointer(motion_event, pointer_index, "getPointerId")];
}

int32_t AMotionEvent_getToolType(const AInputEvent *motion_event, size_t pointer_index) {
    checked_pointer(motion_event, pointer_index, "getToolType");
    return AMOTION_EVENT_TOOL_TYPE_FINGER;
}

float AMotionEvent_getRawX(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getRawX").x;
}

float AMotionEvent_getRawY(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getRawY").y;
}

float AMotionEvent_getX(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getX").x;
}

float AMotionEvent_getY(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getY").y;
}

float AMotionEvent_getPressure(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getPressure").pressure;
}

float AMotionEvent_getSize(const AInputEvent *motion_event, size_t pointer_index) {
    return axis_value(place(motion_event, pointer_index, own_sample(motion_event), "getSize"), AMOTION_EVENT_AXIS_SIZE);
}

float AMotionEvent_getTouchMajor(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getTouchMajor").touch_major;
}

float AMotionEvent_getTouchMinor(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getTouchMinor").touch_major;
}

float AMotionEvent_getToolMajor(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getToolMajor").touch_major;
}

float AMotionEvent_getToolMinor(const AInputEvent *motion_event, size_t pointer_index) {
    return place(motion_event, pointer_index, own_sample(motion_event), "getToolMinor").touch_major;
}

float AMotionEvent_getOrientation(const AInputEvent *motion_event, size_t pointer_index) {
    return axis_value(place(motion_event, pointer_index, own_sample(motion_event), "getOrientation"),
                      AMOTION_EVENT_AXIS_ORIENTATION);
}

float AMotionEvent_getAxisValue(const AInputEvent *motion_event, int32_t axis, size_t pointer_index) {
    return axis_value(place(motion_event, pointer_index, own_sample(motion_event), "getAxisValue"), axis);
}

size_t AMotionEvent_getHistorySize(const AInputEvent *motion_event) {
    return own_sample(motion_event);
}

int64_t AMotionEvent_getHistoricalEventTime(const AInputEvent *motion_event, size_t history_index) {
    if (history_index >= motion_event->motion.sample_count) {
        fatal("getHistoricalEventTime", "historical", history_index);
    }
    return motion_event->motion.sample_times[history_index];
}

float AMotionEvent_getHistoricalRawX(const AInputEvent *motion_event, size_t pointer_index, size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalRawX").x;
}

float AMotionEvent_getHistoricalRawY(const AInputEvent *motion_event, size_t pointer_index, size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalRawY").y;
}

float AMotionEvent_getHistoricalX(const AInputEvent *motion_event, size_t pointer_index, size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalX").x;
}

float AMotionEvent_getHistoricalY(const AInputEvent *motion_event, size_t pointer_index, size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalY").y;
}

float AMotionEvent_getHistoricalPressure(const AInputEvent *motion_event, size_t pointer_index, size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalPressure").pressure;
}

float AMotionEvent_getHistoricalSize(const AInputEvent *motion_event, size_t pointer_index, size_t history_index) {
    return axis_value(place(motion_event, pointer_index, history_index, "getHistoricalSize"), AMOTION_EVENT_AXIS_SIZE);
}

float AMotionEvent_getHistoricalTouchMajor(const AInputEvent *motion_event, size_t pointer_index,
                                           size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalTouchMajor").touch_major;
}

float AMotionEvent_getHistoricalTouchMinor(const AInputEvent *motion_event, size_t pointer_index,
                                           size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalTouchMinor").touch_major;
}

float AMotionEvent_getHistoricalToolMajor(const AInputEvent *motion_event, size_t pointer_index,
                                          size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalToolMajor").touch_major;
}

float AMotionEvent_getHistoricalToolMinor(const AInputEvent *motion_event, size_t pointer_index,
                                          size_t history_index) {
    return place(motion_event, pointer_index, history_index, "getHistoricalToolMinor").touch_major;
}

float AMotionEvent_getHistoricalOrientation(const AInputEvent *motion_event, size_t pointer_index,
                                            size_t history_index) {
    return axis_value(place(motion_event, pointer_index, history_index, "getHistoricalOrientation"),
                      AMOTION_EVENT_AXIS_ORIENTATION);
}

float AMotionEvent_getHistoricalAxisValue(const AInputEvent *motion_event, int32_t axis, size_t pointer_index,
                                          size_t history_index) {
    return axis_value(place(motion_event, pointer_index, history_index, "getHistoricalAxisValue"), axis);
}

}  // extern "C"
