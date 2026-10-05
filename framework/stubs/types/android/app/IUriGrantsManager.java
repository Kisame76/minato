package android.app;

import android.content.pm.ParceledListSlice;
import android.net.Uri;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0045, gap 13a), never in framework.jar: the URI grants service's AIDL interface, with the
 * three methods ContentResolver's persisted URI permissions call.
 */
public interface IUriGrantsManager {
    void takePersistableUriPermission(Uri uri, int modeFlags, String toPackage, int userId) throws RemoteException;

    void releasePersistableUriPermission(Uri uri, int modeFlags, String toPackage, int userId) throws RemoteException;

    ParceledListSlice getUriPermissions(String packageName, boolean incoming, boolean persistedOnly)
            throws RemoteException;
}
