package io.github.kisame76.minato.test.restrictbackground;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.os.Bundle;
import android.util.Log;

/**
 * Data Saver as an app sees it (ADR-0054 step d)): the restrict-background status at the start, then "ready", then
 * each ACTION_RESTRICT_BACKGROUND_CHANGED while the system turns Data Saver on and off again: whether it came to a
 * registered receiver only, and the status then. It ends with "done" after the second.
 */
public class RestrictActivity extends Activity {
    private static final String TAG = "restrict-background-test";

    private int mChanges;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static String status(int status) {
        switch (status) {
            case ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED:
                return "DISABLED";
            case ConnectivityManager.RESTRICT_BACKGROUND_STATUS_WHITELISTED:
                return "WHITELISTED";
            case ConnectivityManager.RESTRICT_BACKGROUND_STATUS_ENABLED:
                return "ENABLED";
            default:
                return "unknown " + status;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ConnectivityManager connectivity = getSystemService(ConnectivityManager.class);
        log("status: " + status(connectivity.getRestrictBackgroundStatus()));
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                mChanges++;
                log("changed " + mChanges + ": registered only "
                        + ((intent.getFlags() & Intent.FLAG_RECEIVER_REGISTERED_ONLY) != 0) + ", status "
                        + status(connectivity.getRestrictBackgroundStatus()));
                if (mChanges == 2) {
                    unregisterReceiver(this);
                    log("done");
                }
            }
        };
        registerReceiver(receiver, new IntentFilter(ConnectivityManager.ACTION_RESTRICT_BACKGROUND_CHANGED),
                Context.RECEIVER_EXPORTED);
        log("ready");
    }
}
