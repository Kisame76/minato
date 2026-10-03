package android.content.pm;

import android.os.IBinder;
import android.os.RemoteException;
import java.util.List;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: the package service's AIDL interface, with the methods
 * PackageManager's static helpers call through ActivityThread.getPackageManager(). Minato's PackageManager is our
 * ApplicationPackageManager on the app's manifest (decision 7), so no IPackageManager answers before step c.
 */
public interface IPackageManager {
    PackageInfo getPackageInfo(String packageName, long flags, int userId) throws RemoteException;

    ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) throws RemoteException;

    void makeProviderVisible(int recipientAppId, String visibleAuthority) throws RemoteException;

    IBinder getHoldLockToken() throws RemoteException;

    void holdLock(IBinder token, int durationMs) throws RemoteException;

    void setKeepUninstalledPackages(List<String> packageList) throws RemoteException;
}
