package android.app;

/**
 * Compile-only stub (the Unity test APK resolve list, batch E), never in framework.jar: the proto class of ApplicationExitInfo,
 * whose field numbers ApplicationExitInfo's readFromProto and writeToProto name. javac inlines the constants, so
 * those two methods hold the numbers below, which are not the proto's own (the proto file is not fetched): the
 * system server saves exit reasons through them, and Minato has none.
 */
public final class ApplicationExitInfoProto {
    public static final long PID = 1L;
    public static final long REAL_UID = 2L;
    public static final long PACKAGE_UID = 3L;
    public static final long DEFINING_UID = 4L;
    public static final long PROCESS_NAME = 5L;
    public static final long CONNECTION_GROUP = 6L;
    public static final long REASON = 7L;
    public static final long SUB_REASON = 8L;
    public static final long STATUS = 9L;
    public static final long IMPORTANCE = 10L;
    public static final long PSS = 11L;
    public static final long RSS = 12L;
    public static final long TIMESTAMP = 13L;
    public static final long DESCRIPTION = 14L;
    public static final long STATE = 15L;
    public static final long TRACE_FILE = 16L;
}
