/*
 * One session of the app's package installer (LocalPackageInstaller, the Unity test APK resolve list, batch E): its files in a
 * folder of its own, written through a FileBridge as PackageInstallerSession's openWrite does, listed, read back and
 * removed. commit answers the committer's IntentSender as a caller that is no privileged installer gets it: a broken
 * APK STATUS_FAILURE_INVALID at once, a valid one STATUS_PENDING_USER_ACTION with the confirm intent, which
 * InstallConfirmActivity answers by having the host install the file; its end is STATUS_SUCCESS, STATUS_FAILURE or
 * STATUS_FAILURE_ABORTED, and the session goes. Every other
 * call of the session's interface (multi-package, staged, data loaders, checksums, pre-approval) logs once.
 */
package android.content.pm;

import android.app.ActivityThread;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.PackageInstaller.SessionInfo;
import android.os.FileBridge;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import com.android.minato.services.Unwritten;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipFile;

final class LocalPackageInstallerSession extends IPackageInstallerSession.Default {
    private final LocalPackageInstaller mInstaller;
    private final SessionInfo mInfo;
    private final File mDir;
    private final Object mLock = new Object();
    private float mClientProgress;
    private boolean mSealed;
    private boolean mDestroyed;
    private IntentSender mStatusReceiver;

    LocalPackageInstallerSession(LocalPackageInstaller installer, SessionInfo info, File dir) {
        mInstaller = installer;
        mInfo = info;
        mDir = dir;
    }

    /** The session as a caller of the service gets it. */
    SessionInfo info() {
        synchronized (mLock) {
            return LocalPackageInstaller.copy(mInfo);
        }
    }

    /** openSession: the folder exists once someone opens the session. */
    void open() {
        synchronized (mLock) {
            if (!mDestroyed && !mDir.isDirectory() && !mDir.mkdirs()) {
                throw new IllegalStateException("Could not create " + mDir);
            }
        }
    }

    private void assertNotSealedLocked(String cookie) {
        if (mDestroyed) {
            throw new SecurityException("Caller has no access to session " + mInfo.sessionId);
        }
        if (mSealed) {
            throw new SecurityException(cookie + " not allowed after sealing");
        }
    }

    @Override
    public String[] getNames() {
        synchronized (mLock) {
            String[] names = mDir.list();
            return names != null ? names : new String[0];
        }
    }

    @Override
    @SuppressWarnings("deprecation") // FileBridge, which PackageInstallerSession still uses at the tag
    public ParcelFileDescriptor openWrite(String name, long offsetBytes, long lengthBytes) {
        synchronized (mLock) {
            assertNotSealedLocked("openWrite");
            if (!isValidName(name)) {
                throw new IllegalArgumentException("Invalid name: " + name);
            }
            mDir.mkdirs();
            try {
                ParcelFileDescriptor target = ParcelFileDescriptor.open(new File(mDir, name),
                        ParcelFileDescriptor.MODE_READ_WRITE | ParcelFileDescriptor.MODE_CREATE);
                if (lengthBytes > 0) {
                    Os.posix_fallocate(target.getFileDescriptor(), 0, lengthBytes);
                }
                if (offsetBytes > 0) {
                    Os.lseek(target.getFileDescriptor(), offsetBytes, OsConstants.SEEK_SET);
                }
                FileBridge bridge = new FileBridge();
                bridge.setTargetFile(target);
                bridge.start();
                return bridge.getClientSocket();
            } catch (ErrnoException | IOException e) {
                throw new IllegalStateException(e);
            }
        }
    }

    @Override
    public ParcelFileDescriptor openRead(String name) {
        synchronized (mLock) {
            if (mDestroyed) {
                throw new SecurityException("Caller has no access to session " + mInfo.sessionId);
            }
            if (!isValidName(name)) {
                throw new IllegalArgumentException("Invalid name: " + name);
            }
            try {
                return ParcelFileDescriptor.open(new File(mDir, name), ParcelFileDescriptor.MODE_READ_ONLY);
            } catch (FileNotFoundException e) {
                throw new IllegalStateException(e);
            }
        }
    }

    @Override
    public void removeSplit(String splitName) {
        synchronized (mLock) {
            assertNotSealedLocked("removeSplit");
            File marker = new File(mDir, splitName + ".removed");
            mDir.mkdirs();
            try {
                marker.createNewFile();
            } catch (IOException e) {
                throw new IllegalStateException(e);
            }
        }
    }

    @Override
    public void setClientProgress(float progress) {
        synchronized (mLock) {
            mClientProgress = progress;
            // As PackageInstallerSession's computeProgressLocked: the client's staging is 80 % of the progress.
            mInfo.progress = 0.8f * progress;
            mInfo.updatedMillis = System.currentTimeMillis();
        }
    }

