package io.github.kisame76.minato.test.isolated.feature;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** A service of the feature split; the test asks PackageManager about it only. */
public class FeatureService extends Service {
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
