// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's haptic feedback.
class Vibrator {
    /** @hide */
    public void performHapticFeedback(int constant, boolean always, String reason, boolean fromIme) {
        throw new RuntimeException("stub");
    }
}
