package io.github.kisame76.minato.test.startingwindow;

import android.app.Activity;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;

/**
 * An activity that takes 3 s to create, so that its starting window stays on screen long enough for a capture (the
 * splash's colour is the test's subject), then shows a yellow content view, which no splash colour of the test is.
 */
public class SlowActivity extends Activity {
    static final String TAG = "starting-window-test";
    static final int CONTENT = 0xFFFFEB3B;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String name = getClass().getSimpleName();
        Log.i(TAG, name + ": created");
        View content = new View(this);
        content.setBackgroundColor(CONTENT);
        setContentView(content);
        SystemClock.sleep(3000);
        content.post(() -> Log.i(TAG, name + ": drawn"));
    }
}
