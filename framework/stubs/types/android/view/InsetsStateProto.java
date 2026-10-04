package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: InsetsState.dumpDebug names its field constants,
 * which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not the
 * proto's field numbers, are what dumpDebug writes; it never runs in an app.
 */
public final class InsetsStateProto {
    public static final long DISPLAY_CUTOUT = 1L;
    public static final long DISPLAY_FRAME = 2L;
    public static final long SOURCES = 3L;
}
