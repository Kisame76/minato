package android.view.inputmethod;

/**
 * Compile-only stub (ADR-0043 step d)), never in framework.jar: EditorInfo.dumpDebug names its field constants, which protoc's
 * javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not the proto's field
 * numbers, are what dumpDebug writes; it runs only for an input method's trace.
 */
public final class EditorInfoProto {
    public static final long INPUT_TYPE = 1L;
    public static final long IME_OPTIONS = 2L;
    public static final long PRIVATE_IME_OPTIONS = 3L;
    public static final long PACKAGE_NAME = 4L;
    public static final long FIELD_ID = 5L;
    public static final long TARGET_INPUT_METHOD_USER_ID = 6L;
}
