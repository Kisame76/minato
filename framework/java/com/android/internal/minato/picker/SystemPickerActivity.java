package com.android.internal.minato.picker;

import android.app.Activity;
import android.app.ActivityThread;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import com.android.minato.picker.PickerRequest;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The system picker (ADR-0057 decision 1): the activity that stands for Android's DocumentsUI and photo picker when an
 * app starts one of the five picker intents for a result. It asks the host for the pick over the session and waits;
 * the host answers with PICK_RESULT, and the activity returns that result to the app's activity and finishes. A
 * cancel is RESULT_CANCELED without data. Without a session there is no host to ask, and the result is a cancel at
 * once.
 */
public final class SystemPickerActivity extends Activity {
    private static final String TAG = "SystemPicker";
    /** Linux's EOPNOTSUPP, as ActivityThread's requests answer without a session. */
    private static final int EOPNOTSUPP = 95;

    private static final AtomicLong sNextId = new AtomicLong(1);
    /** The activities waiting for the host's answer, by request ID; the main thread's. */
    private static final Map<Long, SystemPickerActivity> sWaiting = new HashMap<>();

    private long mId;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final Intent intent = getIntent();
        final int kind = PickerIntents.kindOf(intent);
        mId = sNextId.getAndIncrement();
        sWaiting.put(mId, this);
        final int error = kind == PickerIntents.NONE ? -EOPNOTSUPP
                : ActivityThread.requestPick(mId, requestText(intent, kind));
        if (error != 0) {
            if (error != -EOPNOTSUPP) {
                Log.w(TAG, "The host refused the pick request: " + error);
            }
            sWaiting.remove(mId);
            setResult(RESULT_CANCELED);
            finish();
        }
    }

    /** The request's text from the app's intent, inside the limits the runtime enforces. */
    private static String requestText(Intent intent, int kind) {
        final int max = kind == PickerIntents.PICK_IMAGES
                ? intent.getIntExtra(PickerIntents.EXTRA_PICK_IMAGES_MAX, 1) : 0;
        return PickerRequest.text(kind, intent.getType(), intent.getStringArrayExtra(Intent.EXTRA_MIME_TYPES),
                intent.getBooleanExtra(Intent.EXTRA_ALLOW_MULTIPLE, false), max,
                intent.getStringExtra(Intent.EXTRA_TITLE));
    }

    /** The host's answer to request `id`: PICK_RESULT's result code and number of picks. */
    public static void onResult(long id, int resultCode, int count) {
        final SystemPickerActivity picker = sWaiting.remove(id);
        if (picker == null) {
            Log.w(TAG, "An answer for pick request " + id + " that nobody waits for");
            return;
        }
        picker.deliver(resultCode, count);
    }

    private void deliver(int resultCode, int count) {
        if (resultCode == RESULT_CANCELED || count == 0) {
            setResult(RESULT_CANCELED);
        } else {
            // The picks arrive with ADR-0057 step b: the host's manifest of what it cloned.
            Log.w(TAG, "The host picked " + count + ", which step b serves; the result is a cancel");
            setResult(RESULT_CANCELED);
        }
        finish();
    }

    @Override
    protected void onDestroy() {
        // Gone before the answer, for instance finishActivity(requestCode): the host drops the picker.
        if (sWaiting.remove(mId) != null) {
            ActivityThread.cancelPick(mId);
        }
        super.onDestroy();
    }
}
