package com.android.internal.util;

import android.annotation.NonNull;

/**
 * Compile-only stub (ADR-0039 b3.3), never in framework.jar: OverlayableInfo's constructor calls it, and
 * PackageManager.ComponentEnabledSetting's checks its int annotations (b3.5a). AOSP's class
 * checks the other annotations against hidden constants of UserHandle, PackageManager and Intent, which our stubs
 * do not carry; OverlayableInfo is in framework.jar only because ApkAssets' registration looks it up.
 */
public final class AnnotationValidations {
    private AnnotationValidations() {}

    public static void validate(Class<NonNull> annotation, NonNull ignored, Object value) {
        throw new RuntimeException("stub");
    }

    public static void validate(Class<? extends java.lang.annotation.Annotation> annotation,
            java.lang.annotation.Annotation ignored, int value) {
        throw new RuntimeException("stub");
    }
}
