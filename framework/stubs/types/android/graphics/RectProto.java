package android.graphics;

/**
 * Compile-only stub (ADR-0039 b3.3), never in framework.jar: Rect.dumpDebug and readFromProto name its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values,
 * distinct but not the proto's field numbers, are what dumpDebug writes and readFromProto expects; neither runs in
 * an app.
 */
public final class RectProto {
    public static final long LEFT = 1L;
    public static final long TOP = 2L;
    public static final long RIGHT = 3L;
    public static final long BOTTOM = 4L;
}
