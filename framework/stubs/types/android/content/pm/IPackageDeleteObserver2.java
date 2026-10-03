package android.content.pm;

import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: the AIDL observer that PackageManager's
 * UninstallCompleteCallback reads from a parcel.
 */
public interface IPackageDeleteObserver2 extends IInterface {
    void onPackageDeleted(String packageName, int returnCode, String msg) throws RemoteException;

    abstract class Stub {
        public static IPackageDeleteObserver2 asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
