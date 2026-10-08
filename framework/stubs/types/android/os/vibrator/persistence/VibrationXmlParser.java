package android.os.vibrator.persistence;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: a vibration's XML form, which NotificationChannel's own XML, the
 * system's, reads and writes.
 */
public final class VibrationXmlParser {
    public static final int FLAG_ALLOW_HIDDEN_APIS = 1;

    public static android.os.VibrationEffect parseVibrationEffect(java.io.Reader reader, int flags)
            throws java.io.IOException {
        throw new RuntimeException("stub");
    }
}
