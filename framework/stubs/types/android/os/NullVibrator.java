package android.os;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: the vibrator of an input device without one,
 * which InputDevice.getVibrator gives once Vibrator is in framework.jar.
 */
public abstract class NullVibrator extends Vibrator {
    public static NullVibrator getInstance() {
        throw new RuntimeException("stub");
    }
}
