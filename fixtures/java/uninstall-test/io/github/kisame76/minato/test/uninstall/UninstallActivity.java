package io.github.kisame76.minato.test.uninstall;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * Asks to uninstall an app that is not installed, three ways: ACTION_UNINSTALL_PACKAGE with EXTRA_RETURN_RESULT,
 * ACTION_DELETE, and PackageInstaller.uninstall with its status callback. Android answers all three without a dialog
 * for an absent package. The same code runs with and without REQUEST_DELETE_PACKAGES in the manifest.
 */
public class UninstallActivity extends Activity {
    private static final String TAG = "uninstall-test";
    private static final String ACTION = "io.github.kisame76.minato.test.uninstall.STATUS";
    private static final Uri ABSENT = Uri.parse("package:io.github.kisame76.absent");

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "holds REQUEST_DELETE_PACKAGES: " + (checkSelfPermission("android.permission.REQUEST_DELETE_PACKAGES")
                == PackageManager.PERMISSION_GRANTED));
        Log.i(TAG, "resolves ACTION_DELETE: " + (getPackageManager().resolveActivity(
                new Intent(Intent.ACTION_DELETE, ABSENT), 0) != null));
        Log.i(TAG, "resolves ACTION_UNINSTALL_PACKAGE: " + (getPackageManager().resolveActivity(
                new Intent(Intent.ACTION_UNINSTALL_PACKAGE, ABSENT), 0) != null));
        try {
            startActivityForResult(new Intent(Intent.ACTION_UNINSTALL_PACKAGE, ABSENT)
                    .putExtra(Intent.EXTRA_RETURN_RESULT, true), 1);
        } catch (RuntimeException e) {
            Log.i(TAG, "uninstall package: " + e.getClass().getName());
            second();
        }
    }

    private void second() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_DELETE, ABSENT), 2);
        } catch (RuntimeException e) {
            Log.i(TAG, "delete: " + e.getClass().getName());
            third();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == 1) {
            Log.i(TAG, "uninstall package: result " + resultCode + ", install result "
                    + (data == null ? "no data" : String.valueOf(data.getIntExtra(Intent.EXTRA_INSTALL_RESULT, -99))));
            second();
        } else if (requestCode == 2) {
            Log.i(TAG, "delete: result " + resultCode + ", " + (data == null ? "no data" : "data"));
            third();
        } else if (requestCode == 3 || requestCode == 4) {
            Log.i(TAG, "self " + (requestCode == 3 ? "uninstall package" : "delete") + ": result " + resultCode
                    + ", install result " + (data == null ? "no data"
                    : String.valueOf(data.getIntExtra(Intent.EXTRA_INSTALL_RESULT, -99))));
            self(requestCode + 1);
        }
    }

    /** The app asks to uninstall itself (3: ACTION_UNINSTALL_PACKAGE, 4: ACTION_DELETE); a dialog asks the user. */
    private void self(int request) {
        final Uri self = Uri.parse("package:" + getPackageName());
        if (request == 3) {
            Log.i(TAG, "self asks");
            startActivityForResult(new Intent(Intent.ACTION_UNINSTALL_PACKAGE, self)
                    .putExtra(Intent.EXTRA_RETURN_RESULT, true), 3);
        } else if (request == 4) {
            startActivityForResult(new Intent(Intent.ACTION_DELETE, self), 4);
        } else {
            Log.i(TAG, "done");
        }
    }

    private void third() {
        final Handler handler = new Handler(Looper.getMainLooper());
        final BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                unregisterReceiver(this);
                handler.removeCallbacksAndMessages(null);
                Log.i(TAG, "installer status: " + intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -99)
                        + ", package " + intent.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME));
                self(3);
            }
        };
        registerReceiver(receiver, new IntentFilter(ACTION), Context.RECEIVER_NOT_EXPORTED);
        handler.postDelayed(() -> {
            Log.i(TAG, "installer status: none");
            self(3);
        }, 10000);
        final PendingIntent pending = PendingIntent.getBroadcast(this, 0,
                new Intent(ACTION).setPackage(getPackageName()), PendingIntent.FLAG_MUTABLE);
        try {
            getPackageManager().getPackageInstaller().uninstall("io.github.kisame76.absent", pending.getIntentSender());
        } catch (RuntimeException e) {
            handler.removeCallbacksAndMessages(null);
            unregisterReceiver(receiver);
            Log.i(TAG, "installer uninstall: " + e.getClass().getName());
            self(3);
        }
    }
}
