package io.github.kisame76.minato.test.startingwindow;

import android.app.Activity;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;

/**
 * An activity that keeps its starting window on screen for 2 s with a pre-draw listener that returns false, as
 * androidx core-splashscreen's setKeepOnScreenCondition does: the window draws no frame until the listener lets it.
 */
public class KeepActivity extends Activity {
    static final long KEEP_MS = 2000;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        long created = SystemClock.uptimeMillis();
        View content = new View(this);
        content.setBackgroundColor(SlowActivity.CONTENT);
        setContentView(content);
        content.getViewTreeObserver().addOnPreDrawListener(new ViewTreeObserver.OnPreDrawListener() {
            @Override
            public boolean onPreDraw() {
                if (SystemClock.uptimeMillis() - created < KEEP_MS) {
                    content.postDelayed(content::invalidate, 50);
                    return false;
                }
                content.getViewTreeObserver().removeOnPreDrawListener(this);
                Log.i(SlowActivity.TAG, "KeepActivity: drawn after at least " + KEEP_MS + " ms");
                return true;
            }
        });
        Log.i(SlowActivity.TAG, "KeepActivity: created");
    }
}
