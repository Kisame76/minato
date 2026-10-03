package android.content.pm;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: FeatureInfo's dumpDebug names its field constants,
 * which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not
 * the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class FeatureInfoProto {
    public static final long NAME = 1L;
    public static final long VERSION = 2L;
    public static final long GLES_VERSION = 3L;
    public static final long FLAGS = 4L;
}
