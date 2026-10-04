package com.android.internal.util;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: statsd's generated atoms, whose event types
 * StrictMode.onUnsafeIntentLaunch switches on, and ContentProvider's event of a getType without permission (step
 * c2c1). javac inlines the constants; distinct values, not statsd's.
 */
public final class FrameworkStatsLog {
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__INTERNAL_NON_EXPORTED_COMPONENT_MATCH = 1;
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__EXPLICIT_INTENT_FILTER_UNMATCH = 2;
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__NULL_ACTION_MATCH = 3;

    public static final int GET_TYPE_ACCESSED_WITHOUT_PERMISSION = 1043;
    public static final int GET_TYPE_ACCESSED_WITHOUT_PERMISSION__LOCATION__PROVIDER_FRAMEWORK_PERMISSION = 1;
    public static final int GET_TYPE_ACCESSED_WITHOUT_PERMISSION__LOCATION__PROVIDER_CHECK_URI_PERMISSION = 2;

    public static void write(int code, int arg1, int arg2, String arg3, String arg4) {
        throw new RuntimeException("stub");
    }
}
