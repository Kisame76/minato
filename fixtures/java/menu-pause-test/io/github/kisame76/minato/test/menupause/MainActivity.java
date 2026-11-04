package io.github.kisame76.minato.test.menupause;

import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/** The activity under test; it says "ready" once it has the focus, and the script writes the rest of the markers. */
public class MainActivity extends LoggedActivity {
    private boolean mReady;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView view = new TextView(this);
        view.setText("menu-pause-test");
        setContentView(view);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mReady) {
            mReady = true;
            Log.i(TAG, "ready");
        }
    }
}
