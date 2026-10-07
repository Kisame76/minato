// Ours in place of libinput's InputEventLabels.cpp (ADR-0045 step 7b1), which the fetch leaves out: its tables need a
// header generated from the kernel's input codes. libinput asks it for two things:
//   - an axis's label and an axis by its label, for MotionEvent.axisToString and axisFromString: kAxisLabels is
//     libinput's AXES_SEQUENCE at android-15.0.0_r1, every AMOTION_EVENT_AXIS_ without its prefix at the index of
//     its value, with RESERVED_29 to RESERVED_31 between RELATIVE_Y and GENERIC_1 as libinput has them
//     (framework/tests/axis_labels_test.py checks the table against MotionEvent.java's AXIS_ constants). A label
//     answers by value as libinput's vector does, and a value by its exact label as libinput's map does;
//   - a key code's label and a key code by its label (step 7c1): for KeyEvent.keyCodeToString and keyCodeFromString,
//     through AOSP's android_view_KeyEvent.cpp and libinput's KeyEvent::getLabel and getKeyCodeFromLabel, and for the
//     keys a key character map names (KeyCharacterMap.cpp). kKeyLabels holds Android 15's 319, KEYCODE_UNKNOWN to
//     KEYCODE_SCREENSHOT, each KEYCODE_ constant's name without the prefix at the index of its value, as KeyEvent.java
//     at android-15.0.0_r1 has them (framework/tests/keycode_labels_test.py checks the two agree). A label answers by
//     value as libinput's vector does, and a value by its exact label as libinput's map does.
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

