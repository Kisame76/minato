package android.os;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: MessageQueue's dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct
 * but not the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class MessageQueueProto {
    public static final long MESSAGES = 1L;
    public static final long IS_POLLING_LOCKED = 2L;
    public static final long IS_QUITTING = 3L;
}
