package io.github.kisame76.minato.test.boundservices;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

import static io.github.kisame76.minato.test.boundservices.BoundServicesApplication.log;

/** A service whose onBind answers null, which a connection hears as a null binding. */
public class NullService extends Service {
    @Override
    public void onCreate() {
        super.onCreate();
        log("NullService onCreate");
    }

    @Override
    public IBinder onBind(Intent intent) {
        log("NullService onBind");
        return null;
    }

    @Override
    public boolean onUnbind(Intent intent) {
        log("NullService onUnbind");
        return false;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        log("NullService onDestroy");
        BoundServicesApplication.next();
    }
}
