package android.content;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: Configuration.dumpDebug's resources variant name its
 * field constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values,
 * distinct but not the proto's field numbers, are what dumpDebug writes and readFromProto expects; neither runs in
 * an app.
 */
public final class ResourcesConfigurationProto {
    public static final long CONFIGURATION = 1L;
    public static final long SCREEN_HEIGHT_PX = 2L;
    public static final long SCREEN_WIDTH_PX = 3L;
    public static final long SDK_VERSION = 4L;
}
