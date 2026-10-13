package io.github.kisame76.minato.test.boundservices;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;

import static io.github.kisame76.minato.test.boundservices.BoundServicesApplication.log;
import static io.github.kisame76.minato.test.boundservices.BoundServicesApplication.main;

/** The bound service: its binder, each callback with the intent's action and its thread, and onRebind asked for. */
public class BoundService extends Service {
    /** The binder onBind gives, which a connection in the process gets as it is. */
    static IBinder sBinder;

    @Override
    public void onCreate() {
        super.onCreate();
        sBinder = new Binder();
        log("BoundService onCreate main " + main());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        log("BoundService onStartCommand id " + startId);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        log("BoundService onBind action " + intent.getAction() + " main " + main());
        return sBinder;
    }

    @Override
    public boolean onUnbind(Intent intent) {
        String line = "BoundService onUnbind action " + intent.getAction();
        if (BoundServicesApplication.waiting()) {
            BoundServicesApplication.seen("service unbind");
        } else {
            log(line);
        }
        return true;
    }

    @Override
    public void onRebind(Intent intent) {
        if (BoundServicesApplication.waiting()) {
            BoundServicesApplication.seen("service rebind");
        } else {
            log("BoundService onRebind action " + intent.getAction());
        }
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        if (BoundServicesApplication.waiting()) {
            BoundServicesApplication.seen("service destroyed");
        } else {
            log("BoundService onDestroy main " + main());
            BoundServicesApplication.next();
        }
    }
}
