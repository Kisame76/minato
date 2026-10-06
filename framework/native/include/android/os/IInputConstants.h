// Ours in place of the header aidl --lang=cpp writes from frameworks/native's
// libs/input/android/os/IInputConstants.aidl at android-15.0.0_r1 (ADR-0045 step 7b1), which libinput's Input.h and
// VelocityTracker.h include: the interface's constants with the AIDL's values, without the binder interface around
// them, which Minato has no use for (decision 10 of ADR-0039: no libbinder). It includes binder/IBinder.h as the
// generated header does, which Input.h relies on for its sp<IBinder>: Minato's stand-in, which only declares IBinder.
#pragma once

#include <binder/IBinder.h>
#include <stdint.h>

namespace android::os {

class IInputConstants {
public:
    static constexpr int32_t UNMULTIPLIED_DEFAULT_DISPATCHING_TIMEOUT_MILLIS = 5000;
    static constexpr int32_t INVALID_BATTERY_CAPACITY = -1;
    static constexpr int32_t INVALID_INPUT_EVENT_ID = 0;
    static constexpr int32_t INVALID_INPUT_DEVICE_ID = -2;
    static constexpr int32_t POLICY_FLAG_INJECTED_FROM_ACCESSIBILITY = 0x20000;
    static constexpr int32_t MOTION_EVENT_FLAG_WINDOW_IS_OBSCURED = 0x1;
    static constexpr int32_t MOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED = 0x2;
    static constexpr int32_t MOTION_EVENT_FLAG_HOVER_EXIT_PENDING = 0x4;
    static constexpr int32_t MOTION_EVENT_FLAG_IS_GENERATED_GESTURE = 0x8;
    static constexpr int32_t INPUT_EVENT_FLAG_CANCELED = 0x20;
    static constexpr int32_t MOTION_EVENT_FLAG_NO_FOCUS_CHANGE = 0x40;
    static constexpr int32_t MOTION_EVENT_PRIVATE_FLAG_SUPPORTS_ORIENTATION = 0x80;
    static constexpr int32_t MOTION_EVENT_PRIVATE_FLAG_SUPPORTS_DIRECTIONAL_ORIENTATION = 0x100;
    static constexpr int32_t INPUT_EVENT_FLAG_IS_ACCESSIBILITY_EVENT = 0x800;
    static constexpr int32_t INPUT_EVENT_FLAG_TAINTED = static_cast<int32_t>(0x80000000);
    static constexpr int32_t MOTION_EVENT_FLAG_TARGET_ACCESSIBILITY_FOCUS = 0x40000000;
    static constexpr int32_t DEFAULT_POINTER_ACCELERATION = 3;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_DEFAULT = -1;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_IMPULSE = 0;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_LSQ1 = 1;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_LSQ2 = 2;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_LSQ3 = 3;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_WLSQ2_DELTA = 4;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_WLSQ2_CENTRAL = 5;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_WLSQ2_RECENT = 6;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_INT1 = 7;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_INT2 = 8;
    static constexpr int32_t VELOCITY_TRACKER_STRATEGY_LEGACY = 9;
    static constexpr int32_t DEVICE_CLASS_KEYBOARD = 0x00000001;
    static constexpr int32_t DEVICE_CLASS_ALPHAKEY = 0x00000002;
    static constexpr int32_t DEVICE_CLASS_TOUCH = 0x00000004;
    static constexpr int32_t DEVICE_CLASS_CURSOR = 0x00000008;
    static constexpr int32_t DEVICE_CLASS_TOUCH_MT = 0x00000010;
    static constexpr int32_t DEVICE_CLASS_DPAD = 0x00000020;
    static constexpr int32_t DEVICE_CLASS_GAMEPAD = 0x00000040;
    static constexpr int32_t DEVICE_CLASS_SWITCH = 0x00000080;
    static constexpr int32_t DEVICE_CLASS_JOYSTICK = 0x00000100;
    static constexpr int32_t DEVICE_CLASS_VIBRATOR = 0x00000200;
    static constexpr int32_t DEVICE_CLASS_MIC = 0x00000400;
    static constexpr int32_t DEVICE_CLASS_EXTERNAL_STYLUS = 0x00000800;
    static constexpr int32_t DEVICE_CLASS_ROTARY_ENCODER = 0x00001000;
    static constexpr int32_t DEVICE_CLASS_SENSOR = 0x00002000;
    static constexpr int32_t DEVICE_CLASS_BATTERY = 0x00004000;
    static constexpr int32_t DEVICE_CLASS_LIGHT = 0x00008000;
    static constexpr int32_t DEVICE_CLASS_TOUCHPAD = 0x00010000;
    static constexpr int32_t DEVICE_CLASS_VIRTUAL = 0x20000000;
    static constexpr int32_t DEVICE_CLASS_EXTERNAL = 0x40000000;
};

}  // namespace android::os
