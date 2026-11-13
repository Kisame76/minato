package android.hardware.vibrator.V1_0;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the vibrator HAL's effect strengths, which
 * VibrationEffect names; javac inlines them, with the HAL's values.
 */
public final class EffectStrength {
    public static final byte LIGHT = 0;
    public static final byte MEDIUM = 1;
    public static final byte STRONG = 2;
}
