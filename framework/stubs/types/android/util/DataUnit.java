package android.util;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: FileUtils' hidden size helpers
 * (roundStorageSize and the like) convert with it; SharedPreferences never does.
 */
public enum DataUnit {
    KILOBYTES, MEGABYTES, GIGABYTES, TERABYTES, KIBIBYTES, MEBIBYTES, GIBIBYTES, TEBIBYTES;

    public long toBytes(long v) {
        throw new RuntimeException("stub");
    }
}
