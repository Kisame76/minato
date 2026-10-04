package com.android.internal.os;

import android.os.Handler;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: StrictMode posts its asynchronous dropbox
 * calls to it.
 */
public final class BackgroundThread {
    public static Handler getHandler() {
        throw new RuntimeException("stub");
    }
}
