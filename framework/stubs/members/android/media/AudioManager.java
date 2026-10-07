// Hidden members of the public API's compile-only stub (ADR-0045 step 7c1, typed keys), which tools/managergen joins
// with it: AOSP's SoundEffectConstants picks a repeated focus move's sound with them (stub-references.txt).
class AudioManager {
    /** @hide */
    public static final int NUM_NAVIGATION_REPEAT_SOUND_EFFECTS = 4;

    /** @hide */
    public static int getNthNavigationRepeatSoundEffect(int n) {
        throw new RuntimeException("stub");
    }
}
