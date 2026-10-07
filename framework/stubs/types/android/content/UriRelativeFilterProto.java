package android.content;

/**
 * Compile-only stub (ADR-0045 d1), never in framework.jar: UriRelativeFilter's dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values,
 * distinct but not the proto's field numbers, are what dumpDebug writes, as BundleProto's; it does not run in
 * an app.
 */
public final class UriRelativeFilterProto {
    public static final long URI_PART = 1L;
    public static final long PATTERN_TYPE = 2L;
    public static final long FILTER = 3L;
}
