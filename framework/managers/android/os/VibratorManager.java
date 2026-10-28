// The written part of VibratorManager, which tools/managergen joins with the API's methods (gap 1 of the compatibility
// plan, batch 3): AOSP's public vibrate methods down to the hidden one that SystemVibratorManager implements, and the
// hidden constructor that it and an input device's vibrator manager call. The attributes stay as given, as in
// Vibrator's written part.
import android.app.ActivityThread;

class VibratorManager {
    /** @hide */
    public VibratorManager() {
    }

    public final void vibrate(CombinedVibration effect) {
        vibrate(effect, null);
    }

    public final void vibrate(CombinedVibration effect, VibrationAttributes attributes) {
        vibrate(Process.myUid(), ActivityThread.currentPackageName(), effect, null, attributes);
    }

    /** @hide */
    public void vibrate(int uid, String opPkg, CombinedVibration effect, String reason, VibrationAttributes attributes) {
    }
}
