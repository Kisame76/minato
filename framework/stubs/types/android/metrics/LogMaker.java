package android.metrics;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: a metrics log entry, which
 * StatusBarNotification.getLogMaker makes for the system's notification logging.
 */
public class LogMaker {
    public LogMaker(int category) {
        throw new RuntimeException("stub");
    }

    public LogMaker setPackageName(String packageName) {
        throw new RuntimeException("stub");
    }

    public LogMaker addTaggedData(int tag, Object value) {
        throw new RuntimeException("stub");
    }
}
