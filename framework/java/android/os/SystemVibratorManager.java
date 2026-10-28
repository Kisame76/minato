/*
 * The vibrator manager a context gives (gap 1 of the compatibility plan, batch 3), under AOSP's name: our own, since
 * AOSP's needs the vibrator HAL's AIDL, but with its answers, over the local vibrator manager service
 * (VibratorManagerService), which has no vibrator: no IDs, NullVibrator for any ID, the context's vibrator as the
 * default one, and vibrations and cancels that the service checks and drops.
 */
package android.os;

import android.content.Context;
import android.util.Log;

/** @hide */
public class SystemVibratorManager extends VibratorManager {
    private static final String TAG = "VibratorManager";
    /** VibrationAttributes.USAGE_FILTER_MATCH_ALL, a hidden constant the generated class lacks. */
    private static final int USAGE_FILTER_MATCH_ALL = -1;

    private final Context mContext;
    private final IVibratorManagerService mService;
    private final Binder mToken = new Binder();
    private int[] mVibratorIds;

    public SystemVibratorManager(Context context) {
        mContext = context;
        mService = IVibratorManagerService.Stub.asInterface(
                ServiceManager.getService(Context.VIBRATOR_MANAGER_SERVICE));
    }

    @Override
    public synchronized int[] getVibratorIds() {
        if (mVibratorIds != null) {
            return mVibratorIds;
        }
        try {
            if (mService == null) {
                Log.w(TAG, "Failed to retrieve vibrator ids; no vibrator manager service.");
            } else {
                return mVibratorIds = mService.getVibratorIds();
            }
        } catch (RemoteException e) {
            e.rethrowFromSystemServer();
        }
        return new int[0];
    }

    @Override
    public Vibrator getVibrator(int vibratorId) {
        return NullVibrator.getInstance();
    }

    @Override
    public Vibrator getDefaultVibrator() {
        return mContext.getSystemService(Vibrator.class);
    }

    @Override
    public void vibrate(int uid, String opPkg, CombinedVibration effect, String reason,
            VibrationAttributes attributes) {
        if (mService == null) {
            Log.w(TAG, "Failed to vibrate; no vibrator manager service.");
            return;
        }
        try {
            mService.vibrate(uid, Context.DEVICE_ID_DEFAULT, opPkg, effect, attributes, reason, mToken);
        } catch (RemoteException e) {
            Log.w(TAG, "Failed to vibrate.", e);
        }
    }

    @Override
    public void cancel() {
        if (mService == null) {
            Log.w(TAG, "Failed to cancel vibration; no vibrator manager service.");
            return;
        }
        try {
            mService.cancelVibrate(USAGE_FILTER_MATCH_ALL, mToken);
        } catch (RemoteException e) {
            Log.w(TAG, "Failed to cancel vibration.", e);
        }
    }
}
