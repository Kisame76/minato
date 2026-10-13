package io.github.kisame76.minato.test.appservices;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.Looper;

import static io.github.kisame76.minato.test.appservices.AppServicesApplication.log;

/**
 * The started service of parts 1 to 4: its callbacks, their thread, its contexts, the intents it gets (copies of the
 * ones sent) and how it ends.
 */
public class CountingService extends Service {
    private static int sInstances;

    @Override
    public void onCreate() {
        super.onCreate();
        sInstances++;
        log("CountingService onCreate instance " + sInstances + " main " + (Looper.myLooper() == Looper.getMainLooper())
                + " application " + (getApplication() == AppServicesApplication.sApp)
                + " applicationContext " + (getApplicationContext() == AppServicesApplication.sApp)
                + " base " + getBaseContext().getClass().getName()
                + " loader " + (getClassLoader() == AppServicesApplication.sApp.getClassLoader()));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        int n = intent.getIntExtra("n", -1);
        log("CountingService onStartCommand id " + startId + " flags " + flags + " n " + n + " action "
                + intent.getAction() + " copy " + (intent != AppServicesApplication.sSent) + " main "
                + (Looper.myLooper() == Looper.getMainLooper()));
        if (n == 2) {
            // Not the last start: the service stays.
            log("stopSelfResult 1: " + stopSelfResult(1));
            AppServicesApplication.next();
        } else if (n == 4) {
            stopSelf();
            log("stopSelf asked");
        }
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        log("CountingService onDestroy instance " + sInstances + " main "
                + (Looper.myLooper() == Looper.getMainLooper()));
        super.onDestroy();
        AppServicesApplication.next();
    }
}
