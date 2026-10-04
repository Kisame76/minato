package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: InsetsSource.dumpDebug names its field constants,
 * which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not the
 * proto's field numbers, are what dumpDebug writes; it never runs in an app.
 */
public final class InsetsSourceProto {
    public static final long FRAME = 1L;
    public static final long TYPE = 2L;
    public static final long TYPE_NUMBER = 3L;
    public static final long VISIBLE = 4L;
    public static final long VISIBLE_FRAME = 5L;
}
