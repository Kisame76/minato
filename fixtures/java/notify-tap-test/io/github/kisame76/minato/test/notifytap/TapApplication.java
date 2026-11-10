package io.github.kisame76.minato.test.notifytap;

import android.app.Application;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Log;

/**
 * ADR-0074 decision 4's test of a tap on a notification: the app posts one whose content intent opens TapActivity
 * with an extra, "room", and the extras chat and news apps keep an ID in (a nested Bundle, a String[] and an int[], a
 * Uri, an Intent inside the Intent, a ParcelUuid), and a second later sends that PendingIntent itself, which is what
 * SystemUI does on a tap (a tap in the emulator's shade did not reach the notification from the reference script);
 * TapActivity logs what it was opened with and, half a second later, the active notifications. Minato's own tap,
 * through Notification Center and APKRT_EVENT_NOTIFICATION_TAP, is the Simulator's check. POST_NOTIFICATIONS is
 * granted before the start. The tag is the label.
 */
public class TapApplication extends Application {
    static final String TAG = "notify-tap-test";

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager notes = getSystemService(NotificationManager.class);
        notes.createNotificationChannel(new NotificationChannel("taps", "Taps", NotificationManager.IMPORTANCE_HIGH));
        Bundle thread = new Bundle();
        thread.putInt("id", 42);
        thread.putString("title", "Team");
        Intent open = new Intent(this, TapActivity.class).putExtra("room", "7").putExtra("thread", thread)
                .putExtra("tags", new String[] {"news", "sport"}).putExtra("counts", new int[] {3, 4})
                .putExtra("uri", Uri.parse("content://io.github.kisame76.minato.test.notifytap/article/9"))
                .putExtra("inner", new Intent("io.github.kisame76.minato.test.notifytap.OPEN").putExtra("article", 9))
                .putExtra("uuid", ParcelUuid.fromString("6f1c2a3e-0000-4000-8000-000000000007"));
        PendingIntent content = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE);
        notes.notify("tap", 3, new Notification.Builder(this, "taps").setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle("Tap me").setContentText("It opens the app").setContentIntent(content)
                .setAutoCancel(true).build());
        Log.i(TAG, "posted: enabled " + notes.areNotificationsEnabled());
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            try {
                content.send();
                Log.i(TAG, "sent the content intent");
            } catch (PendingIntent.CanceledException e) {
                Log.i(TAG, "the content intent was cancelled");
            }
        }, 1000);
    }
}
