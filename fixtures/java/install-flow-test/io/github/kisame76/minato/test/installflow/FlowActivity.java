package io.github.kisame76.minato.test.installflow;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;

/**
 * An app installer's flow: without the grant it opens the settings page for unknown app sources of its own package,
 * and with it hands its own APK to the package installer, logging what each step returns.
 */
public class FlowActivity extends Activity {
    private static final String TAG = "install-flow-test";
    private static final String TYPE = "application/vnd.android.package-archive";
    private static final int SOURCES = 1;
    private static final int INSTALL = 2;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final boolean allowed = getPackageManager().canRequestPackageInstalls();
        Log.i(TAG, "canRequestPackageInstalls: " + allowed);
        if (allowed) {
            install();
        } else {
            startActivityForResult(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + getPackageName())), SOURCES);
        }
    }

    @SuppressWarnings("deprecation")
    private void install() {
        final Uri apk = Uri.parse("content://" + getPackageName() + ".apk/self.apk");
        final Intent intent = new Intent(Intent.ACTION_INSTALL_PACKAGE).setDataAndType(apk, TYPE)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION).putExtra(Intent.EXTRA_RETURN_RESULT, true);
        startActivityForResult(intent, INSTALL);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == SOURCES) {
            final boolean allowed = getPackageManager().canRequestPackageInstalls();
            Log.i(TAG, "canRequestPackageInstalls after the settings page: " + allowed);
            if (allowed) {
                install();
            } else {
                Log.i(TAG, "done");
            }
        } else if (request == INSTALL) {
            Log.i(TAG, "install result: " + (result == RESULT_OK ? "ok" : result == RESULT_CANCELED ? "cancelled"
                    : "failed " + result));
            Log.i(TAG, "done");
        }
    }
}
