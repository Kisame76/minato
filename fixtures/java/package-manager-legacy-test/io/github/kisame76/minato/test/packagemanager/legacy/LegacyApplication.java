package io.github.kisame76.minato.test.packagemanager.legacy;

import android.app.ActivityThread;
import android.app.Application;
import android.app.ApplicationPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.util.Log;
import java.util.Arrays;

/**
 * ADR-0039 b3.5b1's second test: the permissions of an app with targetSdk 28, which platform.xml's split permissions
 * extend, with their flags and grants, logged with the tag package-manager-legacy-test and compared with Android 15's
 * log (tools/android-reference.sh --app package-manager-legacy-test). As package-manager-test, the static
 * initializer reports through our ApplicationPackageManager and onCreate through Android's.
 */
public class LegacyApplication extends Application {
    private static final String TAG = "package-manager-legacy-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.packagemanager.legacy";

    static {
        PackageManager pm = null;
        try {
            pm = new ApplicationPackageManager(ActivityThread.currentActivityThread());
        } catch (NoSuchMethodError e) {
            // Android: there is no such constructor, and onCreate reports.
        }
        if (pm != null) {
            run(pm);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        run(getPackageManager());
    }

    static void run(PackageManager pm) {
        try {
            report(pm);
        } catch (Throwable e) {
            Log.i(TAG, "failed: " + e);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(e));
        }
        Log.i(TAG, "done");
    }

    @SuppressWarnings("deprecation") // getPackageInfo(String, int), which apps below API 33 call.
    static void report(PackageManager pm) throws Exception {
        PackageInfo info = pm.getPackageInfo(PACKAGE, PackageManager.GET_PERMISSIONS);
        Log.i(TAG, "targetSdkVersion " + info.applicationInfo.targetSdkVersion);
        Log.i(TAG, "requestedPermissions " + Arrays.toString(info.requestedPermissions));
        for (int i = 0; i < info.requestedPermissions.length; i++) {
            String permission = info.requestedPermissions[i];
            Log.i(TAG, permission + " flags 0x" + Integer.toHexString(info.requestedPermissionsFlags[i])
                    + ", checkPermission " + pm.checkPermission(permission, PACKAGE));
        }
    }
}
