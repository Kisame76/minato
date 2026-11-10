package io.github.kisame76.minato.test.installsessionflow;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;

import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * An app installer through a PackageInstaller session: without the grant it opens the settings page for unknown app
 * sources of its own package, then writes its own APK into a session, commits it, starts the confirm intent of the
 * pending status for a result, and logs what each step returns, ending with the session's final status.
 */
public class SessionFlowActivity extends Activity {
    private static final String TAG = "install-session-flow-test";
    private static final String ACTION = "io.github.kisame76.minato.test.installsessionflow.STATUS";
    private static final int SOURCES = 1;
    private static final int CONFIRM = 2;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        registerReceiver(new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                onStatus(intent);
            }
        }, new IntentFilter(ACTION), Context.RECEIVER_NOT_EXPORTED);
        final boolean allowed = getPackageManager().canRequestPackageInstalls();
        Log.i(TAG, "canRequestPackageInstalls: " + allowed);
        if (allowed) {
            commit();
        } else {
            startActivityForResult(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + getPackageName())), SOURCES);
        }
    }

    private void commit() {
        try {
            final PackageInstaller installer = getPackageManager().getPackageInstaller();
            final int id = installer.createSession(
                    new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL));
            try (PackageInstaller.Session session = installer.openSession(id)) {
                try (OutputStream out = session.openWrite("base.apk", 0, -1);
                        InputStream in = new FileInputStream(getApplicationInfo().sourceDir)) {
                    in.transferTo(out);
                    session.fsync(out);
                }
                final PendingIntent sender = PendingIntent.getBroadcast(this, 1,
                        new Intent(ACTION).setPackage(getPackageName()), PendingIntent.FLAG_MUTABLE);
                session.commit(sender.getIntentSender());
            }
        } catch (IOException | RuntimeException e) {
            Log.i(TAG, "commit failed: " + e.getClass().getName());
            Log.i(TAG, "done");
        }
    }

    private void onStatus(Intent intent) {
        final int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -99);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Log.i(TAG, "status: pending user action");
            final Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
            startActivityForResult(confirm, CONFIRM);
        } else {
            Log.i(TAG, "final status: " + status);
            Log.i(TAG, "done");
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == SOURCES) {
            final boolean allowed = getPackageManager().canRequestPackageInstalls();
            Log.i(TAG, "canRequestPackageInstalls after the settings page: " + allowed);
            if (allowed) {
                commit();
            } else {
                Log.i(TAG, "done");
            }
        } else if (request == CONFIRM) {
            Log.i(TAG, "confirm result: " + (result == RESULT_OK ? "ok" : result == RESULT_CANCELED ? "cancelled"
                    : "failed " + result));
        }
    }
}
