package io.github.kisame76.minato.test.appservices;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** A disabled service, which no start finds. */
public class OffService extends Service {
    @Override
    public void onCreate() {
        AppServicesApplication.log("OffService onCreate");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
