/*
 * The vibrator manager service: IVibratorManagerService of system_server,
 * local in the app's process, which ServiceManager's table gives as "vibrator_manager" and AOSP's SystemVibratorManager
 * asks, for SystemVibrator, View's haptic feedback and VibratorManager. It has no vibrator, as Android 15's service on
 * a device without one: no vibrator IDs, so hasVibrator() answers false and a vibration does nothing, after the
 * permission checks that Android's service makes (VIBRATE for a vibration and its cancel, VIBRATE_ALWAYS_ON for an
 * always-on effect, ACCESS_VIBRATOR_STATE for a vibrator's state, with PermissionEnforcer's message). A haptic feedback
 * does nothing. The iPhone's haptics come later, as a host event. It is AIDL's Default class, as the other local
 * services are.
 */
package android.os;

import android.Manifest;
import android.app.ActivityThread;
import android.content.Context;
import android.content.pm.PackageManager;

public final class VibratorManagerService extends IVibratorManagerService.Default {
    /** PermissionEnforcer's message. */
    private static final String ACCESS_DENIED = "Access denied, requires: ";

    private final Binder mBinder = new Binder();

    private VibratorManagerService() {
        mBinder.attachInterface(this, IVibratorManagerService.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "vibrator_manager": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new VibratorManagerService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public int[] getVibratorIds() {
        return new int[0];
    }

    @Override
    public VibratorInfo getVibratorInfo(int vibratorId) {
        return null;
    }

    @Override
    public boolean isVibrating(int vibratorId) {
        enforce(Manifest.permission.ACCESS_VIBRATOR_STATE);
        return false;
    }

    @Override
    public boolean registerVibratorStateListener(int vibratorId, IVibratorStateListener listener) {
        enforce(Manifest.permission.ACCESS_VIBRATOR_STATE);
        return false;
    }

    @Override
    public boolean unregisterVibratorStateListener(int vibratorId, IVibratorStateListener listener) {
        enforce(Manifest.permission.ACCESS_VIBRATOR_STATE);
        return false;
    }

    @Override
    public boolean setAlwaysOnEffect(int uid, String opPkg, int alwaysOnId, CombinedVibration vibration,
            VibrationAttributes attributes) {
        context().enforceCallingOrSelfPermission(Manifest.permission.VIBRATE_ALWAYS_ON, "setAlwaysOnEffect");
        return false;
    }

    @Override
    public void vibrate(int uid, int deviceId, String opPkg, CombinedVibration vibration,
            VibrationAttributes attributes, String reason, IBinder token) {
        context().enforceCallingOrSelfPermission(Manifest.permission.VIBRATE, "vibrate");
    }

    @Override
    public void cancelVibrate(int usageFilter, IBinder token) {
        context().enforceCallingOrSelfPermission(Manifest.permission.VIBRATE, "cancelVibrate");
    }

    @Override
    public void performHapticFeedback(int uid, int deviceId, String opPkg, int constant, boolean always,
            String reason, boolean fromIme) {
    }

    private static Context context() {
        return ActivityThread.currentApplication();
    }

    private static void enforce(String permission) {
        Context context = context();
        if (context == null || context.checkCallingOrSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(ACCESS_DENIED + permission);
        }
    }
}
