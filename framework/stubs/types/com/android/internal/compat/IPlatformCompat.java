package com.android.internal.compat;

import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0043 test 2), never in framework.jar: CompatChanges and its ChangeIdStateCache ask the
 * system's compat service through it about another package or uid, a service that no app process of Minato has.
 */
public interface IPlatformCompat {
    boolean isChangeEnabledByPackageName(long changeId, String packageName, int userId) throws RemoteException;

    boolean isChangeEnabledByUid(long changeId, int uid) throws RemoteException;

    void putAllOverridesOnReleaseBuilds(CompatibilityOverridesByPackageConfig overridesByPackage)
            throws RemoteException;

    void putOverridesOnReleaseBuilds(CompatibilityOverrideConfig overrides, String packageName)
            throws RemoteException;

    void removeAllOverridesOnReleaseBuilds(CompatibilityOverridesToRemoveByPackageConfig overridesToRemoveByPackage)
            throws RemoteException;

    void removeOverridesOnReleaseBuilds(CompatibilityOverridesToRemoveConfig overridesToRemove, String packageName)
            throws RemoteException;

    abstract class Stub {
        public static IPlatformCompat asInterface(IBinder binder) {
            throw new RuntimeException("stub");
        }
    }
}
