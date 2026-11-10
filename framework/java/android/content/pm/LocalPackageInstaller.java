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

import android.content.pm.PackageInstaller.SessionInfo;
import android.content.pm.PackageInstaller.SessionParams;
import android.os.Parcel;
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

    /** The APK of the app's session `sessionId`, for the activity that has the host confirm it; null if none. */
    public static java.io.File sessionApk(int sessionId) {
        final LocalPackageInstaller installer = sInstance;
        if (installer == null) {
            return null;
        }
        final LocalPackageInstallerSession session;
        synchronized (installer.mLock) {
            session = installer.mSessions.get(sessionId);
        }
        return session != null ? session.stagedApk() : null;
    }

    /** The user's answer for the session (0 installed, 1 failed, 2 cancelled), which sends its final status. */
    public static void sessionResult(int sessionId, int result) {
        final LocalPackageInstaller installer = sInstance;
        final LocalPackageInstallerSession session;
        if (installer == null) {
            return;
        }
        synchronized (installer.mLock) {
            session = installer.mSessions.get(sessionId);
        }
        if (session != null) {
            session.installed(result);
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
