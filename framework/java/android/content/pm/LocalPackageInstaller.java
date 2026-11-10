/*
 * The package installer service of Minato's one app process (the Unity test APK resolve list, batch E): what system_server's
 * PackageInstallerService answers an app without the permission to install packages, answered in the process. The
 * app's sessions live here, in memory and for the process: createSession, getSessionInfo, getAllSessions,
 * getMySessions, abandonSession and openSession, with the files of a session below the app's cache folder. A session
 * is the app's own: no other session exists in Minato, so getAllSessions and getMySessions are alike.
 * Departures, named: commit installs nothing and answers nothing (an IntentSender needs a PendingIntent, ADR-0045
 * step d3), session callbacks are accepted and never fire, and the installs, uninstalls and archives other than
 * sessions are not written (each logs once, as the managers' generated methods do).
 */
package android.content.pm;

import android.app.ActivityThread;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.PackageInstaller.SessionInfo;
import android.content.pm.PackageInstaller.SessionParams;
import android.net.Uri;
import android.os.Parcel;
import android.util.Log;
import com.android.internal.minato.install.UninstallActivity;
import android.util.SparseArray;
import com.android.minato.services.Unwritten;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;

public final class LocalPackageInstaller extends IPackageInstaller.Default {
    private final String mPackageName;
    private final File mStaging;
    private final int mUserId;
    private final Object mLock = new Object();
    private final SparseArray<LocalPackageInstallerSession> mSessions = new SparseArray<>();
    private final Random mRandom = new Random();
    private static volatile LocalPackageInstaller sInstance;

    /**
     * @param packageName the app's package, the installer of its sessions
     * @param staging     the folder below which each session has a folder of its own
     */
    public LocalPackageInstaller(String packageName, File staging, int userId) {
        mPackageName = packageName;
        mStaging = staging;
        mUserId = userId;
        sInstance = this;
    }

    /** The app's session by its ID, for SessionInfo.getAppIcon (framework/patches' packageinstaller patch). */
    static SessionInfo sessionInfo(int sessionId) {
        LocalPackageInstaller installer = sInstance;
        return installer != null ? installer.getSessionInfo(sessionId) : null;
    }

    @Override
    public int createSession(SessionParams params, String installerPackageName, String installerAttributionTag,
            int userId) {
        if (params.mode != SessionParams.MODE_FULL_INSTALL && params.mode != SessionParams.MODE_INHERIT_EXISTING) {
            throw new IllegalArgumentException("Invalid install mode: " + params.mode);
        }
        synchronized (mLock) {
            int id;
            do {
                id = mRandom.nextInt(Integer.MAX_VALUE - 1) + 1;
            } while (mSessions.indexOfKey(id) >= 0);
            SessionInfo info = new SessionInfo();
            info.sessionId = id;
            info.userId = userId;
            info.installerPackageName = installerPackageName;
            info.installerAttributionTag = installerAttributionTag;
            info.mode = params.mode;
            info.sizeBytes = params.sizeBytes;
            info.appPackageName = params.appPackageName;
            info.appIcon = params.appIcon;
            info.appLabel = params.appLabel;
            info.installLocation = params.installLocation;
            info.installFlags = params.installFlags;
            info.requireUserAction = params.requireUserAction;
            info.createdMillis = System.currentTimeMillis();
            mSessions.put(id, new LocalPackageInstallerSession(this, info, new File(mStaging, "session-" + id)));
            return id;
        }
    }

    /** The APKs of the app's session `sessionId`, base first, for the activity that has the host confirm them. */
    public static java.util.List<java.io.File> sessionApks(int sessionId) {
        final LocalPackageInstaller installer = sInstance;
        if (installer == null) {
            return java.util.Collections.emptyList();
        }
        final LocalPackageInstallerSession session;
        synchronized (installer.mLock) {
            session = installer.mSessions.get(sessionId);
        }
        return session != null ? session.stagedApks() : java.util.Collections.emptyList();
    }

    /**
     * The user's answer for the session (0 installed, 1 failed, 2 cancelled) and, for a failure, why (0 none,
     * 1 invalid, 2 conflict, 3 downgrade, 4 storage, 5 incompatible), which send its final status.
     */
    public static void sessionResult(int sessionId, int result, int reason) {
        final LocalPackageInstaller installer = sInstance;
        final LocalPackageInstallerSession session;
        if (installer == null) {
            return;
        }
        synchronized (installer.mLock) {
            session = installer.mSessions.get(sessionId);
        }
        if (session != null) {
            session.installed(result, reason);
        }
    }

    /** The status receivers of PackageInstaller.uninstall that wait for the user, by the ID in the confirm intent. */
    private static final SparseArray<IntentSender> sUninstalls = new SparseArray<>();
    private static int sNextUninstall = 1;

