package com.android.internal.minato.install;

import android.app.Activity;
import android.app.ActivityThread;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.LocalPackageInstaller;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The activity that stands in for Android's uninstaller when an app asks to remove an app (ACTION_DELETE or
 * ACTION_UNINSTALL_PACKAGE with "package:<name>", or PackageInstaller.uninstall, whose status intent starts it): an
 * app without REQUEST_DELETE_PACKAGES is refused at once with RESULT_FIRST_USER, as Android's is; otherwise the guest
 * only asks (APKRT_REQUEST_UNINSTALL) and the user decides on the host, which names the target app. It ends as
 * Android's uninstaller does: RESULT_OK once the app is gone, RESULT_CANCELED when the user declined,
 * RESULT_FIRST_USER when the app is not there or could not be removed; with EXTRA_RETURN_RESULT an uninstall that
 * happened carries EXTRA_INSTALL_RESULT. No window of its own.
 */
public final class UninstallActivity extends Activity {
    private static final String TAG = "Uninstall";
    /** Linux's EOPNOTSUPP, as ActivityThread's requests answer without a session. */
    private static final int EOPNOTSUPP = 95;
    private static final String CLASS = UninstallActivity.class.getName();
    private static final String PERMISSION = "android.permission.REQUEST_DELETE_PACKAGES";
    /** The extra that names the PackageInstaller.uninstall status receiver the answer goes to (Minato's). */
    public static final String EXTRA_CALLBACK_ID = "com.android.internal.minato.extra.UNINSTALL_CALLBACK";

    private static final AtomicLong sNextId = new AtomicLong(1);
    /** The activities waiting for the host's answer, by request ID; the main thread's. */
    private static final Map<Long, UninstallActivity> sWaiting = new HashMap<>();

    private long mId;
    private String mTarget;
    private int mCallback;
    private boolean mReported;

    /** Whether `intent` asks to remove a package, naming no component. */
    @SuppressWarnings("deprecation") // ACTION_UNINSTALL_PACKAGE is the action apps still send
    public static boolean handles(Intent intent) {
        final String action = intent.getAction();
        return (Intent.ACTION_DELETE.equals(action) || Intent.ACTION_UNINSTALL_PACKAGE.equals(action))
                && intent.getPackage() == null && intent.getData() != null
                && "package".equals(intent.getData().getScheme());
    }

    /** What the package manager lists for the request: Android's package installer, whose uninstaller it is. */
    public static ResolveInfo resolveInfo() {
        final ResolveInfo resolved = new ResolveInfo();
        final ActivityInfo info = new ActivityInfo();
        info.packageName = "com.google.android.packageinstaller";
        info.name = "com.android.packageinstaller.UninstallerActivity";
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
        // A change of the configuration does not recreate it, which would ask the host a second time.
        info.configChanges = -1;
        info.theme = com.android.internal.R.style.Theme_Translucent_NoTitleBar;
        return info;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final Uri data = getIntent().getData();
        mTarget = data == null ? "" : data.getSchemeSpecificPart();
        mCallback = getIntent().getIntExtra(EXTRA_CALLBACK_ID, 0);
        if (mTarget == null || mTarget.isEmpty() || !ActivityThread.permissionGranted(PERMISSION)) {
            end(RESULT_FIRST_USER, 1);
            return;
        }
        mId = sNextId.getAndIncrement();
        sWaiting.put(mId, this);
        final int error = ActivityThread.requestUninstall(mId, mTarget + "\n" + getPackageName());
        if (error != 0) {
            if (error != -EOPNOTSUPP) {
                Log.w(TAG, "The host refused the request: " + error);
            }
            sWaiting.remove(mId);
            end(error == -EOPNOTSUPP ? RESULT_CANCELED : RESULT_FIRST_USER, error == -EOPNOTSUPP ? 2 : 1);
        }
    }

    /** The host's answer (UNINSTALL_RESULT): the request ID and 0 removed, 1 failed (also: not there), 2 cancelled. */
    public static void onResult(long id, int status) {
        final UninstallActivity activity = sWaiting.remove(id);
        if (activity == null) {
            Log.w(TAG, "An answer for request " + id + " that nobody waits for");
            return;
        }
        activity.end(status == 0 ? RESULT_OK : status == 2 ? RESULT_CANCELED : RESULT_FIRST_USER, status);
    }

    private void end(int result, int status) {
        if (!mReported) {
            mReported = true;
            if (mCallback != 0) {
                LocalPackageInstaller.uninstallResult(mCallback, mTarget, status);
            }
        }
        if (result == RESULT_OK && getIntent().getBooleanExtra(Intent.EXTRA_RETURN_RESULT, false)) {
            // PackageManager.DELETE_SUCCEEDED
            setResult(result, new Intent().putExtra(Intent.EXTRA_INSTALL_RESULT, 1));
        } else {
            setResult(result);
        }
        finish();
    }

    @Override
    protected void onDestroy() {
        sWaiting.remove(mId);
        // Gone without an answer (back press, finish from outside): the user backed out.
        if (!mReported) {
            mReported = true;
            if (mCallback != 0) {
                LocalPackageInstaller.uninstallResult(mCallback, mTarget, 2);
            }
        }
        super.onDestroy();
    }
}
