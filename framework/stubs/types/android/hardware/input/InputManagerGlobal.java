package android.hardware.input;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: ViewConfiguration asks it for an input device's fling
 * thresholds.
 */
public final class InputManagerGlobal {
    public static InputManagerGlobal getInstance() {
        throw new RuntimeException("stub");
    }

    public android.view.InputDevice getInputDevice(int id) {
        throw new RuntimeException("stub");
    }

    public boolean injectInputEvent(android.view.InputEvent event, int mode) {
        throw new RuntimeException("stub");
    }

    public boolean injectInputEvent(android.view.InputEvent event, int mode, int targetUid) {
        throw new RuntimeException("stub");
    }
}