    @Override
    public void addClientProgress(float progress) {
        setClientProgress(mClientProgress + progress);
    }

    @Override
    public void close() {
    }

    @Override
    public void seal() {
        synchronized (mLock) {
            mSealed = true;
            mInfo.sealed = true;
        }
    }

    @Override
    public void abandon() {
        synchronized (mLock) {
            if (mDestroyed) {
                return;
            }
            mDestroyed = true;
        }
        mInstaller.forget(mInfo.sessionId);
        deleteTree(mDir);
    }

    @Override
    public void commit(android.content.IntentSender statusReceiver, boolean forTransferred) {
        final File apk;
        synchronized (mLock) {
            if (mDestroyed) {
                throw new SecurityException("Caller has no access to session " + mInfo.sessionId);
            }
            mSealed = true;
            mInfo.sealed = true;
            mStatusReceiver = statusReceiver;
            apk = stagedApk();
        }
        // A session with several files is a split set (base.apk and split_*.apk), which the host cannot take yet: it is
        // refused with a message that names it, and logged so that the gap report shows it.
        final File[] files = mDir.listFiles(File::isFile);
        if (files != null && files.length > 1) {
            Unwritten.log("android.content.pm.IPackageInstallerSession.commit (a session with several files: a split "
                    + "set, " + files.length + " files; the host takes one APK)", null);
            report(PackageInstaller.STATUS_FAILURE_INCOMPATIBLE,
                    "Split sets are not supported yet: the session holds " + files.length + " files", null);
            return;
        }
        // As Android's session: what is not an APK is refused at once, before any question to the user.
        if (apk == null || !isApk(apk)) {
            report(PackageInstaller.STATUS_FAILURE_INVALID, "Failed to parse " + (apk != null ? apk : mDir), null);
            return;
        }
        // A caller that is not a privileged installer is asked to confirm: the status carries the activity to start.
        final Intent confirm = new Intent(PackageInstaller.ACTION_CONFIRM_INSTALL)
                .putExtra(PackageInstaller.EXTRA_SESSION_ID, mInfo.sessionId);
        report(PackageInstaller.STATUS_PENDING_USER_ACTION, "Pending user action", confirm);
    }

    /** The APK the session holds: its only file, or null when it holds none or several (a split set). */
    File stagedApk() {
        final File[] files = mDir.listFiles(File::isFile);
        return files != null && files.length == 1 ? files[0] : null;
    }

    /**
     * The user's answer, from the host: 0 installed, 1 failed, 2 cancelled. It ends the session with the status
     * Android's sender gets: STATUS_SUCCESS, STATUS_FAILURE, STATUS_FAILURE_ABORTED.
     */
    void installed(int result) {
        report(result == 0 ? PackageInstaller.STATUS_SUCCESS
                : result == 2 ? PackageInstaller.STATUS_FAILURE_ABORTED : PackageInstaller.STATUS_FAILURE,
                result == 0 ? "Installation succeeded" : result == 2 ? "Installation canceled by user"
                : "Installation failed", null);
        abandon();
    }

    private static boolean isApk(File file) {
        try (ZipFile zip = new ZipFile(file)) {
            return zip.getEntry("AndroidManifest.xml") != null;
        } catch (IOException e) {
            return false;
        }
    }

    /** Sends one status to the committer's IntentSender, with the extras Android's carries. */
    private void report(int status, String message, Intent confirm) {
        final IntentSender receiver;
        synchronized (mLock) {
            receiver = mStatusReceiver;
        }
        if (receiver == null) {
            return;
        }
        final Intent fill = new Intent()
                .putExtra(PackageInstaller.EXTRA_STATUS, status)
                .putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE, message)
                .putExtra(PackageInstaller.EXTRA_SESSION_ID, mInfo.sessionId);
        if (confirm != null) {
            fill.putExtra(Intent.EXTRA_INTENT, confirm);
        }
        try {
            receiver.sendIntent(ActivityThread.currentApplication(), 0, fill, null, null);
        } catch (IntentSender.SendIntentException e) {
            Log.w("PackageInstaller", "The status receiver of session " + mInfo.sessionId + " is gone: " + e);
        }
    }

    @Override
    public List<String> fetchPackageNames() {
        return new ArrayList<>(Arrays.asList(getNames()));
    }

    @Override
    public boolean isMultiPackage() {
        return false;
    }

    @Override
    public int[] getChildSessionIds() {
        return new int[0];
    }

    @Override
    public int getParentSessionId() {
        return -1;
    }

    @Override
    public boolean isStaged() {
        return false;
    }

    @Override
    public int getInstallFlags() {
        return mInfo.installFlags;
    }

    private static boolean isValidName(String name) {
        return name != null && !name.isEmpty() && name.indexOf('/') < 0 && !name.equals(".") && !name.equals("..");
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }
}
