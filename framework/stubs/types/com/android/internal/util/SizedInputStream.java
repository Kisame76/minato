package com.android.internal.util;

import java.io.InputStream;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: FileUtils.copy with a byte count wraps its
 * input in it on the userspace path; SharedPreferences never copies.
 */
public class SizedInputStream extends InputStream {
    public SizedInputStream(InputStream wrapped, long length) {
        throw new RuntimeException("stub");
    }

    @Override
    public int read() {
        throw new RuntimeException("stub");
    }
}
