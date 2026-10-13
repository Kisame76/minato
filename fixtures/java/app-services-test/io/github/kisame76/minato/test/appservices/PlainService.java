package io.github.kisame76.minato.test.appservices;

import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

import static io.github.kisame76.minato.test.appservices.AppServicesApplication.log;

/** The service of part 6, without a foreground service type, which a target SDK of 35 cannot put in the foreground. */
public class PlainService extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification notification = new Notification.Builder(this, "sync")
                .setSmallIcon(android.R.drawable.stat_notify_sync).setContentTitle("Plain").build();
        try {
            startForeground(2, notification);
            log("untyped: foreground");
        } catch (RuntimeException e) {
            log("untyped: " + e.getClass().getName());
        }
        log("plain type " + getForegroundServiceType());
        stopSelf();
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        log("PlainService onDestroy");
        super.onDestroy();
        AppServicesApplication.next();
    }
}
