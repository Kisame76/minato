package com.android.internal.os;

/** Compile-only stub (ADR-0039 b3.1), never in framework.jar: Log.wtf calls it. */
public class RuntimeInit {
    public static void wtf(String tag, Throwable t, boolean system) {
        throw new RuntimeException("stub");
    }
}
