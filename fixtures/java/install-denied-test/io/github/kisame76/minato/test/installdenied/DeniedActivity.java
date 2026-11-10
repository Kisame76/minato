package io.github.kisame76.minato.test.installdenied;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;

/**
 * Hands an APK to the installer without the right to install apps. The installer does not install: it asks for the
 * right first, and the user's answer decides. With no one to ask (no host answer) the app gets RESULT_CANCELED. The
 * device line says how many times the framework's settings stand-in asked the host (Minato's counter, read by
 * reflection; Android has none).
 */
public class DeniedActivity extends Activity {
    private static final String TAG = "install-denied-test";
    private static final String TYPE = "application/vnd.android.package-archive";

    private File mStale;

    private static int asked() {
        try {
            final Method method = Class.forName("com.android.internal.minato.install.InstallSourcesActivity")
                    .getDeclaredMethod("asked");
            method.setAccessible(true);
            return (Integer) method.invoke(null);
        } catch (ReflectiveOperationException | RuntimeException e) {
            return -1;
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "canRequestPackageInstalls: " + getPackageManager().canRequestPackageInstalls());
        // A copy an earlier installer activity left behind (Minato's: cache/install); the next one clears it.
        mStale = new File(getCacheDir(), "install/999.apk");
        mStale.getParentFile().mkdirs();
        try {
            mStale.createNewFile();
        } catch (IOException e) {
            Log.w(TAG, "no stale file: " + e);
        }
        final Uri apk = Uri.parse("content://" + getPackageName() + ".files/app.apk");
        startActivityForResult(new Intent(Intent.ACTION_INSTALL_PACKAGE).setDataAndType(apk, TYPE), 7);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Log.i(TAG, "result: " + (requestCode == 7 ? resultCode : -99));
        Log.i(TAG + "-device", "asked the host for the right: " + asked());
        Log.i(TAG + "-device", "stale copy cleared: " + !mStale.exists());
        Log.i(TAG, "done");
    }
}
