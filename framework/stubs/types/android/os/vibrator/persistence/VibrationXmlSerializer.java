package android.os.vibrator.persistence;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: a vibration's XML form, which NotificationChannel's own XML, the
 * system's, reads and writes.
 */
public final class VibrationXmlSerializer {
    public static final int FLAG_ALLOW_HIDDEN_APIS = 1;

    public static void serialize(android.os.VibrationEffect effect, java.io.Writer writer, int flags)
            throws java.io.IOException {
        throw new RuntimeException("stub");
    }
}
