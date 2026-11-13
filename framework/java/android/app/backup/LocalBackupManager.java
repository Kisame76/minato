/*
 * The backup service: IBackupManager of system_server, local in the app's
 * process, which ServiceManager's table gives as "backup" and AOSP's BackupManager asks. Minato has no backup
 * transport and no restore: an app's data changed notifications are taken and dropped, which is what the emulator
 * (apk-runner-35) does to an app that has no transport to hand them to [verified: batch-g-test: dataChanged() ends
 * without a result, requestRestore(null) gives -1 as no restore session begins]. isBackupEnabled needs the BACKUP
 * permission, which an app does not hold, and so is a SecurityException, as there [verified: batch-g-test]. Every other
 * call is AIDL's Default answer (no session, false, 0, null).
 */
package android.app.backup;

import android.os.Binder;
import android.os.IBinder;
import android.os.Process;

public final class LocalBackupManager extends IBackupManager.Default {
    private final Binder mBinder = new Binder();

    private LocalBackupManager() {
        mBinder.attachInterface(this, IBackupManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "backup": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalBackupManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    private static SecurityException needsBackup() {
        return new SecurityException("Neither user " + Process.myUid()
                + " nor current process has android.permission.BACKUP.");
    }

    @Override
    public boolean isBackupEnabled() {
        throw needsBackup();
    }

    @Override
    public boolean isBackupEnabledForUser(int userId) {
        throw needsBackup();
    }
}
