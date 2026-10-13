package io.github.kisame76.minato.test.splits.feature;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** A service that only the feature split's manifest declares; the test asks PackageManager about it only. */
public class FeatureService extends Service {
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