    /**
     * PackageInstaller.uninstall as an app without the right to delete packages gets it: a SecurityException without
     * REQUEST_DELETE_PACKAGES; otherwise STATUS_PENDING_USER_ACTION with the intent that asks the user
     * (ACTION_UNINSTALL_PACKAGE, answered by UninstallActivity), and the final status once it ends.
     */
    @Override
    @SuppressWarnings("deprecation") // ACTION_UNINSTALL_PACKAGE is the action Android's own status intent carries
    public void uninstall(VersionedPackage versionedPackage, String callerPackageName, int flags,
            IntentSender statusReceiver, int userId) {
        if (!ActivityThread.permissionGranted("android.permission.REQUEST_DELETE_PACKAGES")) {
            throw new SecurityException("Neither user " + android.os.Process.myUid() + " nor current process has "
                    + "android.permission.DELETE_PACKAGES.");
        }
        final int id;
        synchronized (sUninstalls) {
            id = sNextUninstall++;
            sUninstalls.put(id, statusReceiver);
        }
        final Intent ask = new Intent(Intent.ACTION_UNINSTALL_PACKAGE,
                Uri.fromParts("package", versionedPackage.getPackageName(), null))
                .putExtra(Intent.EXTRA_RETURN_RESULT, true)
                .putExtra(UninstallActivity.EXTRA_CALLBACK_ID, id);
        sendUninstallStatus(statusReceiver, PackageInstaller.STATUS_PENDING_USER_ACTION, "Pending user action",
                versionedPackage.getPackageName(), ask);
    }

    /** The user's answer for the uninstall `id` (0 removed, 1 failed, 2 cancelled): its final status. */
    public static void uninstallResult(int id, String packageName, int result) {
        final IntentSender receiver;
        synchronized (sUninstalls) {
            receiver = sUninstalls.get(id);
            sUninstalls.remove(id);
        }
        if (receiver != null) {
            sendUninstallStatus(receiver, result == 0 ? PackageInstaller.STATUS_SUCCESS
                    : result == 2 ? PackageInstaller.STATUS_FAILURE_ABORTED : PackageInstaller.STATUS_FAILURE,
                    result == 0 ? "Succeeded" : result == 2 ? "Uninstall canceled by user" : "Failed",
                    packageName, null);
        }
    }

    private static void sendUninstallStatus(IntentSender receiver, int status, String message, String packageName,
            Intent ask) {
        final Intent fill = new Intent()
                .putExtra(PackageInstaller.EXTRA_STATUS, status)
                .putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE, message)
                .putExtra(PackageInstaller.EXTRA_PACKAGE_NAME, packageName);
        if (ask != null) {
            fill.putExtra(Intent.EXTRA_INTENT, ask);
        }
        try {
            receiver.sendIntent(ActivityThread.currentApplication(), 0, fill, null, null);
        } catch (IntentSender.SendIntentException e) {
            Log.w("PackageInstaller", "The status receiver of an uninstall is gone: " + e);
        }
    }

    /** A session's own folder and info are gone. */
    void forget(int sessionId) {
        synchronized (mLock) {
            mSessions.remove(sessionId);
        }
    }

    @Override
    public void abandonSession(int sessionId) {
        LocalPackageInstallerSession session;
        synchronized (mLock) {
            session = mSessions.get(sessionId);
        }
        if (session == null) {
            throw new SecurityException("Caller has no access to session " + sessionId);
        }
        session.abandon();
    }

    @Override
    public IPackageInstallerSession openSession(int sessionId) {
        synchronized (mLock) {
            LocalPackageInstallerSession session = mSessions.get(sessionId);
            if (session == null) {
                throw new SecurityException("Caller has no access to session " + sessionId);
            }
            session.open();
            return session;
        }
    }

    @Override
    public SessionInfo getSessionInfo(int sessionId) {
        synchronized (mLock) {
            LocalPackageInstallerSession session = mSessions.get(sessionId);
            return session != null ? session.info() : null;
        }
    }

    @Override
    @SuppressWarnings("rawtypes")
    public ParceledListSlice getAllSessions(int userId) {
        return slice();
    }

    @Override
    @SuppressWarnings("rawtypes")
    public ParceledListSlice getMySessions(String installerPackageName, int userId) {
        return slice();
    }

    @Override
    @SuppressWarnings("rawtypes")
    public ParceledListSlice getStagedSessions() {
        return new ParceledListSlice<SessionInfo>(new ArrayList<>());
    }

    private ParceledListSlice<SessionInfo> slice() {
        List<SessionInfo> infos = new ArrayList<>();
        synchronized (mLock) {
            for (int i = 0; i < mSessions.size(); i++) {
                infos.add(mSessions.valueAt(i).info());
            }
        }
        return new ParceledListSlice<>(infos);
    }

    /** A copy through a parcel, as the caller of a binder gets it: the session goes on changing. */
    static SessionInfo copy(SessionInfo info) {
        Parcel parcel = Parcel.obtain();
        try {
            info.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return SessionInfo.CREATOR.createFromParcel(parcel);
        } finally {
            parcel.recycle();
        }
    }

    @Override
    public void registerCallback(IPackageInstallerCallback callback, int userId) {
        Unwritten.log("android.content.pm.IPackageInstaller.registerCallback (ADR-0045 step d3: callbacks never fire)",
                null);
    }

    @Override
    public void unregisterCallback(IPackageInstallerCallback callback) {
    }
}
