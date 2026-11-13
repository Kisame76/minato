/*
 * The power service (ADR-0045 decisions 3 and 4): IPowerManager of
 * system_server, local in the app's process, which ServiceManager's table gives as "power" and PowerManager asks. It
 * answers as Android 15's does for an app on a phone that is on, not saving power and not idle [verified:
 * services-test on apk-runner-35-bare]:
 *   - a wake lock needs WAKE_LOCK, as PowerManagerService.acquireWakeLock and releaseWakeLock enforce it, and is then
 *     accepted and kept by its token; nothing else follows yet: screen wake locks keep the screen on with step c
 *     (ADR-0028's KEEP_SCREEN_ON), and a partial one changes nothing, as iOS suspends a backgrounded app anyway;
 *   - the levels PowerManagerService supports, without the proximity sensor's, as no sensor is there;
 *   - interactive, battery saver off (PowerSaveState for every service type), not idle, not boosted.
 * It is AIDL's Default class, as the display service is: IPowerManager names BatterySaverPolicyConfig and
 * ParcelDuration, which framework.jar lacks. Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.os;

import android.Manifest;
import android.app.ActivityThread;
import android.app.Application;
import android.util.ArraySet;

public final class PowerService extends IPowerManager.Default {
    private final Binder mBinder = new Binder();
    /** The wake locks held, by their token. */
    private final ArraySet<IBinder> mHeld = new ArraySet<>();

    private PowerService() {
        mBinder.attachInterface(this, IPowerManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "power": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new PowerService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public void acquireWakeLock(IBinder lock, int flags, String tag, String packageName, WorkSource ws,
            String historyTag, int displayId, IWakeLockCallback callback) {
        enforceWakeLock();
        synchronized (mHeld) {
            mHeld.add(lock);
        }
    }

    @Override
    public void acquireWakeLockAsync(IBinder lock, int flags, String tag, String packageName, WorkSource ws,
            String historyTag) {
        enforceWakeLock();
        synchronized (mHeld) {
            mHeld.add(lock);
        }
    }

    @Override
    public void releaseWakeLock(IBinder lock, int flags) {
        enforceWakeLock();
        synchronized (mHeld) {
            mHeld.remove(lock);
        }
    }

    @Override
    public void releaseWakeLockAsync(IBinder lock, int flags) {
        enforceWakeLock();
        synchronized (mHeld) {
            mHeld.remove(lock);
        }
    }

    @Override
    @SuppressWarnings("deprecation") // The screen's levels, deprecated for apps, which PowerManagerService supports.
    public boolean isWakeLockLevelSupported(int level) {
        switch (level) {
            case PowerManager.PARTIAL_WAKE_LOCK:
            case PowerManager.SCREEN_DIM_WAKE_LOCK:
            case PowerManager.SCREEN_BRIGHT_WAKE_LOCK:
            case PowerManager.FULL_WAKE_LOCK:
            case PowerManager.DOZE_WAKE_LOCK:
            case PowerManager.DRAW_WAKE_LOCK:
                return true;
            default:
                return false;
        }
    }

    @Override
    public boolean isInteractive() {
        return true;
    }

    @Override
    public boolean isDisplayInteractive(int displayId) {
        return true;
    }

    @Override
    public PowerSaveState getPowerSaveState(int serviceType) {
        return new PowerSaveState.Builder().setBatterySaverEnabled(false).setGlobalBatterySaverEnabled(false)
                .build();
    }

    /** As PowerManagerService: the caller must hold WAKE_LOCK, with Context's message, which names its uid. */
    private static void enforceWakeLock() {
        Application app = ActivityThread.currentApplication();
        if (app != null) {
            app.enforceCallingOrSelfPermission(Manifest.permission.WAKE_LOCK, null);
        }
    }
}
