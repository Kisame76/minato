package android.os;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: Looper's dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct
 * but not the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class LooperProto {
    public static final long THREAD_NAME = 1L;
    public static final long THREAD_ID = 2L;
    public static final long QUEUE = 3L;
}
