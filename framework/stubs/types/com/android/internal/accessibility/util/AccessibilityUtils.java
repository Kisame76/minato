package com.android.internal.accessibility.util;

/** Compile-only stub (ADR-0043 step d)), never in framework.jar: TextView asks it only while accessibility is on. */
public final class AccessibilityUtils {
    public @interface A11yTextChangeType {
    }

    public static final int NONE = 0;
    public static final int TEXT = 1;
    public static final int PARCELABLE_SPAN = 2;

    public static int textOrSpanChanged(CharSequence before, CharSequence after) {
        throw new RuntimeException("stub");
    }
}
