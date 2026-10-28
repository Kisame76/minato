package io.github.kisame76.minato.test.documentsedit;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * ADR-0057's write-back test: an app opens a document with ACTION_OPEN_DOCUMENT, replaces its content through the
 * URI (mode "wt"), closes the stream and reads the document back. On Android the URI is the file itself, so the file
 * in Download holds the new text at once; Minato's document is a clone, and the host writes it back to the user's
 * file (the check compares that file after the run). After "done" the app ends itself, as a user would leave it, so
 * that a host that ends the session at the app's finish, the iPhone's, needs no one to quit it. Logs under the tag
 * documents-edit-test.
 */
public class EditActivity extends Activity {
    static final String TAG = "documents-edit-test";
    private static final int REQUEST = 7;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(() -> {
            log("start");
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("text/plain");
            startActivityForResult(intent, REQUEST);
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        log("result: " + resultCode + " flags=" + (data == null ? -1 : data.getFlags()));
        try {
            Uri uri = data.getData();
            try (OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
                out.write("edited by app\n".getBytes(StandardCharsets.UTF_8));
            }
            log("wrote");
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                log("read back: " + new String(in.readAllBytes(), StandardCharsets.UTF_8).trim());
            }
        } catch (Exception e) {
            log("failed: " + e);
        }
        mHandler.postDelayed(() -> {   // the host writes a clone back a second after its last change
            log("done");
            finish();
        }, 2500);
    }

    // The finish's first step, at once on Android (onDestroy may come much later); the picker's pause is not logged.
    @Override
    protected void onPause() {
        super.onPause();
        if (isFinishing()) {
            log("paused to finish");
        }
    }
}
