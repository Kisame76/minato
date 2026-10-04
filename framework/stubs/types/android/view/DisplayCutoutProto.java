package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: DisplayCutout.dumpDebug names its field constants,
 * which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not the
 * proto's field numbers, are what dumpDebug writes; it never runs in an app.
 */
public final class DisplayCutoutProto {
    public static final long BOUND_BOTTOM = 1L;
    public static final long BOUND_LEFT = 2L;
    public static final long BOUND_RIGHT = 3L;
    public static final long BOUND_TOP = 4L;
    public static final long INSETS = 5L;
    public static final long SIDE_OVERRIDES = 6L;
    public static final long WATERFALL_INSETS = 7L;
}
