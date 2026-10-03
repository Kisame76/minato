package android.app;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: WindowConfiguration.dumpDebug and readFromProto name
 * its field constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these
 * values, distinct but not the proto's field numbers, are what dumpDebug writes and readFromProto expects; neither
 * runs in an app.
 */
public final class WindowConfigurationProto {
    public static final long ACTIVITY_TYPE = 1L;
    public static final long APP_BOUNDS = 2L;
    public static final long BOUNDS = 3L;
    public static final long MAX_BOUNDS = 4L;
    public static final long WINDOWING_MODE = 5L;
}
