package io.github.kisame76.minato.test.appservices;

import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;

import static io.github.kisame76.minato.test.appservices.AppServicesApplication.log;

/**
 * The foreground service of part 5, of type dataSync: a type the manifest does not name, then its own, its type
 * while in the foreground and after, and stopSelf.
 */
public class SyncService extends Service {
    @Override
    public void onCreate() {
        super.onCreate();
        log("SyncService onCreate type " + getForegroundServiceType());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        log("SyncService onStartCommand id " + startId + " copy " + (intent != AppServicesApplication.sSent));
        Notification notification = new Notification.Builder(this, "sync")
                .setSmallIcon(android.R.drawable.stat_notify_sync).setContentTitle("Sync").build();
        try {
            startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION);
            log("location: foreground");
        } catch (IllegalArgumentException e) {
            log("location: " + e.getClass().getName() + ": " + e.getMessage());
        }
        startForeground(1, notification);
        log("foreground type " + getForegroundServiceType());
        stopForeground(STOP_FOREGROUND_REMOVE);
        log("stopped foreground type " + getForegroundServiceType());
        stopSelf(startId);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        log("SyncService onDestroy");
        super.onDestroy();
        AppServicesApplication.next();
    }
}
