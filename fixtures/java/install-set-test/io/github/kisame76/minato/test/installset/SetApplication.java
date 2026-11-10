package io.github.kisame76.minato.test.installset;

import android.app.Application;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Writes a split set, the four APKs of another app's set (base, feature, arm64 and language splits), into one
 * PackageInstaller session and commits it: an installer that takes sets asks the user (STATUS_PENDING_USER_ACTION with the confirm intent).
 */
public class SetApplication extends Application {
    private static final String TAG = "install-set-test";
    private static final String ACTION = "io.github.kisame76.minato.test.installset.STATUS";
    private static final String[] FILES = {"base.apk", "split_config.arm64_v8a.apk", "split_config.en.apk",
            "split_feature.apk"};

    @Override
    public void onCreate() {
        super.onCreate();
        registerReceiver(new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                final Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
                Log.i(TAG, "set status: " + intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -99)
                        + (confirm != null ? ", confirm intent: " + confirm.getAction() : ""));
                Log.i(TAG + "-device", "message: " + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                Log.i(TAG, "done");
            }
        }, new IntentFilter(ACTION), Context.RECEIVER_NOT_EXPORTED);
        new Handler(Looper.getMainLooper()).post(this::install);
    }

    private void install() {
        try {
            final PackageInstaller installer = getPackageManager().getPackageInstaller();
            final int id = installer.createSession(
                    new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL));
            try (PackageInstaller.Session session = installer.openSession(id)) {
                for (String name : FILES) {
                    try (InputStream in = getAssets().open(name); OutputStream out = session.openWrite(name, 0, -1)) {
                        in.transferTo(out);
                        session.fsync(out);
                    }
                }
                final PendingIntent sender = PendingIntent.getBroadcast(this, 1,
                        new Intent(ACTION).setPackage(getPackageName()), PendingIntent.FLAG_MUTABLE);
                session.commit(sender.getIntentSender());
            }
        } catch (IOException | RuntimeException e) {
            Log.i(TAG, "install failed: " + e.getClass().getName());
            Log.i(TAG, "done");
        }
    }
}
