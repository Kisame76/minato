package io.github.kisame76.minato.test.notifytap;

import android.app.Activity;
import android.app.NotificationManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/** What the notification's tap opens (notify-tap-test): its extra, then whether the notification went. */
public class TapActivity extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Log.i(TapApplication.TAG, "opened by the tap: room " + getIntent().getStringExtra("room"));
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            int active = getSystemService(NotificationManager.class).getActiveNotifications().length;
            Log.i(TapApplication.TAG, "active after the tap: " + active);
            Log.i(TapApplication.TAG, "done");
        }, 500);
    }
}
