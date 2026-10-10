package com.android.internal.app;

import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (the Unity test APK probe's batch C), never in framework.jar: the app ops service's AIDL interface, which
 * the legacy Camera's constructor watches for the shutter sound's audio operation, and Minato has no camera to open.
 */
public interface IAppOpsService extends android.os.IInterface {
    void startWatchingMode(int op, String packageName, IAppOpsCallback callback) throws RemoteException;

    void stopWatchingMode(IAppOpsCallback callback) throws RemoteException;

    int checkAudioOperation(int code, int usage, int uid, String packageName) throws RemoteException;

    abstract class Stub extends android.os.Binder implements IAppOpsService {
        public static IAppOpsService asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
