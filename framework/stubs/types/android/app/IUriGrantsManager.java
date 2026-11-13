package android.app;

import android.content.pm.ParceledListSlice;
import android.net.Uri;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the URI grants service's AIDL interface, with the
 * three methods ContentResolver's persisted URI permissions call, and the check Icon's loadDrawable makes for a URI of
 * another app.
 */
public interface IUriGrantsManager {
    void takePersistableUriPermission(Uri uri, int modeFlags, String toPackage, int userId) throws RemoteException;

    void releasePersistableUriPermission(Uri uri, int modeFlags, String toPackage, int userId) throws RemoteException;

    ParceledListSlice getUriPermissions(String packageName, boolean incoming, boolean persistedOnly)
            throws RemoteException;

    int checkGrantUriPermission_ignoreNonSystem(int sourceUid, String targetPkg, Uri uri, int modeFlags, int userId)
            throws RemoteException;
}
