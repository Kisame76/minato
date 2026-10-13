package io.github.kisame76.minato.test.intentquery;

import android.content.Intent;
import android.os.IBinder;

/** One of the services whose filters intent-query-test resolves against; it never starts. */
public class LowService extends android.app.Service {
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
