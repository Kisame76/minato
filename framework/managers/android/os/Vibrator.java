// The written part of Vibrator (the Unity test APK probe: View's fields name the type, and a class whose fields cannot be
// resolved cannot be read by reflection), which tools/managergen joins with the API's methods. Minato's devices give an
// app no vibrator service yet (getSystemService returns null for it, as ADR-0045 decision 4 has it for the feature), so
// the class is the abstract API: the hidden constructor that subclasses call, and the haptic feedback View asks for,
// which does nothing here.
class Vibrator {
    /** @hide */
    protected Vibrator() {
    }

    /** @hide */
    public void performHapticFeedback(int constant, boolean always, String reason, boolean fromIme) {
    }
}
