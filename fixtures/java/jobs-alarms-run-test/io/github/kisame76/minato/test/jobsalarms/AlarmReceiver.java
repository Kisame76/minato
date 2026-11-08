package io.github.kisame76.minato.test.jobsalarms;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** The receiver of the alarms' PendingIntents: it hands the action to the test. */
public class AlarmReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        JobsAlarmsApplication.record("alarm " + intent.getAction());
    }
}
