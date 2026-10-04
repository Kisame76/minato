package android.view.contentcapture;

import android.util.ArraySet;
import java.util.List;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: content capture stays off on Minato: no Context
 * gives a ContentCaptureManager, as on a device without the service.
 */
public final class ContentCaptureHelper {
    public static boolean sVerbose;

    public static boolean sDebug;

    public static void setLoggingLevel() {
        throw new RuntimeException("stub");
    }

    public static void setLoggingLevel( int level) {
        throw new RuntimeException("stub");
    }

    public static <T> ArraySet<T> toSet( List<T> list) {
        throw new RuntimeException("stub");
    }
}
