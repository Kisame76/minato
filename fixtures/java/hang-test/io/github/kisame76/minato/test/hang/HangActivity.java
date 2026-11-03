package io.github.kisame76.minato.test.hang;

import android.app.Activity;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.widget.TextView;

/**
 * A hung guest: once its first frame is drawn, the main thread sleeps 60 s, then logs "awake". Its lifecycle callbacks
 * are logged, so a check sees which ones the host's menu and Quit reached while it hung.
 */
public class HangActivity extends Activity {
    private static final String TAG = "hang-test";
    private static final long HANG_MS = 60000;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("hang-test");
        setContentView(text);
        Log.i(TAG, "onCreate");
        text.post(() -> {
            Log.i(TAG, "hanging");
            SystemClock.sleep(HANG_MS);
            Log.i(TAG, "awake");
        });
    }

    @Override
    protected void onPause() {
        super.onPause();
        Log.i(TAG, "onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        Log.i(TAG, "onStop");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "onDestroy");
    }
}
