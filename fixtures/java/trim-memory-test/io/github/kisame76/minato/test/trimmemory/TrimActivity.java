package io.github.kisame76.minato.test.trimmemory;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/**
 * The activity of trim-memory-test: logs its lifecycle and the memory callbacks it gets. It says "ready" once its
 * window has the focus, and "done" after the callback of level 80.
 */
public class TrimActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("trim-memory-test");
        setContentView(text);
        Log.i(TrimApplication.TAG, "activity onCreate");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !TrimApplication.ready) {
            TrimApplication.ready = true;
            Log.i(TrimApplication.TAG, "ready");
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        Log.i(TrimApplication.STEPS, "activity onStop");
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        TrimApplication.memory("activity onTrimMemory " + level);
        if (level == 80) {
            TrimApplication.memory("done");
        }
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        TrimApplication.memory("activity onLowMemory");
    }
}
