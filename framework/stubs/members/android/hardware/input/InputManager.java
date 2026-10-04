// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Instrumentation injects input synchronously. javac inlines it: frameworks/native's InputEventInjectionSync's
// WAIT_FOR_FINISHED [inferred].
class InputManager {
    /** @hide */
    public static final int INJECT_INPUT_EVENT_MODE_WAIT_FOR_FINISH = 2;
}
