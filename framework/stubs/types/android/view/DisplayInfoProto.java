package android.view;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: DisplayInfo's dumpDebug names its field constants, which
 * protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not the
 * proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class DisplayInfoProto {
    public static final long LOGICAL_WIDTH = 1L;
    public static final long LOGICAL_HEIGHT = 2L;
    public static final long APP_WIDTH = 3L;
    public static final long APP_HEIGHT = 4L;
    public static final long NAME = 5L;
    public static final long FLAGS = 6L;
    public static final long CUTOUT = 7L;
}
