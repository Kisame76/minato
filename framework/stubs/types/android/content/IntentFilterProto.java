package android.content;

/**
 * Compile-only stub (ADR-0045 d1), never in framework.jar: IntentFilter's dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values,
 * distinct but not the proto's field numbers, are what dumpDebug writes, as BundleProto's; it does not run in
 * an app.
 */
public final class IntentFilterProto {
    public static final long ACTIONS = 1L;
    public static final long CATEGORIES = 2L;
    public static final long DATA_SCHEMES = 3L;
    public static final long DATA_SCHEME_SPECS = 4L;
    public static final long DATA_AUTHORITIES = 5L;
    public static final long DATA_PATHS = 6L;
    public static final long DATA_TYPES = 7L;
    public static final long PRIORITY = 8L;
    public static final long HAS_PARTIAL_TYPES = 9L;
    public static final long GET_AUTO_VERIFY = 10L;
    public static final long EXTRAS = 11L;
    public static final long MIME_GROUPS = 12L;
    public static final long URI_RELATIVE_FILTER_GROUPS = 13L;
}
