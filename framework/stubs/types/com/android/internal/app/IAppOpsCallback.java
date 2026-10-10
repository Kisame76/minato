package com.android.internal.app;

/**
 * Compile-only stub (the Unity test APK probe's batch C), never in framework.jar: the app ops service's callback, which the
 * legacy Camera's constructor implements for the shutter sound's audio operation, and Minato has no camera to open.
 */
public interface IAppOpsCallback extends android.os.IInterface {
    void opChanged(int op, int uid, String packageName, String persistentDeviceId);

    abstract class Stub extends android.os.Binder implements IAppOpsCallback {
        @Override
        public android.os.IBinder asBinder() {
            throw new RuntimeException("stub");
        }
    }
}
