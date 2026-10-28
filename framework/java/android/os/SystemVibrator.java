/*
 * The vibrator a context gives (gap 1 of the compatibility plan, batch 3), under AOSP's name: our own, since AOSP's
 * needs the vibrator HAL's AIDL, with the answers AOSP's gives on a device without a vibrator, whose info is
 * VibratorInfo.EMPTY_VIBRATOR_INFO: no vibrator, no amplitude control, the ID -1, effects of unknown support, no
 * primitives and unknown frequencies. A vibration goes to the context's vibrator manager as a parallel combination
 * and its cancel too, so that the service's permission checks are Android's.
 */
package android.os;

import android.content.Context;
import android.util.Log;

/** @hide */
public class SystemVibrator extends Vibrator {
    private static final String TAG = "Vibrator";

    private final VibratorManager mVibratorManager;

    public SystemVibrator(Context context) {
        mVibratorManager = context.getSystemService(VibratorManager.class);
    }

    @Override
    public boolean hasVibrator() {
        return mVibratorManager != null && mVibratorManager.getVibratorIds().length > 0;
    }

    @Override
    public boolean hasAmplitudeControl() {
        return false;
    }

    @Override
    public int getId() {
        return -1;
    }

    @Override
    public int[] areEffectsSupported(int... effectIds) {
        final int[] supported = new int[effectIds.length];
        java.util.Arrays.fill(supported, VIBRATION_EFFECT_SUPPORT_UNKNOWN);
        return supported;
    }

    @Override
    public boolean[] arePrimitivesSupported(int... primitiveIds) {
        return new boolean[primitiveIds.length];
    }

    @Override
    public int[] getPrimitiveDurations(int... primitiveIds) {
        return new int[primitiveIds.length];
    }

    @Override
    public float getResonantFrequency() {
        return Float.NaN;
    }

    @Override
    public float getQFactor() {
        return Float.NaN;
    }

    @Override
    public void vibrate(int uid, String opPkg, VibrationEffect effect, String reason,
            VibrationAttributes attributes) {
        if (mVibratorManager == null) {
            Log.w(TAG, "Failed to vibrate; no vibrator manager.");
            return;
        }
        mVibratorManager.vibrate(uid, opPkg, CombinedVibration.createParallel(effect), reason, attributes);
    }

    @Override
    public void cancel() {
        if (mVibratorManager == null) {
            Log.w(TAG, "Failed to cancel vibrate; no vibrator manager.");
            return;
        }
        mVibratorManager.cancel();
    }
}
