/*
 * Our stand-in for statsd's generated atoms (frameworks/proto_logging, which we do not fetch): the event types
 * StrictMode.onUnsafeIntentLaunch switches on (ADR-0039 step c1), ContentProvider's event of a getType without
 * permission (step c2c1), View.performClick's touch gesture atom (step c2c2) and ChangeReporter's compat change reports
 * (step c2e), whose writes Android sends to statsd. Minato has no statsd, so every write does nothing. The touch
 * gesture's atom ID and classifications carry AOSP's values [inferred: atoms.proto], the compat change's too
 * [verified: atoms.proto:458-459 and 14523-14538 at android-15.0.0_r1], the others the distinct values the
 * compile-only stub gave them; javac inlines them, so only reflection sees ours.
 */
package com.android.internal.util;

public final class FrameworkStatsLog {
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__INTERNAL_NON_EXPORTED_COMPONENT_MATCH = 1;
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__EXPLICIT_INTENT_FILTER_UNMATCH = 2;
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__NULL_ACTION_MATCH = 3;

    public static final int GET_TYPE_ACCESSED_WITHOUT_PERMISSION = 1043;
    public static final int GET_TYPE_ACCESSED_WITHOUT_PERMISSION__LOCATION__PROVIDER_FRAMEWORK_PERMISSION = 1;
    public static final int GET_TYPE_ACCESSED_WITHOUT_PERMISSION__LOCATION__PROVIDER_CHECK_URI_PERMISSION = 2;

    public static final int TOUCH_GESTURE_CLASSIFIED = 177;
    public static final int TOUCH_GESTURE_CLASSIFIED__CLASSIFICATION__UNKNOWN_CLASSIFICATION = 0;
    public static final int TOUCH_GESTURE_CLASSIFIED__CLASSIFICATION__SINGLE_TAP = 1;
    public static final int TOUCH_GESTURE_CLASSIFIED__CLASSIFICATION__DOUBLE_TAP = 2;
    public static final int TOUCH_GESTURE_CLASSIFIED__CLASSIFICATION__LONG_PRESS = 3;
    public static final int TOUCH_GESTURE_CLASSIFIED__CLASSIFICATION__DEEP_PRESS = 4;
    public static final int TOUCH_GESTURE_CLASSIFIED__CLASSIFICATION__SCROLL = 5;

    public static final int APP_COMPATIBILITY_CHANGE_REPORTED = 228;
    public static final int APP_COMPATIBILITY_CHANGE_REPORTED__STATE__UNKNOWN_STATE = 0;
    public static final int APP_COMPATIBILITY_CHANGE_REPORTED__STATE__ENABLED = 1;
    public static final int APP_COMPATIBILITY_CHANGE_REPORTED__STATE__DISABLED = 2;
    public static final int APP_COMPATIBILITY_CHANGE_REPORTED__STATE__LOGGED = 3;
    public static final int APP_COMPATIBILITY_CHANGE_REPORTED__SOURCE__UNKNOWN_SOURCE = 0;
    public static final int APP_COMPATIBILITY_CHANGE_REPORTED__SOURCE__APP_PROCESS = 1;
    public static final int APP_COMPATIBILITY_CHANGE_REPORTED__SOURCE__SYSTEM_SERVER = 2;

    private FrameworkStatsLog() {
    }

    /** ContentProvider's getType without permission; statsd is absent, so it goes nowhere. */
    public static void write(int code, int arg1, int arg2, String arg3, String arg4) {
    }

    /** View.performClick's touch gesture; statsd is absent, so it goes nowhere. */
    public static void write(int code, String arg1, int arg2) {
    }

    /** ChangeReporter's compat change report; statsd is absent, so it goes nowhere. */
    public static void write(int code, int arg1, long arg2, int arg3, int arg4) {
    }
}
