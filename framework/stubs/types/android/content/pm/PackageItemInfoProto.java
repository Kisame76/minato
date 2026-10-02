package android.content.pm;

/**
 * Compile-only stub (ADR-0039 b3.4), never in framework.jar: PackageItemInfo.dumpDebug names its field constants, which
 * protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not the
 * proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class PackageItemInfoProto {
    public static final long NAME = 1L;
    public static final long PACKAGE_NAME = 2L;
    public static final long LABEL_RES = 3L;
    public static final long NON_LOCALIZED_LABEL = 4L;
    public static final long ICON = 5L;
    public static final long BANNER = 6L;
    public static final long IS_ARCHIVED = 7L;
}
