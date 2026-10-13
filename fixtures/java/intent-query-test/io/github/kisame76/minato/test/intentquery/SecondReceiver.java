package io.github.kisame76.minato.test.intentquery;

import android.content.Context;
import android.content.Intent;

/** One of the receivers whose filters intent-query-test resolves against; no broadcast reaches it. */
public class SecondReceiver extends android.content.BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
    }
}
