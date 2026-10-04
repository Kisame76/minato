// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's haptic feedback, and the ctor that an input device's vibrators call (ADR-0045, the input service).
class Vibrator {
    /** @hide */
    public void performHapticFeedback(int constant, boolean always, String reason, boolean fromIme) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public Vibrator() {
        throw new RuntimeException("stub");
    }
}
