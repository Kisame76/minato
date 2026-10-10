package android.service.print;

/**
 * Compile-only stub (the Unity test APK probe's gap sort, category 1), never in framework.jar: the enum values of
 * core/proto/android/service/print.proto that PrintAttributes' color and duplex modes are; javac inlines them. The
 * values are the public API's (PrintAttributes.COLOR_MODE_* and DUPLEX_MODE_* in api/current.txt).
 */
public final class PrintAttributesProto {
    public static final int COLOR_MODE_MONOCHROME = 1;
    public static final int COLOR_MODE_COLOR = 2;
    public static final int DUPLEX_MODE_NONE = 1;
    public static final int DUPLEX_MODE_LONG_EDGE = 2;
    public static final int DUPLEX_MODE_SHORT_EDGE = 4;
}
