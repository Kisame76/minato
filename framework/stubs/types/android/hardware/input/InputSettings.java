package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: the input settings, which read
 * Settings.System. javac inlines the pointer speeds, AOSP's values.
 */
public final class InputSettings {
    public static final int MIN_POINTER_SPEED = -7;

    public static final int MAX_POINTER_SPEED = 7;

    public static float getMaximumObscuringOpacityForTouch(android.content.Context context) {
        throw new RuntimeException("stub");
    }

    public static boolean isStylusPointerIconEnabled(android.content.Context context) {
        throw new RuntimeException("stub");
    }

    public static boolean isAccessibilityStickyKeysFeatureEnabled() {
        throw new RuntimeException("stub");
    }
}
