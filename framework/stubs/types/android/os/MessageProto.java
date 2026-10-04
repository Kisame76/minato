package android.os;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: Message's dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct
 * but not the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class MessageProto {
    public static final long WHEN = 1L;
    public static final long CALLBACK = 2L;
    public static final long WHAT = 3L;
    public static final long ARG1 = 4L;
    public static final long ARG2 = 5L;
    public static final long OBJ = 6L;
    public static final long TARGET = 7L;
    public static final long BARRIER = 8L;
}
