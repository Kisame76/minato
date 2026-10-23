package com.android.internal.minato.open;

import android.app.Activity;
import android.app.ActivityThread;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The activity that stands in for the phone's apps that iOS answers (ADR-0062): the browser, mail, phone, messages,
 * maps and the share sheet. It asks the host to open what the intent names (APKRT_REQUEST_OPEN) and ends with its answer,
 * RESULT_OK when the host showed it or the user finished, RESULT_CANCELED when it was not opened. It has no window of its
 * own: the host's sheet is the screen, and the translucent theme keeps the app's under it.
 */
public final class SystemOpenActivity extends Activity {
    private static final String TAG = "SystemOpen";
    /** Linux's EOPNOTSUPP, as ActivityThread's requests answer without a session. */
    private static final int EOPNOTSUPP = 95;

    private static final AtomicLong sNextId = new AtomicLong(1);
    /** The activities waiting for the host's answer, by request ID; the main thread's. */
    private static final Map<Long, SystemOpenActivity> sWaiting = new HashMap<>();
    /** Whether the share folders of an earlier process were swept: the first request does it, when none can wait. */
    private static boolean sSwept;

    private long mId;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (!sSwept) {
            sSwept = true;
            OpenIntents.deleteShare(this, -1);
        }
        final Intent intent = getIntent();
        mId = sNextId.getAndIncrement();
        final String text = OpenIntents.requestText(this, intent, mId);
        if (text == null) {
            OpenIntents.deleteShare(this, mId);
            setResult(RESULT_CANCELED);
            finish();
            return;
        }
        sWaiting.put(mId, this);
        final int error = ActivityThread.requestOpen(mId, text);
        if (error != 0) {
            if (error != -EOPNOTSUPP) {
                Log.w(TAG, "The host refused the open request: " + error);
            }
            sWaiting.remove(mId);
            OpenIntents.deleteShare(this, mId);
            setResult(RESULT_CANCELED);
            finish();
        }
    }

    /** The host's answer to request `id`: OPEN_RESULT's result code (RESULT_OK or RESULT_CANCELED) and its reason, for the log. */
    public static void onResult(long id, int resultCode, int reason) {
        final SystemOpenActivity activity = sWaiting.remove(id);
        if (activity == null) {
            Log.w(TAG, "An answer for open request " + id + " that nobody waits for");
            return;
        }
        if (reason != 0) {
            Log.i(TAG, "The host did not open request " + id + ": reason " + reason);
        }
        OpenIntents.deleteShare(activity, id);
        activity.setResult(resultCode == RESULT_OK ? RESULT_OK : RESULT_CANCELED);
        activity.finish();
    }

    @Override
    protected void onDestroy() {
        // Gone before the answer, for instance finishActivity(requestCode): the host drops what it shows.
        if (sWaiting.remove(mId) != null) {
            ActivityThread.cancelOpen(mId);
            OpenIntents.deleteShare(this, mId);
        }
        super.onDestroy();
    }
}
