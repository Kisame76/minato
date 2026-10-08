package android.content;

/**
 * Compile-only stub (the framework breadth's batch 1), never in framework.jar: ClipDescription's dumpDebug names its
 * field constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values,
 * distinct but not the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class ClipDescriptionProto {
    public static final long MIME_TYPES = 1L;
    public static final long LABEL = 2L;
    public static final long EXTRAS = 3L;
    public static final long TIMESTAMP_MS = 4L;
}
