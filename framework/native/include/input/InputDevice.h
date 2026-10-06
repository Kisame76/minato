// Stand-in for libinput's input/InputDevice.h (ADR-0045 step 7b1), which the fetch leaves out: it would pull in
// KeyCharacterMap.h and the AIDL of input devices. libinput's Input.cpp takes three things from it: the reserved
// device ids, which a focus, capture, drag or touch mode event carries (the enum below, as InputDevice.h has it at
// android-15.0.0_r1), ftl's enum_string and std::map, which InputDevice.h includes.
#pragma once

#include <android/os/IInputConstants.h>
#include <ftl/enum.h>
#include <stdint.h>

#include <map>

namespace android {

enum ReservedInputDeviceId : int32_t {
    // Device id representing an invalid device
    INVALID_INPUT_DEVICE_ID = android::os::IInputConstants::INVALID_INPUT_DEVICE_ID,
    // Device id of a special "virtual" keyboard that is always present.
    VIRTUAL_KEYBOARD_ID = -1,
    // Device id of the "built-in" keyboard if there is one.
    BUILT_IN_KEYBOARD_ID = 0,
    // First device id available for dynamic devices
    END_RESERVED_ID = 1,
};

}  // namespace android
