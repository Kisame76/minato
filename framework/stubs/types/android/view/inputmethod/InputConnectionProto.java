package android.view.inputmethod;

/**
 * Compile-only stub (ADR-0043 step d)), never in framework.jar: EditableInputConnection.dumpDebug names its field constants, which
 * protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not the
 * proto's field numbers, are what dumpDebug writes; it runs only for an input method's trace.
 */
public final class InputConnectionProto {
    public static final long CURSOR_CAPS_MODE = 1L;
    public static final long SELECTED_TEXT_START = 2L;
    public static final long SELECTED_TEXT_END = 3L;
}
