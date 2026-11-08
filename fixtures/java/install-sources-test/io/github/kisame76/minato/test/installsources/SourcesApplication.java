package io.github.kisame76.minato.test.installsources;

import android.app.Application;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

/**
 * An app that wants to install apps asks first whether it may (Android 15, nothing granted): the answer is false, and
 * the settings page for unknown app sources of its own package resolves, so that the app can send the user there. The
 * install intents for an APK resolve too.
 */
public class SourcesApplication extends Application {
    private static final String TAG = "install-sources-test";
    private static final String TYPE = "application/vnd.android.package-archive";

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::start);
    }

    @SuppressWarnings("deprecation")
    private void start() {
        final PackageManager packages = getPackageManager();
        Log.i(TAG, "canRequestPackageInstalls: " + packages.canRequestPackageInstalls());
        final Intent own = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                Uri.parse("package:" + getPackageName()));
        final ResolveInfo found = packages.resolveActivity(own, 0);
        Log.i(TAG, "settings page resolves: " + (found != null));
        final Uri apk = Uri.parse("content://" + getPackageName() + ".files/app.apk");
        final Intent install = new Intent(Intent.ACTION_INSTALL_PACKAGE).setDataAndType(apk, TYPE);
        Log.i(TAG, "install intent resolves: " + (packages.resolveActivity(install, 0) != null));
        final Intent view = new Intent(Intent.ACTION_VIEW).setDataAndType(apk, TYPE);
        Log.i(TAG, "view intent resolves: " + (packages.resolveActivity(view, 0) != null));
        Log.i(TAG, "done");
    }
}
