package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: an input device's vibrator; Minato's devices
 * have none.
 */
public final class InputDeviceVibrator extends android.os.Vibrator {
    public InputDeviceVibrator(int deviceId, int vibratorId) {
        throw new RuntimeException("stub");
    }

    public boolean hasVibrator() {
        throw new RuntimeException("stub");
    }

    public boolean hasAmplitudeControl() {
        throw new RuntimeException("stub");
    }

    public void cancel() {
        throw new RuntimeException("stub");
    }
}
