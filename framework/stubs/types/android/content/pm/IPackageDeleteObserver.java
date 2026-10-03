package android.content.pm;

import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: the AIDL observer of PackageManager's deletions, which
 * its LegacyPackageDeleteObserver calls.
 */
public interface IPackageDeleteObserver {
    void packageDeleted(String packageName, int returnCode) throws RemoteException;
}
