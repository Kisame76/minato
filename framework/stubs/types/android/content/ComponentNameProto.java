package android.content;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: ComponentName's dumpDebug names its field constants,
 * which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not
 * the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class ComponentNameProto {
    public static final long PACKAGE_NAME = 1L;
    public static final long CLASS_NAME = 2L;
}
