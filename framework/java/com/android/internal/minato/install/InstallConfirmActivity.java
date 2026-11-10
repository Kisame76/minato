package com.android.internal.minato.install;

import android.app.Activity;
import android.app.ActivityThread;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.LocalPackageInstaller;
import android.content.pm.PackageInstaller;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The activity that stands in for Android's package installer when an app hands it an APK (ACTION_INSTALL_PACKAGE, or
 * ACTION_VIEW of application/vnd.android.package-archive): the app must be allowed to install apps
 * (canRequestPackageInstalls), the file is copied below the app's cache folder, and the host
 * (APKRT_REQUEST_CONFIRM_INSTALL) asks the user and imports it into the library. It ends as Android's installer
 * does: RESULT_OK once the app is in the library, RESULT_CANCELED when the user declined, RESULT_FIRST_USER when the
 * install failed. No window of its own.
 */
public final class InstallConfirmActivity extends Activity {
    private static final String TAG = "InstallConfirm";
    private static final String TYPE = "application/vnd.android.package-archive";
    /** Linux's EOPNOTSUPP, as ActivityThread's requests answer without a session. */
    private static final int EOPNOTSUPP = 95;
    private static final String CLASS = InstallConfirmActivity.class.getName();

    private static final AtomicLong sNextId = new AtomicLong(1);
    /** The activities waiting for the host's answer, by request ID; the main thread's. */
    private static final Map<Long, InstallConfirmActivity> sWaiting = new HashMap<>();

    private long mId;
    private File mFile;
    /** The session this confirms (PackageInstaller), or -1 for an APK handed over by an intent. */
    private int mSession = -1;
    private boolean mReported;

    /**
     * Whether `intent` hands over an APK, naming no component: a session's confirm intent, or an install or view
     * intent of the APK type, which an intent without a type has from the provider of its content URI, as Android
     * resolves it (`resolver` is the app's, null for the intent's own type only).
     */
    @SuppressWarnings("deprecation")
    public static boolean handles(Intent intent, ContentResolver resolver) {
        final String action = intent.getAction();
        if (PackageInstaller.ACTION_CONFIRM_INSTALL.equals(action)) {
            return true;
        }
        if (!(Intent.ACTION_INSTALL_PACKAGE.equals(action) || Intent.ACTION_VIEW.equals(action))
                || intent.getPackage() != null) {
            return false;
        }
        final String type = resolver != null ? intent.resolveTypeIfNeeded(resolver) : intent.getType();
        return TYPE.equals(type);
    }

    /** What the package manager lists for the handed-over APK: Android's package installer. */
    public static ResolveInfo resolveInfo() {
        final ResolveInfo resolved = new ResolveInfo();
        final ActivityInfo info = new ActivityInfo();
        info.packageName = "com.google.android.packageinstaller";
        info.name = "com.android.packageinstaller.InstallStart";
        info.exported = true;
        info.applicationInfo = new ApplicationInfo();
        info.applicationInfo.packageName = info.packageName;
        resolved.activityInfo = info;
        resolved.match = 0x108000;
        resolved.isDefault = true;
        return resolved;
    }

    /** The activity of the framework class, in the app's package as an activity of the app is. */
    public static ActivityInfo activityInfo(ApplicationInfo app) {
        final ActivityInfo info = new ActivityInfo();
        info.packageName = app.packageName;
        info.name = CLASS;
        info.applicationInfo = app;
        info.processName = app.processName;
        info.exported = false;
        info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
        info.configChanges = -1;
        info.theme = com.android.internal.R.style.Theme_Translucent_NoTitleBar;
        return info;
    }

    private static final int ASK_FOR_THE_RIGHT = 1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        clearStaleCopies();
        mSession = getIntent().getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1);
        if (mSession < 0 && getIntent().getData() == null) {
            end(RESULT_CANCELED);
        } else if (!ActivityThread.installSources()) {
            // As Android's installer: without the right to install apps it sends the user to the settings page for the
            // app (its "unknown sources" dialog) and goes on once the right is given.
            startActivityForResult(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + getPackageName())), ASK_FOR_THE_RIGHT);
        } else {
            confirm();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != ASK_FOR_THE_RIGHT) {
            return;
        }
        if (ActivityThread.installSources()) {
            confirm();
        } else {
            end(RESULT_CANCELED);
        }
    }

    /**
     * Removes the copies below the cache folder that no request waits for any more: an activity that went before the
     * host answered leaves its file, because the host may still be reading it. The files of waiting requests stay.
     */
    private void clearStaleCopies() {
        final File[] copies = new File(getCacheDir(), "install").listFiles();
        if (copies == null) {
            return;
        }
        for (File copy : copies) {
            boolean waiting = false;
            for (InstallConfirmActivity other : sWaiting.values()) {
                waiting |= other != this && copy.equals(other.mFile);
            }
            if (!waiting) {
                copy.delete();
            }
        }
    }

    /** Copies the APK below the cache folder and has the host confirm it. */
    private void confirm() {
        mId = sNextId.getAndIncrement();
        if (mSession >= 0) {
            mFile = LocalPackageInstaller.sessionApk(mSession);
            if (mFile == null) {
                end(RESULT_FIRST_USER);
                return;
            }
        } else {
            final Uri data = getIntent().getData();
            mFile = new File(getCacheDir(), "install/" + mId + ".apk");
            try (InputStream in = getContentResolver().openInputStream(data)) {
                if (in == null) {
                    throw new IOException("no stream for " + data);
                }
                mFile.getParentFile().mkdirs();
                Files.copy(in, mFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "The APK cannot be read: " + e);
                end(RESULT_FIRST_USER);
                return;
            }
        }
        sWaiting.put(mId, this);
        final int error = ActivityThread.requestConfirmInstall(mId, mFile.getPath() + "\n" + getPackageName());
        if (error != 0) {
            if (error != -EOPNOTSUPP) {
                Log.w(TAG, "The host refused the request: " + error);
            }
            sWaiting.remove(mId);
            end(error == -EOPNOTSUPP ? RESULT_CANCELED : RESULT_FIRST_USER);
        }
    }

    /** The host's answer (INSTALL_RESULT): the request ID and 0 ready, 1 failed, 2 cancelled. */
    public static void onResult(long id, int status) {
        final InstallConfirmActivity activity = sWaiting.remove(id);
        if (activity == null) {
            Log.w(TAG, "An answer for request " + id + " that nobody waits for");
            return;
        }
        activity.end(status == 0 ? RESULT_OK : status == 2 ? RESULT_CANCELED : RESULT_FIRST_USER);
    }

    private void end(int result) {
        if (mFile != null && mSession < 0) {
            mFile.delete();
        }
        if (mSession >= 0 && !mReported) {
            mReported = true;
            LocalPackageInstaller.sessionResult(mSession,
                    result == RESULT_OK ? 0 : result == RESULT_CANCELED ? 2 : 1);
        }
        setResult(result);
        finish();
    }

    @Override
    protected void onDestroy() {
        sWaiting.remove(mId);
        // Gone without an answer (back press, finish from outside): the user backed out, as Android's installer
        // reports it.
        if (mSession >= 0 && !mReported) {
            mReported = true;
            LocalPackageInstaller.sessionResult(mSession, 2);
        }
        super.onDestroy();
    }
}
