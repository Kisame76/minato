package com.android.internal.os;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: Log.wtf calls it; StrictMode hands the application object
 * to the activity manager with a violation (ADR-0039 step c1).
 */
public class RuntimeInit {
    public static void wtf(String tag, Throwable t, boolean system) {
        throw new RuntimeException("stub");
    }

    public static android.os.IBinder getApplicationObject() {
        throw new RuntimeException("stub");
    }
}
