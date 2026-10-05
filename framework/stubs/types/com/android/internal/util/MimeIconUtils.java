package com.android.internal.util;

import android.content.ContentResolver;

/**
 * Compile-only stub (ADR-0045, gap 13a), never in framework.jar: ContentResolver.getTypeInfo's icons, which are
 * Icons, compile-only as well.
 */
public class MimeIconUtils {
    public static ContentResolver.MimeTypeInfo getTypeInfo(String mimeType) {
        throw new RuntimeException("stub");
    }
}
