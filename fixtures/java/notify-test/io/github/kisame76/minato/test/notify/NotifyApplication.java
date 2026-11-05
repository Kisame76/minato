package io.github.kisame76.minato.test.notify;

import android.app.Application;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.os.Handler;
import android.os.Looper;
import android.service.notification.StatusBarNotification;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * ADR-0074 decision 2's test of NotificationManager.notify and cancel: two channels, a post, the same post again (which
 * replaces it), a second one, one on a channel the app never made, a cancel and a cancel of all, each followed by the
 * app's active notifications half a second later, as the service posts them in order. notify-test has no grant, so
 * nothing is active; its twin notify-test-granted gets POST_NOTIFICATIONS before the start. The tag is the label.
 */
public class NotifyApplication extends Application {
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private NotificationManager mNotes;
    private String mTag;
    private int mStep;

    @Override
    public void onCreate() {
        super.onCreate();
        mTag = getApplicationInfo().loadLabel(getPackageManager()).toString();
        mNotes = getSystemService(NotificationManager.class);
        Log.i(mTag, "enabled " + mNotes.areNotificationsEnabled() + ", importance " + mNotes.getImportance());
        mNotes.createNotificationChannel(new NotificationChannel("messages", "Messages",
                NotificationManager.IMPORTANCE_HIGH));
        mNotes.createNotificationChannel(new NotificationChannel("quiet", "Quiet", NotificationManager.IMPORTANCE_LOW));
        mHandler.postDelayed(this::next, 500);
    }

    private Notification note(String channel, String title) {
        return new Notification.Builder(this, channel).setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle(title).setContentText("Text of " + title).build();
    }

    private void next() {
        switch (mStep++) {
            case 0: mNotes.notify("chat", 7, note("messages", "First")); log("posted chat 7"); break;
            case 1: mNotes.notify("chat", 7, note("messages", "First again")); log("posted chat 7 again"); break;
            case 2: mNotes.notify(8, note("quiet", "Second")); log("posted 8"); break;
            case 3: mNotes.notify(9, note("none", "No channel")); log("posted 9 on no channel"); break;
            case 4: mNotes.cancel("chat", 7); log("cancelled chat 7"); break;
            case 5: mNotes.cancelAll(); log("cancelled all"); break;
            default:
                Log.i(mTag, "done");
                return;
        }
    }

    /** The active notifications half a second after `what`, as "tag|id title". */
    private void log(String what) {
        mHandler.postDelayed(() -> {
            List<String> active = new ArrayList<>();
            for (StatusBarNotification shown : mNotes.getActiveNotifications()) {
                active.add(shown.getTag() + "|" + shown.getId() + " "
                        + shown.getNotification().extras.getCharSequence(Notification.EXTRA_TITLE));
            }
            Collections.sort(active);
            Log.i(mTag, what + ": active " + active);
            next();
        }, 500);
    }
}
