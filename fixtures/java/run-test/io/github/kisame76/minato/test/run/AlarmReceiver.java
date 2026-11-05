package io.github.kisame76.minato.test.run;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Records the alarm whose action it receives, at the elapsed time it arrives. */
public class AlarmReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        RunApplication.fired(intent.getAction());
    }
}
