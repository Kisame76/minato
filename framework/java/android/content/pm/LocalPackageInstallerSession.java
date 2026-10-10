/*
 * One session of the app's package installer (LocalPackageInstaller, the Unity test APK resolve list, batch E): its files in a
 * folder of its own, written through a FileBridge as PackageInstallerSession's openWrite does, listed, read back and
 * removed. commit installs nothing and answers nothing: an IntentSender needs a PendingIntent (ADR-0045 step d3), and
 * the app cannot make one yet; the session stays as Android's stays until the user confirms the prompt. Every other
 * call of the session's interface (multi-package, staged, data loaders, checksums, pre-approval) logs once.
 */
package android.content.pm;

import android.content.pm.PackageInstaller.SessionInfo;
import android.os.FileBridge;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import com.android.minato.services.Unwritten;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class LocalPackageInstallerSession extends IPackageInstallerSession.Default {
    private final LocalPackageInstaller mInstaller;
    private final SessionInfo mInfo;
    private final File mDir;
    private final Object mLock = new Object();
    private float mClientProgress;
    private boolean mSealed;
    private boolean mDestroyed;

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
        Unwritten.log("android.content.pm.IPackageInstallerSession.commit (no in-app installs yet; ADR-0045 step d3)",
                null);
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
