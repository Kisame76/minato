package io.github.kisame76.minato.test.contexts;

import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.hardware.display.DisplayManager;
import android.os.ConditionVariable;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.system.ErrnoException;
import android.system.Os;
import android.system.StructStat;
import android.util.Log;
import android.view.Display;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Locale;

/**
 * The Context calls and classes that apps' SDKs use at their start (the Unity test APK probe's blockers 2, 4, 5, 6 and 8),
 * each logged as Android 15 answers it: the main looper, package contexts, the device protected storage context, a
 * ConditionVariable and a Messenger; and the other contexts an app makes: for a configuration, a display and a split.
 */
public class ContextsApplication extends Application {
    static final String TAG = "contexts-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        log("main looper: the main thread's " + (getMainLooper() == Looper.getMainLooper()));
        packageContexts();
        storageContexts();
        configurationContext();
        displayContext();
        splitContext();
        attributionContext();
        conditionVariable();
        messenger();
    }

    private void packageContexts() {
        try {
            Context own = createPackageContext(getPackageName(), 0);
            log("own package context: " + own.getPackageName() + ", the same data folder "
                    + own.getDataDir().equals(getDataDir()) + ", the app's name "
                    + own.getString(own.getResources().getIdentifier("app_name", "string", getPackageName())));
        } catch (PackageManager.NameNotFoundException e) {
            log("own package context: " + e);
        }
        try {
            Context system = createPackageContext("android", 0);
            log("android package context: " + system.getPackageName() + ", its OK "
                    + system.getString(android.R.string.ok));
        } catch (PackageManager.NameNotFoundException e) {
            log("android package context: " + e);
        }
        try {
            // CONTEXT_DEVICE_PROTECTED_STORAGE, a system API's flag, which javac inlines.
            Context device = createPackageContext(getPackageName(), 0x8);
            log("own package context, device protected: " + device.getDataDir());
        } catch (PackageManager.NameNotFoundException e) {
            log("own package context, device protected: " + e);
        }
        try {
            Context application = createApplicationContext(getApplicationInfo(), 0);
            log("application context: " + application.getPackageName() + ", the same data folder "
                    + application.getDataDir().equals(getDataDir()));
        } catch (PackageManager.NameNotFoundException e) {
            log("application context: " + e);
        }
        try {
            Context missing = createPackageContext("io.github.kisame76.minato.test.nosuchapp", 0);
            log("a missing package's context: " + missing.getPackageName());
        } catch (PackageManager.NameNotFoundException e) {
            log("a missing package's context: " + e);
        }
    }

    private void storageContexts() {
        Context device = createDeviceProtectedStorageContext();
        log("app context: device protected " + isDeviceProtectedStorage() + ", data folder " + getDataDir());
        log("device protected context: device protected " + device.isDeviceProtectedStorage() + ", data folder "
                + device.getDataDir() + ", files " + device.getFilesDir() + ", cache " + device.getCacheDir());
        log("device protected again: " + device.createDeviceProtectedStorageContext().getDataDir());
        try {
            log("canonical data folders: " + getDataDir().getCanonicalPath() + ", device protected "
                    + device.getDataDir().getCanonicalPath());
        } catch (IOException e) {
            log("canonical data folders: " + e);
        }
        // Only shown, as owners differ by device: the folders above the device protected data, root's 0751 on
        // Android 15 (zygote's tmpfs and the user's folder in it).
        for (String folder : new String[] {"/data/user_de", "/data/user_de/0"}) {
            try {
                StructStat status = Os.stat(folder);
                Log.i(TAG + "-device", folder + ": " + status.st_uid + ":" + status.st_gid + " "
                        + Integer.toOctalString(status.st_mode & 07777));
            } catch (ErrnoException e) {
                Log.i(TAG + "-device", folder + ": " + e);
            }
        }
        device.getSharedPreferences("storage", MODE_PRIVATE).edit().putString("where", "device").commit();
        log("a preference written there: there " + device.getSharedPreferences("storage", MODE_PRIVATE)
                .getString("where", "none") + ", in the app's own " + getSharedPreferences("storage", MODE_PRIVATE)
                .getString("where", "none"));
        getSharedPreferences("moved", MODE_PRIVATE).edit().putInt("count", 7).commit();
        boolean moved = device.moveSharedPreferencesFrom(this, "moved");
        log("moveSharedPreferencesFrom: " + moved + ", there " + device.getSharedPreferences("moved", MODE_PRIVATE)
                .getInt("count", -1) + ", left in the app's own " + getSharedPreferences("moved", MODE_PRIVATE)
                .getInt("count", -1));
        log("moving a missing file: " + device.moveSharedPreferencesFrom(this, "missing"));
        File database = getDatabasePath("moved.db");
        try (FileOutputStream out = new FileOutputStream(database)) {
            out.write(new byte[] {1, 2, 3});
        } catch (IOException e) {
            log("a database file: " + e);
        }
        boolean movedDatabase = device.moveDatabaseFrom(this, "moved.db");
        log("moveDatabaseFrom: " + movedDatabase + ", there " + device.getDatabasePath("moved.db").length()
                + " bytes, left in the app's own " + database.exists());
    }

    private void configurationContext() {
        Configuration override = new Configuration(getResources().getConfiguration());
        override.setLocale(Locale.CANADA_FRENCH);
        override.fontScale = 1.5f;
        Context french = createConfigurationContext(override);
        int greeting = getResources().getIdentifier("greeting", "string", getPackageName());
        Configuration taken = french.getResources().getConfiguration();
        log("configuration context: " + taken.getLocales().get(0) + ", font scale " + taken.fontScale + ", greeting "
                + french.getString(greeting) + ", its own Resources " + (french.getResources() != getResources())
                + "; the app's greeting still " + getString(greeting));
        try {
            createConfigurationContext(null);
            log("a null configuration: accepted");
        } catch (IllegalArgumentException e) {
            log("a null configuration: " + e);
        }
    }

    private void displayContext() {
        Display display = getSystemService(DisplayManager.class).getDisplay(Display.DEFAULT_DISPLAY);
        Context onDisplay = createDisplayContext(display);
        log("display context: display " + onDisplay.getDisplay().getDisplayId() + ", a UI context "
                + onDisplay.isUiContext() + ", the app's density " + (onDisplay.getResources().getDisplayMetrics()
                .densityDpi == getResources().getDisplayMetrics().densityDpi));
        try {
            createDisplayContext(null);
            log("a null display: accepted");
        } catch (IllegalArgumentException e) {
            log("a null display: " + e);
        }
    }

    private void splitContext() {
        try {
            log("context for a split: the base context " + (createContextForSplit("feature") == getBaseContext()));
        } catch (PackageManager.NameNotFoundException e) {
            log("context for a split: " + e);
        }
    }

    private void attributionContext() {
        Context tagged = createAttributionContext("contexts");
        log("attribution context: tag " + tagged.getAttributionTag() + ", the app's " + getAttributionTag()
                + ", its package " + tagged.getPackageName() + ", its device protected copy's tag "
                + tagged.createDeviceProtectedStorageContext().getAttributionTag());
    }

    private void conditionVariable() {
        final ConditionVariable condition = new ConditionVariable();
        boolean closed = condition.block(20);
        condition.open();
        boolean opened = condition.block(20);
        condition.close();
        boolean closedAgain = condition.block(20);
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Thread.sleep(50);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                condition.open();
            }
        }).start();
        boolean byThread = condition.block(5000);
        log("condition variable: closed " + closed + ", opened " + opened + ", closed again " + closedAgain
                + ", opened by a thread " + byThread);
    }

    private void messenger() {
        Handler handler = new Handler(Looper.getMainLooper()) {
            @Override
            public void handleMessage(Message message) {
                log("message " + message.what + ", arg1 " + message.arg1 + ", on the main thread "
                        + (Looper.myLooper() == Looper.getMainLooper()));
                log("done");
            }
        };
        Messenger messenger = new Messenger(handler);
        Messenger again = new Messenger(messenger.getBinder());
        log("messenger through its binder: equal " + again.equals(messenger) + ", the same binder "
                + (again.getBinder() == messenger.getBinder()));
        try {
            again.send(Message.obtain(null, 1, 42, 0));
        } catch (RemoteException e) {
            log("send: " + e);
        }
    }
}
