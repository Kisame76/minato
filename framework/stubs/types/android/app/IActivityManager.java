package android.app;

import android.os.IBinder;
import android.os.RemoteException;
import android.os.StrictMode;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: the activity manager's AIDL interface,
 * which StrictMode asks to dropbox a violation and to register its unsafe intent callback; ADR-0045's local services
 * answer it from step c.
 */
public interface IActivityManager {
    void handleApplicationStrictModeViolation(IBinder app, int penaltyMask, StrictMode.ViolationInfo crashInfo)
            throws RemoteException;

    void registerStrictModeCallback(IBinder binder) throws RemoteException;
}
