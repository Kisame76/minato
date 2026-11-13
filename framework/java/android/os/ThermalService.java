/*
 * The thermal service (ADR-0045 decisions 3 and 4): IThermalService of
 * system_server, local in the app's process, which ServiceManager's table gives as "thermalservice" and PowerManager
 * asks. Until step c gives iOS's thermal state (ProcessInfo.thermalState), the status is THERMAL_STATUS_NONE, as on a
 * cool device [verified: services-test on apk-runner-35-bare]. A status listener is kept and told the status at once,
 * as ThermalManagerService's registerThermalStatusListener posts it; the headroom is NaN, as on a device whose
 * thermal HAL gives none. It is AIDL's Default class: IThermalService names Temperature and CoolingDevice, which
 * framework.jar lacks.
 */
package android.os;

import android.util.Log;

import java.util.ArrayList;

public final class ThermalService extends IThermalService.Default {
    private static final String TAG = "ThermalService";

    private final Binder mBinder = new Binder();
    private final ArrayList<IThermalStatusListener> mStatusListeners = new ArrayList<>();

    private ThermalService() {
        mBinder.attachInterface(this, IThermalService.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "thermalservice": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new ThermalService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public int getCurrentThermalStatus() {
        return PowerManager.THERMAL_STATUS_NONE;
    }

    @Override
    public boolean registerThermalStatusListener(IThermalStatusListener listener) {
        synchronized (mStatusListeners) {
            if (mStatusListeners.contains(listener)) {
                return false;
            }
            mStatusListeners.add(listener);
        }
        try {
            listener.onStatusChange(getCurrentThermalStatus());
        } catch (RemoteException e) {
            Log.e(TAG, "A thermal status listener failed", e);
        }
        return true;
    }

    @Override
    public boolean unregisterThermalStatusListener(IThermalStatusListener listener) {
        synchronized (mStatusListeners) {
            return mStatusListeners.remove(listener);
        }
    }

    @Override
    public float getThermalHeadroom(int forecastSeconds) {
        return Float.NaN;
    }
}
