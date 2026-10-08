package android.os;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: a vibrator's capabilities, against which a
 * vibration effect checks itself before the vibrator service plays it; Minato's vibrator comes with the vibrator's
 * step.
 */
public class VibratorInfo {
    public boolean hasAmplitudeControl() {
        throw new RuntimeException("stub");
    }

    public boolean hasFrequencyControl() {
        throw new RuntimeException("stub");
    }

    public int isEffectSupported(int effectId) {
        throw new RuntimeException("stub");
    }

    public boolean isPrimitiveSupported(int primitiveId) {
        throw new RuntimeException("stub");
    }
}
