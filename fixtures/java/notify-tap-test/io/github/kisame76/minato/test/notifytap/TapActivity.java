package io.github.kisame76.minato.test.notifytap;

import android.app.Activity;
import android.app.NotificationManager;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.util.Arrays;

/**
 * What the notification's tap opens (notify-tap-test): its extra, then the extras that are no string, number or boolean
 * (a cold tap's start must keep them as Android does), then whether the notification went.
 */
public class TapActivity extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Intent intent = getIntent();
        Log.i(TapApplication.TAG, "opened by the tap: room " + intent.getStringExtra("room"));
        Log.i(TapApplication.TAG, "the tap's extras: " + extras(intent));
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            int active = getSystemService(NotificationManager.class).getActiveNotifications().length;
            Log.i(TapApplication.TAG, "active after the tap: " + active);
            Log.i(TapApplication.TAG, "done");
        }, 500);
    }

    /** The extras that are no string, number or boolean, as text; "null" for one that is missing. */
    private static String extras(Intent intent) {
        Bundle thread = intent.getBundleExtra("thread");
        Intent inner = intent.getParcelableExtra("inner");
        return "thread " + (thread == null ? "null" : thread.getInt("id") + " " + thread.getString("title"))
                + ", tags " + Arrays.toString(intent.getStringArrayExtra("tags"))
                + ", counts " + Arrays.toString(intent.getIntArrayExtra("counts"))
                + ", uri " + intent.getParcelableExtra("uri")
                + ", inner " + (inner == null ? "null" : inner.getAction() + " " + inner.getIntExtra("article", 0))
                + ", uuid " + intent.getParcelableExtra("uuid");
    }
}
