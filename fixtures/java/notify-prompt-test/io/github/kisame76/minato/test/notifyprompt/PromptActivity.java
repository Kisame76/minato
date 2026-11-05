package io.github.kisame76.minato.test.notifyprompt;

import android.app.Activity;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * The notification prompt for an app below targetSdk 33 (N1b): on Android 13 and later the first notification channel
 * that such an app creates while its activity is in front makes the system ask the user for POST_NOTIFICATIONS, in a
 * dialog over the activity. The test logs the permission, whether notifications are enabled and the importance before
 * the channel, right after it and three seconds later, and the activity's lifecycle and window focus, which show the
 * dialog: the activity pauses and loses its window focus. It makes a second channel at the end, which asks no more.
 * The label is the tag.
 */
public class PromptActivity extends Activity {
    private String mTag;
    private NotificationManager mNotes;
    private final Handler mHandler = new Handler(Looper.getMainLooper());

    private String state() {
        return "permission "
                + (checkSelfPermission("android.permission.POST_NOTIFICATIONS") == PackageManager.PERMISSION_GRANTED
                        ? "granted" : "denied")
                + ", enabled " + mNotes.areNotificationsEnabled() + ", importance " + mNotes.getImportance();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getApplicationInfo().loadLabel(getPackageManager()).toString();
        mNotes = getSystemService(NotificationManager.class);
        Log.i(mTag, "before the channel: " + state());
        mNotes.createNotificationChannel(new NotificationChannel("messages", "Messages",
                NotificationManager.IMPORTANCE_DEFAULT));
        Log.i(mTag, "right after the channel: " + state());
        mHandler.postDelayed(() -> {
            Log.i(mTag, "three seconds later: " + state() + ", focus " + hasWindowFocus());
            mNotes.createNotificationChannel(new NotificationChannel("second", "Second",
                    NotificationManager.IMPORTANCE_LOW));
            mHandler.postDelayed(() -> {
                Log.i(mTag, "after a second channel: " + state() + ", focus " + hasWindowFocus());
                Log.i(mTag, "done");
            }, 1500);
        }, 3000);
    }

    @Override
    protected void onPause() {
        Log.i(mTag, "onPause");
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(mTag, "onResume");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        Log.i(mTag, "window focus " + hasFocus);
    }
}
