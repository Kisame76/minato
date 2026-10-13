package io.github.kisame76.minato.test.splits.feature;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** A receiver that only the feature split's manifest declares; the test asks PackageManager about it only. */
public class FeatureReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
    }
}
