package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: an input device's vibrators; Minato's
 * devices have none.
 */
public final class InputDeviceVibratorManager extends android.os.VibratorManager {
    public InputDeviceVibratorManager(int deviceId) {
        throw new RuntimeException("stub");
    }

    public int[] getVibratorIds() {
        throw new RuntimeException("stub");
    }

    public android.os.Vibrator getVibrator(int vibratorId) {
        throw new RuntimeException("stub");
    }

    public android.os.Vibrator getDefaultVibrator() {
        throw new RuntimeException("stub");
    }

    public void cancel() {
        throw new RuntimeException("stub");
    }
}
