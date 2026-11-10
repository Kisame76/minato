package io.github.kisame76.minato.test.installsession;

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

import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Installs through a PackageInstaller session twice and logs each status the sender gets: a broken APK (zeros) and the
 * app's own APK (valid; the session is never confirmed).
 */
public class SessionApplication extends Application {
    private static final String TAG = "install-session-test";
    private static final String ACTION = "io.github.kisame76.minato.test.installsession.STATUS";
    private int mCase;
    private Intent mConfirm;

    @Override
    public void onCreate() {
        super.onCreate();
        registerReceiver(new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                final int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -99);
                final Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
                if (mCase <= 2) {
                    Log.i(TAG, "case " + mCase + " status: " + status
                            + (confirm != null ? ", confirm intent: " + confirm.getAction() : ""));
                }
                if (mCase == 1) {
                    mCase = 2;
                    new Handler(Looper.getMainLooper()).post(() -> install(2));
                } else if (mCase == 2 && status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    // Minato only, shown as device lines: the folder lies in the app's data folder, a session with
                    // two files is refused, and a confirm that ends without the host's answer reports ABORTED.
                    Log.i(TAG + "-device", "session folder inside the data folder: "
                            + insideDataFolder(intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1)));
                    mCase = 3;
                    mConfirm = confirm;
                    new Handler(Looper.getMainLooper()).post(() -> install(3));
                } else if (mCase == 3) {
                    Log.i(TAG + "-device", "case 3 status: " + status);
                    mCase = 4;
                    if (mConfirm != null) {
                        mConfirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        startActivity(mConfirm);
                    }
                    new Handler(Looper.getMainLooper()).postDelayed(() -> Log.i(TAG, "done"), 4000);
                } else if (mCase == 4) {
                    Log.i(TAG + "-device", "confirm ended: status " + status);
                } else {
                    Log.i(TAG, "done");
                }
            }
        }, new IntentFilter(ACTION), Context.RECEIVER_NOT_EXPORTED);
        new Handler(Looper.getMainLooper()).post(() -> install(1));
    }

    /** Minato's: whether the APK of the session lies below the app's data folder (reflection; Android has none). */
    private boolean insideDataFolder(int sessionId) {
        try {
            final java.io.File apk = (java.io.File) Class.forName("android.content.pm.LocalPackageInstaller")
                    .getMethod("sessionApk", int.class).invoke(null, sessionId);
            return apk != null && apk.getPath().startsWith(getDataDir().getPath());
        } catch (ReflectiveOperationException | RuntimeException e) {
            return false;
        }
    }

    private void install(int which) {
        mCase = which;
        try {
            final PackageInstaller installer = getPackageManager().getPackageInstaller();
            final int id = installer.createSession(
                    new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL));
            try (PackageInstaller.Session session = installer.openSession(id)) {
                try (OutputStream out = session.openWrite("base.apk", 0, -1)) {
                    if (which == 1) {
                        out.write(new byte[64]);
                    } else {
                        try (InputStream in = new FileInputStream(getApplicationInfo().sourceDir)) {
                            in.transferTo(out);
                        }
                    }
                    session.fsync(out);
                }
                final PendingIntent sender = PendingIntent.getBroadcast(this, which,
                        new Intent(ACTION).setPackage(getPackageName()), PendingIntent.FLAG_MUTABLE);
                if (which == 3) {
                    try (OutputStream second = session.openWrite("split_config.apk", 0, -1)) {
                        second.write(new byte[16]);
                        session.fsync(second);
                    }
                }
                session.commit(sender.getIntentSender());
            }
        } catch (IOException | RuntimeException e) {
            Log.i(TAG, "case " + which + " failed: " + e.getClass().getName());
            Log.i(TAG, "done");
        }
    }
}
