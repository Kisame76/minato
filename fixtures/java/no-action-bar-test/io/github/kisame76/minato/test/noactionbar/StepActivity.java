package io.github.kisame76.minato.test.noactionbar;

import android.app.Activity;
import android.content.Intent;
import android.content.res.TypedArray;
import android.os.Bundle;
import android.util.Log;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * One step of no-action-bar-test: the activity asks for its window feature before super.onCreate, as a game engine's
 * activity asks for FEATURE_NO_TITLE, then calls getActionBar() and setContentView, and logs what Android 15 answers:
 * the request's result, whether there is an action bar before and after the content, the theme's window attributes,
 * the window's features and fullscreen flag, and whether the decor has an action bar view. Then it starts the next
 * step; the last one logs "done". On Android no title dominates the action bar, so no step has one.
 */
abstract class StepActivity extends Activity {
    /** The app's label: the log tag android-reference.sh waits on. */
    private static final String TAG = "no-action-bar-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** The window feature this step asks for before super.onCreate. */
    abstract int feature();

    /** The step started after this one, or null for the last. */
    abstract Class<? extends Activity> next();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        final String name = getClass().getSimpleName();
        final String feature = feature() == Window.FEATURE_NO_TITLE ? "NO_TITLE" : "ACTION_BAR";
        log(name + " requestWindowFeature " + feature + " " + requestWindowFeature(feature()));
        super.onCreate(savedInstanceState);
        log(name + " onCreate getActionBar " + (getActionBar() != null));
        final FrameLayout root = new FrameLayout(this);
        final TextView text = new TextView(this);
        text.setText(name);
        root.addView(text);
        setContentView(root);
        // obtainStyledAttributes wants the attributes in the order of their ids.
        final TypedArray a = obtainStyledAttributes(new int[] {
                android.R.attr.windowNoTitle, android.R.attr.windowFullscreen, android.R.attr.windowActionBar});
        log(name + " theme: windowNoTitle " + a.getBoolean(0, false) + ", windowFullscreen " + a.getBoolean(1, false)
                + ", windowActionBar " + a.getBoolean(2, false));
        a.recycle();
        final Window window = getWindow();
        final int barId = getResources().getIdentifier("action_bar", "id", "android");
        log(name + " content set: getActionBar " + (getActionBar() != null)
                + ", NO_TITLE " + window.hasFeature(Window.FEATURE_NO_TITLE)
                + ", ACTION_BAR " + window.hasFeature(Window.FEATURE_ACTION_BAR)
                + ", fullscreen " + ((window.getAttributes().flags & WindowManager.LayoutParams.FLAG_FULLSCREEN) != 0)
                + ", action bar view " + (barId != 0 && window.getDecorView().findViewById(barId) != null));
        final Class<? extends Activity> next = next();
        if (next != null) {
            startActivity(new Intent(this, next));
        } else {
            log("done");
        }
    }
}
