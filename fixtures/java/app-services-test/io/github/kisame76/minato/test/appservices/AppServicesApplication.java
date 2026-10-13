package io.github.kisame76.minato.test.appservices;

import android.app.Application;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * The app's own started services (ADR-0045 step SV2): startService, startForegroundService, stopService, stopSelf and
 * stopSelfResult, startForeground and stopForeground. A start or a stop answers at once; the service's callbacks
 * follow on the main thread, in their order, so each part goes on from a service's callback (posted, so that the
 * callback returns first) and never from a post of its own, whose place among the service's messages Android does not
 * fix.
 */
public class AppServicesApplication extends Application {
    static final String TAG = "app-services-test";
    static final String COUNT = "io.github.kisame76.minato.test.appservices.COUNT";

    static AppServicesApplication sApp;
    /** The last intent a part started a service with, which the service must get a copy of. */
    static Intent sSent;
    private Handler mHandler;
    private int mPart;

    static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        sApp = this;
        mHandler = new Handler(Looper.getMainLooper());
        getSystemService(NotificationManager.class).createNotificationChannel(
                new NotificationChannel("sync", "Sync", NotificationManager.IMPORTANCE_LOW));
        next();
    }

    /** Runs the next part once the callback that asks for it returned. */
    static void next() {
        sApp.mHandler.post(() -> sApp.part(++sApp.mPart));
    }

    private ComponentName start(Intent intent) {
        sSent = intent;
        return startService(intent);
    }

    private Intent counting(int n) {
        return new Intent(this, CountingService.class).putExtra("n", n);
    }

    private static String name(ComponentName component) {
        return component == null ? "null" : component.flattenToShortString();
    }

    private void part(int part) {
        switch (part) {
            case 1:
                // What answers at once: an implicit intent, a missing service, a disabled one, a stop of none.
                try {
                    startService(new Intent(COUNT));
                    log("implicit: started");
                } catch (IllegalArgumentException e) {
                    log("implicit: " + e.getClass().getName() + ": " + e.getMessage());
                }
                log("missing: " + name(startService(new Intent().setClassName(this, getPackageName() + ".Missing"))));
                log("disabled: " + name(startService(new Intent(this, OffService.class))));
                log("stop not running: " + stopService(counting(0)));
                // Two starts of one service: one creation, two commands with their start IDs, which the service ends
                // only with the last one's (CountingService asks next after the second).
                log("start 1: " + name(start(counting(1))));
                log("start 2: " + name(start(new Intent(COUNT).setPackage(getPackageName()).putExtra("n", 2))));
                break;
            case 2:
                // A stop through the filter's intent finds the running service.
                log("stop running: " + stopService(new Intent(COUNT).setPackage(getPackageName())));
                break;
            case 3:
                // A start and a stop at once: the service is made, started and destroyed, in that order.
                log("start 3: " + name(start(counting(3))));
                log("stop at once: " + stopService(counting(0)));
                break;
            case 4:
                // stopSelf() without a start ID ends the service (CountingService for n 4).
                log("start 4: " + name(start(counting(4))));
                break;
            case 5:
                sSent = new Intent(this, SyncService.class);
                log("foreground start: " + name(startForegroundService(sSent)));
                break;
            case 6:
                log("plain start: " + name(start(new Intent(this, PlainService.class))));
                break;
            default:
                log("done");
                break;
        }
    }
}
