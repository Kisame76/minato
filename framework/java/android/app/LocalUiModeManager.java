/*
 * The UI mode service: IUiModeManager of system_server, local in the app's
 * process, which ServiceManager's table gives as "uimode" and AOSP's UiModeManager asks. It answers as Android 15's
 * UiModeManagerService does for an app on a phone [verified: system-services-test on apk-runner-35]:
 *   - the current mode type is the configuration's, a normal device's (UI_MODE_TYPE_NORMAL) where it has none;
 *   - the night mode is the one the configuration shows, which follows the host's appearance (ADR-0028 decision 9):
 *     MODE_NIGHT_YES in dark mode, MODE_NIGHT_NO otherwise, as the user's dark theme setting gives on Android;
 *   - the contrast is the standard one, 0;
 *   - a callback is kept and never called, since neither the contrast nor the night mode changes while it runs.
 * Every other call is AIDL's Default answer. Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.app;

import android.content.res.Configuration;
import android.os.Binder;
import android.os.IBinder;

import java.util.ArrayList;
import java.util.List;

public final class LocalUiModeManager extends IUiModeManager.Default {
    private final Binder mBinder = new Binder();
    /** The callbacks UiModeManager's globals add; under the service's lock. */
    private final List<IUiModeManagerCallback> mCallbacks = new ArrayList<>();

    private LocalUiModeManager() {
        mBinder.attachInterface(this, IUiModeManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "uimode": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalUiModeManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    /** The application's configuration's uiMode, which the session's configuration sets; 0 before there is one. */
    private static int uiMode() {
        final Application application = ActivityThread.currentApplication();
        return application == null ? 0 : application.getResources().getConfiguration().uiMode;
    }

    @Override
    public synchronized void addCallback(IUiModeManagerCallback callback) {
        mCallbacks.add(callback);
    }

    @Override
    public int getCurrentModeType() {
        final int type = uiMode() & Configuration.UI_MODE_TYPE_MASK;
        return type == Configuration.UI_MODE_TYPE_UNDEFINED ? Configuration.UI_MODE_TYPE_NORMAL : type;
    }

    @Override
    public int getNightMode() {
        return (uiMode() & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
                ? UiModeManager.MODE_NIGHT_YES : UiModeManager.MODE_NIGHT_NO;
    }

    @Override
    public float getContrast() {
        return UiModeManager.ContrastUtils.CONTRAST_DEFAULT_VALUE;
    }
}
