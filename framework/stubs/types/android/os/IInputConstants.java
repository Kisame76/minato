package android.os;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: KeyEvent's flags take their values from it, which
 * aidl generates from frameworks/native's IInputConstants.aidl (not fetched). javac inlines them; the values are the
 * public API's (FLAG_CANCELED) and the tag's [inferred for the hidden two].
 */
public interface IInputConstants {
    int INPUT_EVENT_FLAG_CANCELED = 0x20;

    int INPUT_EVENT_FLAG_IS_ACCESSIBILITY_EVENT = 0x800;

    int INPUT_EVENT_FLAG_TAINTED = 0x80000000;

    int MOTION_EVENT_FLAG_WINDOW_IS_OBSCURED = 0x1;

    int MOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED = 0x2;

    int MOTION_EVENT_FLAG_HOVER_EXIT_PENDING = 0x4;

    int MOTION_EVENT_FLAG_IS_GENERATED_GESTURE = 0x8;

    int MOTION_EVENT_FLAG_NO_FOCUS_CHANGE = 0x40;

    int MOTION_EVENT_FLAG_TARGET_ACCESSIBILITY_FOCUS = 0x40000000;
}
