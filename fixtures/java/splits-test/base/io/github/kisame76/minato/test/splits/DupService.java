package io.github.kisame76.minato.test.splits;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** A service of the base module that the feature split's manifest declares again. */
public class DupService extends Service {
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
