package android.content;

/**
 * Compile-only stub (ADR-0045 d1), never in framework.jar: UriRelativeFilterGroup's dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values,
 * distinct but not the proto's field numbers, are what dumpDebug writes, as BundleProto's; it does not run in
 * an app.
 */
public final class UriRelativeFilterGroupProto {
    public static final long ACTION = 1L;
    public static final long URI_RELATIVE_FILTERS = 2L;
}
