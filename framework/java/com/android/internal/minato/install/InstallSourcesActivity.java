package com.android.internal.minato.install;

import android.app.Activity;
import android.app.ActivityThread;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The activity that stands in for Android's settings page "Install unknown apps" of one app
 * (Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES with "package:<name>"): it asks the host whether the app may install
 * apps (APKRT_REQUEST_ASK_INSTALL_SOURCES), whose sheet is the page, and ends with RESULT_CANCELED as the settings
 * page does when the user comes back. The answer is the grant canRequestPackageInstalls then reports. It has no
 * window of its own; the translucent theme keeps the app's under it.
 */
public final class InstallSourcesActivity extends Activity {
    private static final String TAG = "InstallSources";
    /** Linux's EOPNOTSUPP, as ActivityThread's requests answer without a session. */
    private static final int EOPNOTSUPP = 95;
    private static final String CLASS = InstallSourcesActivity.class.getName();
    private static final String ACTION = "android.settings.MANAGE_UNKNOWN_APP_SOURCES";

    private static final AtomicLong sNextId = new AtomicLong(1);
    /** The activities waiting for the host's answer, by request ID; the main thread's. */
    private static final Map<Long, InstallSourcesActivity> sWaiting = new HashMap<>();

    private static int sAsked;
    private long mId;

    /** How many times the page asked the host (for tests, read by reflection). */
    static int asked() {
        return sAsked;
    }

    /** Whether `intent` is the settings page for unknown app sources, which names no component. */
    public static boolean handles(Intent intent) {
        return ACTION.equals(intent.getAction()) && intent.getPackage() == null;
    }

    /** What the package manager lists for the settings page: Android's own Settings app, as the page is its. */
    public static ResolveInfo resolveInfo() {
        final ResolveInfo resolved = new ResolveInfo();
        final ActivityInfo info = new ActivityInfo();
        info.packageName = "com.android.settings";
        info.name = "com.android.settings.spa.app.appinfo.ExternalSourcesAppListActivity";
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
        final String own = getPackageName();
        // Android's page is for one package; the app's own is the only one the host decides.
        if (data != null && !own.equals(data.getSchemeSpecificPart())) {
            setResult(RESULT_CANCELED);
            finish();
            return;
        }
        sAsked++;
        final String label = String.valueOf(getApplicationInfo().loadLabel(getPackageManager()));
        mId = sNextId.getAndIncrement();
        sWaiting.put(mId, this);
        final int error = ActivityThread.requestInstallSources(mId, own + "\n" + label);
        if (error != 0) {
            if (error != -EOPNOTSUPP) {
                Log.w(TAG, "The host refused the request: " + error);
            }
            sWaiting.remove(mId);
            setResult(RESULT_CANCELED);
            finish();
        }
    }

    /** The host's answer (INSTALL_SOURCES_RESULT): the request ID and whether the app may install apps. */
    public static void onResult(long id, int allowed) {
        final InstallSourcesActivity activity = sWaiting.remove(id);
        ActivityThread.setInstallSources(allowed != 0);
        if (activity == null) {
            Log.w(TAG, "An answer for request " + id + " that nobody waits for");
            return;
        }
        activity.setResult(RESULT_CANCELED);
        activity.finish();
    }

    @Override
    protected void onDestroy() {
        sWaiting.remove(mId);
        super.onDestroy();
    }
}