constexpr const char *kKeyLabels[] = {
        "UNKNOWN", "SOFT_LEFT", "SOFT_RIGHT", "HOME", "BACK", "CALL", "ENDCALL", "0", "1", "2", "3", "4", "5", "6", "7",
        "8", "9", "STAR", "POUND", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "DPAD_CENTER", "VOLUME_UP",
        "VOLUME_DOWN", "POWER", "CAMERA", "CLEAR", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
        "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "COMMA", "PERIOD", "ALT_LEFT", "ALT_RIGHT",
        "SHIFT_LEFT", "SHIFT_RIGHT", "TAB", "SPACE", "SYM", "EXPLORER", "ENVELOPE", "ENTER", "DEL", "GRAVE", "MINUS",
        "EQUALS", "LEFT_BRACKET", "RIGHT_BRACKET", "BACKSLASH", "SEMICOLON", "APOSTROPHE", "SLASH", "AT", "NUM",
        "HEADSETHOOK", "FOCUS", "PLUS", "MENU", "NOTIFICATION", "SEARCH", "MEDIA_PLAY_PAUSE", "MEDIA_STOP",
        "MEDIA_NEXT", "MEDIA_PREVIOUS", "MEDIA_REWIND", "MEDIA_FAST_FORWARD", "MUTE", "PAGE_UP", "PAGE_DOWN",
        "PICTSYMBOLS", "SWITCH_CHARSET", "BUTTON_A", "BUTTON_B", "BUTTON_C", "BUTTON_X", "BUTTON_Y", "BUTTON_Z",
        "BUTTON_L1", "BUTTON_R1", "BUTTON_L2", "BUTTON_R2", "BUTTON_THUMBL", "BUTTON_THUMBR", "BUTTON_START",
        "BUTTON_SELECT", "BUTTON_MODE", "ESCAPE", "FORWARD_DEL", "CTRL_LEFT", "CTRL_RIGHT", "CAPS_LOCK", "SCROLL_LOCK",
        "META_LEFT", "META_RIGHT", "FUNCTION", "SYSRQ", "BREAK", "MOVE_HOME", "MOVE_END", "INSERT", "FORWARD",
        "MEDIA_PLAY", "MEDIA_PAUSE", "MEDIA_CLOSE", "MEDIA_EJECT", "MEDIA_RECORD", "F1", "F2", "F3", "F4", "F5", "F6",
        "F7", "F8", "F9", "F10", "F11", "F12", "NUM_LOCK", "NUMPAD_0", "NUMPAD_1", "NUMPAD_2", "NUMPAD_3", "NUMPAD_4",
        "NUMPAD_5", "NUMPAD_6", "NUMPAD_7", "NUMPAD_8", "NUMPAD_9", "NUMPAD_DIVIDE", "NUMPAD_MULTIPLY",
        "NUMPAD_SUBTRACT", "NUMPAD_ADD", "NUMPAD_DOT", "NUMPAD_COMMA", "NUMPAD_ENTER", "NUMPAD_EQUALS",
        "NUMPAD_LEFT_PAREN", "NUMPAD_RIGHT_PAREN", "VOLUME_MUTE", "INFO", "CHANNEL_UP", "CHANNEL_DOWN", "ZOOM_IN",
        "ZOOM_OUT", "TV", "WINDOW", "GUIDE", "DVR", "BOOKMARK", "CAPTIONS", "SETTINGS", "TV_POWER", "TV_INPUT",
        "STB_POWER", "STB_INPUT", "AVR_POWER", "AVR_INPUT", "PROG_RED", "PROG_GREEN", "PROG_YELLOW", "PROG_BLUE",
        "APP_SWITCH", "BUTTON_1", "BUTTON_2", "BUTTON_3", "BUTTON_4", "BUTTON_5", "BUTTON_6", "BUTTON_7", "BUTTON_8",
        "BUTTON_9", "BUTTON_10", "BUTTON_11", "BUTTON_12", "BUTTON_13", "BUTTON_14", "BUTTON_15", "BUTTON_16",
        "LANGUAGE_SWITCH", "MANNER_MODE", "3D_MODE", "CONTACTS", "CALENDAR", "MUSIC", "CALCULATOR", "ZENKAKU_HANKAKU",
        "EISU", "MUHENKAN", "HENKAN", "KATAKANA_HIRAGANA", "YEN", "RO", "KANA", "ASSIST", "BRIGHTNESS_DOWN",
        "BRIGHTNESS_UP", "MEDIA_AUDIO_TRACK", "SLEEP", "WAKEUP", "PAIRING", "MEDIA_TOP_MENU", "11", "12",
        "LAST_CHANNEL", "TV_DATA_SERVICE", "VOICE_ASSIST", "TV_RADIO_SERVICE", "TV_TELETEXT", "TV_NUMBER_ENTRY",
        "TV_TERRESTRIAL_ANALOG", "TV_TERRESTRIAL_DIGITAL", "TV_SATELLITE", "TV_SATELLITE_BS", "TV_SATELLITE_CS",
        "TV_SATELLITE_SERVICE", "TV_NETWORK", "TV_ANTENNA_CABLE", "TV_INPUT_HDMI_1", "TV_INPUT_HDMI_2",
        "TV_INPUT_HDMI_3", "TV_INPUT_HDMI_4", "TV_INPUT_COMPOSITE_1", "TV_INPUT_COMPOSITE_2", "TV_INPUT_COMPONENT_1",
        "TV_INPUT_COMPONENT_2", "TV_INPUT_VGA_1", "TV_AUDIO_DESCRIPTION", "TV_AUDIO_DESCRIPTION_MIX_UP",
        "TV_AUDIO_DESCRIPTION_MIX_DOWN", "TV_ZOOM_MODE", "TV_CONTENTS_MENU", "TV_MEDIA_CONTEXT_MENU",
        "TV_TIMER_PROGRAMMING", "HELP", "NAVIGATE_PREVIOUS", "NAVIGATE_NEXT", "NAVIGATE_IN", "NAVIGATE_OUT",
        "STEM_PRIMARY", "STEM_1", "STEM_2", "STEM_3", "DPAD_UP_LEFT", "DPAD_DOWN_LEFT", "DPAD_UP_RIGHT",
        "DPAD_DOWN_RIGHT", "MEDIA_SKIP_FORWARD", "MEDIA_SKIP_BACKWARD", "MEDIA_STEP_FORWARD", "MEDIA_STEP_BACKWARD",
        "SOFT_SLEEP", "CUT", "COPY", "PASTE", "SYSTEM_NAVIGATION_UP", "SYSTEM_NAVIGATION_DOWN",
        "SYSTEM_NAVIGATION_LEFT", "SYSTEM_NAVIGATION_RIGHT", "ALL_APPS", "REFRESH", "THUMBS_UP", "THUMBS_DOWN",
        "PROFILE_SWITCH", "VIDEO_APP_1", "VIDEO_APP_2", "VIDEO_APP_3", "VIDEO_APP_4", "VIDEO_APP_5", "VIDEO_APP_6",
        "VIDEO_APP_7", "VIDEO_APP_8", "FEATURED_APP_1", "FEATURED_APP_2", "FEATURED_APP_3", "FEATURED_APP_4",
        "DEMO_APP_1", "DEMO_APP_2", "DEMO_APP_3", "DEMO_APP_4", "KEYBOARD_BACKLIGHT_DOWN", "KEYBOARD_BACKLIGHT_UP",
        "KEYBOARD_BACKLIGHT_TOGGLE", "STYLUS_BUTTON_PRIMARY", "STYLUS_BUTTON_SECONDARY", "STYLUS_BUTTON_TERTIARY",
        "STYLUS_BUTTON_TAIL", "RECENT_APPS", "MACRO_1", "MACRO_2", "MACRO_3", "MACRO_4", "EMOJI_PICKER", "SCREENSHOT",
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

const char *InputEventLookup::getLabelByKeyCode(int32_t keyCode) {
    if (static_cast<size_t>(keyCode) < std::size(kKeyLabels)) {
        return kKeyLabels[keyCode];
    }
    return nullptr;
}

std::optional<int> InputEventLookup::getKeyCodeByLabel(const char *label) {
    for (size_t keyCode = 0; keyCode < std::size(kKeyLabels); keyCode++) {
        if (strcmp(kKeyLabels[keyCode], label) == 0) {
            return static_cast<int>(keyCode);
        }
    }
    return std::nullopt;
}

}  // namespace android
