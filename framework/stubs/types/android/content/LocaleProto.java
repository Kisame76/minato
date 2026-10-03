package android.content;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: Configuration's locales in dumpDebug and readFromProto
 * name its field constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these
 * values, distinct but not the proto's field numbers, are what dumpDebug writes and readFromProto expects; neither
 * runs in an app.
 */
public final class LocaleProto {
    public static final long COUNTRY = 1L;
    public static final long LANGUAGE = 2L;
    public static final long SCRIPT = 3L;
    public static final long VARIANT = 4L;
}
