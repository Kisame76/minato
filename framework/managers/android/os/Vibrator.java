// The written part of Vibrator, which tools/managergen joins with the API's methods (gap 1 of the compatibility plan,
// batch 3): AOSP's chain of the public vibrate methods down to the hidden one that a vibrator implements, as
// SystemVibrator and NullVibrator do, the hidden constructor that subclasses call, and the haptic feedback View asks
// for. The attributes stay as given: VibrationAttributes is a generated manager whose builder answers null, and Minato's
// vibrator service plays nothing yet. AOSP's Vibrator needs the vibrator HAL's AIDL, which the sources lack.
import android.app.ActivityThread;
import android.media.AudioAttributes;
import android.util.Log;

class Vibrator {
    private static final String TAG = "Vibrator";

    /** @hide */
    protected Vibrator() {
    }

    public void vibrate(long milliseconds) {
        vibrate(milliseconds, null);
    }

    public void vibrate(long milliseconds, AudioAttributes attributes) {
        try {
            // This ignores all exceptions to stay compatible with pre-O implementations.
            vibrate(VibrationEffect.createOneShot(milliseconds, VibrationEffect.DEFAULT_AMPLITUDE), attributes);
        } catch (IllegalArgumentException iae) {
            Log.e(TAG, "Failed to create VibrationEffect", iae);
        }
    }

    public void vibrate(long[] pattern, int repeat) {
        vibrate(pattern, repeat, null);
    }

    public void vibrate(long[] pattern, int repeat, AudioAttributes attributes) {
        // This call needs to continue throwing ArrayIndexOutOfBoundsException but ignore all other
        // exceptions for compatibility purposes
        if (repeat < -1 || repeat >= pattern.length) {
            Log.e(TAG, "vibrate called with repeat index out of bounds" +
                    " (pattern.length=" + pattern.length + ", index=" + repeat + ")");
            throw new ArrayIndexOutOfBoundsException();
        }
        try {
            vibrate(VibrationEffect.createWaveform(pattern, repeat), attributes);
        } catch (IllegalArgumentException iae) {
            Log.e(TAG, "Failed to create VibrationEffect", iae);
        }
    }

    public void vibrate(VibrationEffect vibe) {
        vibrate(Process.myUid(), ActivityThread.currentPackageName(), vibe, null, null);
    }

    public void vibrate(VibrationEffect vibe, AudioAttributes attributes) {
        vibrate(Process.myUid(), ActivityThread.currentPackageName(), vibe, null, null);
    }

    public void vibrate(VibrationEffect vibe, VibrationAttributes attributes) {
        vibrate(Process.myUid(), ActivityThread.currentPackageName(), vibe, null, attributes);
    }

    /** @hide */
    public void vibrate(int uid, String opPkg, VibrationEffect vibe, String reason, VibrationAttributes attributes) {
    }

    /** @hide */
    public void performHapticFeedback(int constant, boolean always, String reason, boolean fromIme) {
    }
}
