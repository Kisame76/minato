package io.github.kisame76.minato.test.installreason;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * Hands three APKs that cannot be installed over the app to the installer, each by an intent (ACTION_INSTALL_PACKAGE
 * with EXTRA_RETURN_RESULT) and by a PackageInstaller session, and logs what the installer says: a file that is no
 * APK, the app itself signed with another key, and the app in an older version (a downgrade); then another package
 * that can be installed, whose result is what an install that goes through gives.
 */
public class ReasonActivity extends Activity {
    private static final String TAG = "install-reason-test";
    private static final String TYPE = "application/vnd.android.package-archive";
    private static final String ACTION = "io.github.kisame76.minato.test.installreason.STATUS";
    private static final String[] NAMES = {"junk", "other", "older", "third"};

    private int mCase;
    private boolean mSession;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private BroadcastReceiver mReceiver;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            for (String name : NAMES) {
                try (InputStream in = getAssets().open(name + ".apk")) {
                    Files.copy(in, new File(getCacheDir(), name + ".apk").toPath(),
                            StandardCopyOption.REPLACE_EXISTING);
                }
            }
        } catch (IOException e) {
            Log.i(TAG, "no assets: " + e);
            Log.i(TAG, "done");
            return;
        }
        mReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                onStatus(intent);
            }
        };
        registerReceiver(mReceiver, new IntentFilter(ACTION), Context.RECEIVER_NOT_EXPORTED);
        next();
    }

    /** The next step: the case's intent, then its session, then the next case. */
    private void next() {
        if (mCase >= NAMES.length) {
            unregisterReceiver(mReceiver);
            Log.i(TAG, "done");
            return;
        }
        final String name = NAMES[mCase];
        if (!mSession) {
            final Uri apk = Uri.parse("content://" + getPackageName() + ".files/" + name + ".apk");
            startActivityForResult(new Intent(Intent.ACTION_INSTALL_PACKAGE).setDataAndType(apk, TYPE)
                    .putExtra(Intent.EXTRA_RETURN_RESULT, true)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), 1);
            return;
        }
        try {
            final PackageInstaller installer = getPackageManager().getPackageInstaller();
            final PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                    PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(name.equals("third") ? getPackageName() + ".third" : getPackageName());
            final int id = installer.createSession(params);
            final PendingIntent receiver = PendingIntent.getBroadcast(this, id,
                    new Intent(ACTION).setPackage(getPackageName()), PendingIntent.FLAG_MUTABLE);
            try (PackageInstaller.Session session = installer.openSession(id)) {
                try (InputStream in = Files.newInputStream(new File(getCacheDir(), name + ".apk").toPath());
                        OutputStream out = session.openWrite("base.apk", 0, -1)) {
                    final byte[] buffer = new byte[8192];
                    for (int n; (n = in.read(buffer)) > 0; ) {
                        out.write(buffer, 0, n);
                    }
                    session.fsync(out);
                }
                session.commit(receiver.getIntentSender());
            }
        } catch (IOException | RuntimeException e) {
            Log.i(TAG, name + " session: " + e.getClass().getName());
            advance();
            return;
        }
        // No answer within a minute: the case ends.
        mHandler.postDelayed(() -> {
            Log.i(TAG, name + " session: no status");
            advance();
        }, 60000);
    }

    private void advance() {
        mHandler.removeCallbacksAndMessages(null);
        if (mSession) {
            mCase++;
        }
        mSession = !mSession;
        next();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Log.i(TAG, NAMES[mCase] + " intent: result " + resultCode + ", install result "
                + (data == null ? "no data" : String.valueOf(data.getIntExtra(Intent.EXTRA_INSTALL_RESULT, -99))));
        advance();
    }

    private void onStatus(Intent intent) {
        final int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -99);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            // The user's step: start the activity the installer names; its end sends the final status.
            Log.i(TAG, NAMES[mCase] + " session: pending");
            startActivity((Intent) intent.getParcelableExtra(Intent.EXTRA_INTENT));
            return;
        }
        Log.i(TAG, NAMES[mCase] + " session: status " + status);
        advance();
    }
}
