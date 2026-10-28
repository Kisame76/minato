/*
 * A vibrator that does not exist, under AOSP's name (gap 1 of the compatibility plan, batch 3): what a vibrator
 * manager gives for an ID it does not have and an input device without a vibrator, as Minato's are, gets from
 * InputDevice.getVibrator. Our own, with AOSP's answers: no vibrator, no amplitude control, and vibrations and cancels
 * that do nothing.
 */
package android.os;

/** @hide */
public class NullVibrator extends Vibrator {
    private static final NullVibrator sInstance = new NullVibrator();

    private NullVibrator() {
    }

    public static NullVibrator getInstance() {
        return sInstance;
    }

    @Override
    public boolean hasVibrator() {
        return false;
    }

    @Override
    public boolean hasAmplitudeControl() {
        return false;
    }

    @Override
    public void vibrate(int uid, String opPkg, VibrationEffect effect, String reason,
            VibrationAttributes attributes) {
    }

    @Override
    public void cancel() {
    }
}
