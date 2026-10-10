package io.github.kisame76.minato.test.packageinstaller;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * The Unity test APK's resolve list, batch E: what an app without the permission to install packages meets in
 * android.content.pm.PackageInstaller and its SessionInfo, and in android.app.ApplicationExitInfo. Its parts run one
 * after the other from the resumed activity: the installer the package manager gives, its sessions (none), the
 * refusal of a new one, the classes' public constants and parcelable creators, and the app's own exit reasons (none).
 * Each part logs with the tag package-installer-test; a part that throws logs "failed" and the next one runs, so that
 * every run reaches "done". A failure's stack has the tag package-installer-test-device.
 */
public class InstallerActivity extends Activity {
    static final String TAG = "package-installer-test";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private interface Part {
        void run() throws Exception;
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mStarted) {
            mStarted = true;
            mHandler.post(this::runParts);
        }
    }

    private void runParts() {
        List<Part> parts = Arrays.asList(this::clean, this::installer, this::sessions, this::create, this::session,
                this::invalid, this::constants, this::creators, this::exits);
        for (Part part : parts) {
            try {
                part.run();
            } catch (Exception | LinkageError e) {
                Log.i(TAG, "failed: " + e.getClass().getName() + ": " + e.getMessage());
                Log.i(TAG + "-device", Log.getStackTraceString(e));
            }
        }
        Log.i(TAG, "done");
    }

    /** The installer the package manager gives, and that it is the same for the next ask. */
    private void installer() {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        Log.i(TAG, "installer: " + (installer == null ? "null" : installer.getClass().getName()));
        Log.i(TAG, "same object: " + (installer == getPackageManager().getPackageInstaller()));
    }

    /** The sessions an app without install rights sees: none. */
    private void sessions() {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        Log.i(TAG, "all sessions: " + installer.getAllSessions().size());
        Log.i(TAG, "my sessions: " + installer.getMySessions().size());
        Log.i(TAG, "staged sessions: " + installer.getStagedSessions().size());
        Log.i(TAG, "active staged session: " + installer.getActiveStagedSession());
        Log.i(TAG, "session info 1: " + installer.getSessionInfo(1));
    }

    /**
     * Android takes an abandoned session out of its lists a moment later, on a handler of the installer service: wait
     * up to 3 s until none of the app's sessions is left.
     */
    private static boolean settled(PackageInstaller installer) throws InterruptedException {
        for (int i = 0; i < 60; i++) {
            if (installer.getMySessions().isEmpty()) return true;
            Thread.sleep(50);
        }
        return false;
    }

    /** The sessions an earlier run left in the system are abandoned first, so that every run starts alike. */
    private void clean() throws InterruptedException {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        for (PackageInstaller.SessionInfo info : installer.getMySessions()) {
            installer.abandonSession(info.getSessionId());
        }
        Log.i(TAG, "clean: " + settled(installer));
    }

    /** A new session for the app's own package: what Android gives an app without the permission to install. */
    private void create() throws Exception {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(getPackageName());
        params.setSize(1234);
        int id = installer.createSession(params);
        Log.i(TAG, "createSession: id " + (id > 0));
        PackageInstaller.SessionInfo info = installer.getSessionInfo(id);
        Log.i(TAG, "info: id " + (info.getSessionId() == id) + ", installer " + info.getInstallerPackageName()
                + ", app " + info.getAppPackageName() + ", mode " + info.getMode() + ", size " + info.getSize()
                + ", progress " + info.getProgress());
        Log.i(TAG, "info: active " + info.isActive() + ", sealed " + info.isSealed() + ", staged " + info.isStaged()
                + ", committed " + info.isCommitted() + ", multi " + info.isMultiPackage() + ", reason "
                + info.getInstallReason());
        Log.i(TAG, "info: created " + (info.getCreatedMillis() > 0) + ", updated "
                + (info.getUpdatedMillis() >= info.getCreatedMillis()) + ", user action "
                + info.getRequireUserAction() + ", source " + info.getPackageSource() + ", label " + info.getAppLabel()
                + ", icon " + info.getAppIcon());
        Log.i(TAG, "all " + installer.getAllSessions().size() + ", mine " + installer.getMySessions().size());
        installer.abandonSession(id);
        Log.i(TAG, "abandoned: gone " + settled(installer));
    }

    /** A session's files: written, listed, read back, and the refusals. */
    private void session() throws Exception {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(getPackageName());
        int id = installer.createSession(params);
        PackageInstaller.Session session = installer.openSession(id);
        Log.i(TAG, "session: names " + Arrays.toString(session.getNames()));
        byte[] bytes = new byte[100];
        for (int i = 0; i < bytes.length; i++) bytes[i] = (byte) (i * 7);
        try (OutputStream out = session.openWrite("base.apk", 0, bytes.length)) {
            out.write(bytes);
            session.fsync(out);
        }
        Log.i(TAG, "written: names " + Arrays.toString(session.getNames()));
        try (InputStream in = session.openRead("base.apk")) {
            byte[] back = new byte[200];
            int n = in.read(back);
            Log.i(TAG, "read back: " + n + " bytes, same " + (n == bytes.length
                    && Arrays.equals(bytes, Arrays.copyOf(back, n))));
        }
        try (OutputStream out = session.openWrite("split_config.apk", 0, -1)) {
            out.write(new byte[] {1, 2, 3});
        }
        Log.i(TAG, "two files: names " + sorted(session.getNames()));
        session.removeSplit("config");
        Log.i(TAG, "after removeSplit: names " + sorted(session.getNames()));
        for (String name : new String[] {"../x", "", "a/b"}) {
            try {
                session.openWrite(name, 0, 1).close();
                Log.i(TAG, "openWrite \"" + name + "\": opened");
            } catch (Exception e) {
                Log.i(TAG, "openWrite \"" + name + "\": " + e.getClass().getName() + ": " + e.getMessage());
            }
        }
        session.setStagingProgress(0.5f);
        Log.i(TAG, "staging progress: " + installer.getSessionInfo(id).getProgress());
        session.close();
        Log.i(TAG, "closed: mine " + installer.getMySessions().size());
        installer.openSession(id).abandon();
        settled(installer); // Android forgets the session a moment later: opening it before is a race
        try {
            installer.openSession(id);
        } catch (Exception e) {
            Log.i(TAG, "openSession of an abandoned one: " + e.getClass().getName() + ": "
                    + e.getMessage().replace(String.valueOf(id), "<id>"));
        }
        Log.i(TAG, "abandoned: gone " + settled(installer));
    }

    private static String sorted(String[] names) {
        String[] copy = names.clone();
        Arrays.sort(copy);
        return Arrays.toString(copy);
    }

    /** The calls that Android refuses. */
    private void invalid() throws InterruptedException {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        try {
            installer.abandonSession(987654321);
            Log.i(TAG, "abandon of an unknown session: no error");
        } catch (Exception e) {
            Log.i(TAG, "abandon of an unknown session: " + e.getClass().getName() + ": " + e.getMessage());
        }
        try {
            installer.openSession(987654321);
            Log.i(TAG, "open of an unknown session: no error");
        } catch (Exception e) {
            Log.i(TAG, "open of an unknown session: " + e.getClass().getName() + ": " + e.getMessage());
        }
        PackageInstaller.SessionParams noMode = new PackageInstaller.SessionParams(0);
        try {
            installer.createSession(noMode);
            Log.i(TAG, "createSession without a mode: no error");
        } catch (Exception e) {
            Log.i(TAG, "createSession without a mode: " + e.getClass().getName() + ": " + e.getMessage());
        }
        PackageInstaller.SessionParams inherit = new PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_INHERIT_EXISTING);
        try {
            installer.abandonSession(installer.createSession(inherit));
            Log.i(TAG, "createSession to inherit without a package: no error");
        } catch (Exception e) {
            Log.i(TAG, "createSession to inherit without a package: " + e.getClass().getName() + ": "
                    + e.getMessage());
        }
        PackageInstaller.SessionParams other = new PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        other.setAppPackageName("com.example.other");
        try {
            int id = installer.createSession(other);
            Log.i(TAG, "createSession for another package: id " + (id > 0));
            installer.abandonSession(id);
        } catch (Exception e) {
            Log.i(TAG, "createSession for another package: " + e.getClass().getName() + ": " + e.getMessage());
        }
        Log.i(TAG, "settled: " + settled(installer));
    }

    /**
     * Android 15's public SDK constants of the three classes, by name (they are those of the reference's run: a
     * reflection over getFields would also list the hidden ones, which Android's hidden API policy withholds from an
     * app and Minato does not).
     */
    private static final String[] INSTALLER_CONSTANTS = {
            "ACTION_CONFIRM_INSTALL", "ACTION_CONFIRM_PRE_APPROVAL", "ACTION_SESSION_COMMITTED",
            "ACTION_SESSION_DETAILS", "ACTION_SESSION_UPDATED", "DATA_LOADER_TYPE_INCREMENTAL",
            "DATA_LOADER_TYPE_NONE", "DATA_LOADER_TYPE_STREAMING", "EXTRA_CALLBACK", "EXTRA_DATA_LOADER_TYPE",
            "EXTRA_DELETE_FLAGS", "EXTRA_INSTALL_CONSTRAINTS", "EXTRA_INSTALL_CONSTRAINTS_RESULT",
            "EXTRA_LEGACY_STATUS", "EXTRA_OTHER_PACKAGE_NAME", "EXTRA_PACKAGE_NAME", "EXTRA_PRE_APPROVAL",
            "EXTRA_RESOLVED_BASE_PATH", "EXTRA_SESSION", "EXTRA_SESSION_ID", "EXTRA_STATUS",
            "EXTRA_STATUS_MESSAGE", "EXTRA_STORAGE_PATH", "EXTRA_UNARCHIVE_ALL_USERS", "EXTRA_UNARCHIVE_ID",
            "EXTRA_UNARCHIVE_PACKAGE_NAME", "EXTRA_UNARCHIVE_STATUS", "LOCATION_DATA_APP",
            "LOCATION_MEDIA_DATA", "LOCATION_MEDIA_OBB", "PACKAGE_SOURCE_DOWNLOADED_FILE",
            "PACKAGE_SOURCE_LOCAL_FILE", "PACKAGE_SOURCE_OTHER", "PACKAGE_SOURCE_STORE",
            "PACKAGE_SOURCE_UNSPECIFIED", "REASON_CONFIRM_PACKAGE_CHANGE", "REASON_OWNERSHIP_CHANGED",
            "REASON_REMIND_OWNERSHIP", "STATUS_FAILURE", "STATUS_FAILURE_ABORTED", "STATUS_FAILURE_BLOCKED",
            "STATUS_FAILURE_CONFLICT", "STATUS_FAILURE_INCOMPATIBLE", "STATUS_FAILURE_INVALID",
            "STATUS_FAILURE_STORAGE", "STATUS_FAILURE_TIMEOUT", "STATUS_PENDING_USER_ACTION", "STATUS_SUCCESS",
            "UNARCHIVAL_ERROR_INSTALLER_DISABLED", "UNARCHIVAL_ERROR_INSTALLER_UNINSTALLED",
            "UNARCHIVAL_ERROR_INSUFFICIENT_STORAGE", "UNARCHIVAL_ERROR_NO_CONNECTIVITY",
            "UNARCHIVAL_ERROR_USER_ACTION_NEEDED", "UNARCHIVAL_GENERIC_ERROR", "UNARCHIVAL_OK"
    };

    private static final String[] PARAMS_CONSTANTS = {
            "CONTENTS_FILE_DESCRIPTOR", "MODE_FULL_INSTALL", "MODE_INHERIT_EXISTING",
            "PARCELABLE_STABILITY_LOCAL", "PARCELABLE_STABILITY_VINTF", "PARCELABLE_WRITE_RETURN_VALUE",
            "PERMISSION_STATE_DEFAULT", "PERMISSION_STATE_DENIED", "PERMISSION_STATE_GRANTED",
            "USER_ACTION_NOT_REQUIRED", "USER_ACTION_REQUIRED", "USER_ACTION_UNSPECIFIED"
    };

    private static final String[] EXIT_CONSTANTS = {
            "CONTENTS_FILE_DESCRIPTOR", "PARCELABLE_STABILITY_LOCAL", "PARCELABLE_STABILITY_VINTF",
            "PARCELABLE_WRITE_RETURN_VALUE", "REASON_ANR", "REASON_CRASH", "REASON_CRASH_NATIVE",
            "REASON_DEPENDENCY_DIED", "REASON_EXCESSIVE_RESOURCE_USAGE", "REASON_EXIT_SELF", "REASON_FREEZER",
            "REASON_INITIALIZATION_FAILURE", "REASON_LOW_MEMORY", "REASON_OTHER", "REASON_PACKAGE_STATE_CHANGE",
            "REASON_PACKAGE_UPDATED", "REASON_PERMISSION_CHANGE", "REASON_SIGNALED", "REASON_UNKNOWN",
            "REASON_USER_REQUESTED", "REASON_USER_STOPPED"
    };

    /** The constants' names and values: their identity with Android 15's. */
    private void constants() throws Exception {
        constants(PackageInstaller.class, INSTALLER_CONSTANTS);
        constants(PackageInstaller.SessionParams.class, PARAMS_CONSTANTS);
        constants(ApplicationExitInfo.class, EXIT_CONSTANTS);
    }

    private static void constants(Class<?> type, String[] names) throws Exception {
        List<String> lines = new ArrayList<>();
        for (String name : names) {
            lines.add(name + "=" + type.getField(name).get(null));
        }
        Collections.sort(lines);
        Log.i(TAG, type.getSimpleName() + " constants: " + lines.size());
        for (String line : lines) Log.i(TAG, type.getSimpleName() + " " + line);
    }

    /** The parcelable creators the classes have. */
    private void creators() {
        Log.i(TAG, "SessionInfo creator: " + (PackageInstaller.SessionInfo.CREATOR != null));
        Log.i(TAG, "SessionParams creator: " + (PackageInstaller.SessionParams.CREATOR != null));
        Log.i(TAG, "ApplicationExitInfo creator: " + (ApplicationExitInfo.CREATOR != null));
    }

    /** The app's own exit reasons: none. */
    private void exits() {
        ActivityManager manager = getSystemService(ActivityManager.class);
        List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(getPackageName(), 0, 0);
        Log.i(TAG, "exit reasons: " + exits.size());
        Log.i(TAG, "exit reasons of 5: " + manager.getHistoricalProcessExitReasons(null, 0, 5).size());
    }
}
