package android.os;

/**
 * Compile-only stub (ADR-0043 d2), never in framework.jar: PersistableBundle's dumpDebug names its field constants,
 * which protoc's javastream plugin generates from core/proto, as Bundle's does (BundleProto). javac inlines them, so
 * these values, distinct but not the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class PersistableBundleProto {
    public static final long PARCELLED_DATA_SIZE = 1L;
    public static final long MAP_DATA = 2L;
}
