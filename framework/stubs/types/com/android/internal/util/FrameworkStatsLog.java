package com.android.internal.util;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: statsd's generated atoms, whose event types
 * StrictMode.onUnsafeIntentLaunch switches on. javac inlines the constants; distinct values, not statsd's.
 */
public final class FrameworkStatsLog {
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__INTERNAL_NON_EXPORTED_COMPONENT_MATCH = 1;
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__EXPLICIT_INTENT_FILTER_UNMATCH = 2;
    public static final int UNSAFE_INTENT_EVENT_REPORTED__EVENT_TYPE__NULL_ACTION_MATCH = 3;
}
