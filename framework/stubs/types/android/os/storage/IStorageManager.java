package android.os.storage;

import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: the storage service's AIDL interface,
 * which StrictMode asks whether credential-encrypted storage is unlocked for its CE storage check.
 */
public interface IStorageManager {
    boolean isCeStorageUnlocked(int userId) throws RemoteException;

    abstract class Stub {
        public static IStorageManager asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
