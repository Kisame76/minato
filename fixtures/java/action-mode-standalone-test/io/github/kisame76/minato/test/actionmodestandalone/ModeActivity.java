package io.github.kisame76.minato.test.actionmodestandalone;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.ActionMode;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * An action mode in a window without an action bar. The activity starts one once it
 * has the focus (startActionMode returns the mode, which the window's own standalone bar shows), logs the callbacks
 * in order, the mode's title and subtitle, whether the window has the bar's view, then finishes the mode with its
 * finish(). Ends with "done".
 */
public class ModeActivity extends Activity {
    private static String TAG;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TAG = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        final FrameLayout root = new FrameLayout(this);
        final TextView text = new TextView(this);
        text.setText("content");
        root.addView(text);
        setContentView(root);
        log("onCreate getActionBar " + (getActionBar() != null));
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        log("ready");
        final ActionMode mode = startActionMode(new ActionMode.Callback() {
            @Override
            public boolean onCreateActionMode(ActionMode m, Menu menu) {
                log("onCreateActionMode");
                m.setTitle("mode");
                m.setSubtitle("sub");
                menu.add("Act");
                return true;
            }

            @Override
            public boolean onPrepareActionMode(ActionMode m, Menu menu) {
                log("onPrepareActionMode");
                return false;
            }

            @Override
            public boolean onActionItemClicked(ActionMode m, MenuItem item) {
                return false;
            }

            @Override
            public void onDestroyActionMode(ActionMode m) {
                log("onDestroyActionMode");
            }
        });
        log("startActionMode " + (mode != null ? "returned a mode" : "returned null"));
        if (mode != null) {
            log("title " + mode.getTitle() + ", subtitle " + mode.getSubtitle() + ", type " + mode.getType());
            final View bar = getWindow().getDecorView().findViewById(
                    getResources().getIdentifier("action_mode_bar", "id", "android"));
            log("window has the mode bar " + (bar != null) + (bar != null ? ", visible "
                    + (bar.getVisibility() == View.VISIBLE) : ""));
            mHandler.postDelayed(mode::finish, 1500);
        } else {
            mHandler.postDelayed(() -> log("done"), 1500);
        }
    }

    @Override
    public void onActionModeStarted(ActionMode mode) {
        log("onActionModeStarted");
        super.onActionModeStarted(mode);
    }

    @Override
    public void onActionModeFinished(ActionMode mode) {
        log("onActionModeFinished");
        super.onActionModeFinished(mode);
        mHandler.postDelayed(() -> log("done"), 1000);
    }
}
