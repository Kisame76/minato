package io.github.kisame76.minato.test.turn;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.TextView;

/**
 * The base of turn-test's two activities. It logs every callback of its lifecycle with the saved state it gets, the
 * configuration it hears, and the size of its decor. It does not turn the display: it says "ready for turn 1" and
 * "ready for turn 2" and the script that plays the host turns it to landscape and back to portrait (tools/android-
 * reference.sh's turn-test on the AVD, apk-run's turn steps on the Mac). Half a second after the display has turned and
 * the activity is in its new orientation it logs where its window is; after the portrait the plain activity starts
 * the other and ends, the other says "done".
 */
public abstract class TurnActivity extends Activity {
    static final String TAG = "turn-test";
    private static final long SETTLE_MS = 500;
    private static final Handler HANDLER = new Handler(Looper.getMainLooper());
    private static TurnActivity sCurrent;
    /** 0 before the first focus, 1 while it waits for landscape, 2 while it waits for portrait, 3 when it is over */
    private static int sState;
    private static int sInstances;

    private final String mName = getClass().getSimpleName().replace("Activity", "").toLowerCase();
    private int mInstance;

    abstract Class<?> next();

    private void log(String line) {
        Log.i(TAG, mName + " " + mInstance + " " + line);
    }

    private String config(Configuration configuration) {
        return "orientation " + configuration.orientation + ", " + configuration.screenWidthDp + " x "
                + configuration.screenHeightDp + " dp";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mInstance = ++sInstances;
        sCurrent = this;
        TextView text = new TextView(this);
        text.setText(mName);
        setContentView(text);
        log("onCreate, saved " + (savedInstanceState == null ? "none" : savedInstanceState.getString("saved")) + ", "
                + config(getResources().getConfiguration()));
    }

    @Override
    protected void onStart() {
        super.onStart();
        log("onStart");
    }

    @Override
    protected void onRestoreInstanceState(Bundle savedInstanceState) {
        super.onRestoreInstanceState(savedInstanceState);
        log("onRestoreInstanceState");
    }

    @Override
    protected void onResume() {
        super.onResume();
        log("onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        log("onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        log("onStop");
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putString("saved", mName + " " + mInstance);
        log("onSaveInstanceState");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        log("onDestroy, changing configurations " + isChangingConfigurations());
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        log("onConfigurationChanged " + config(newConfig) + ", resources " + config(getResources().getConfiguration()));
        advance();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        log("onWindowFocusChanged " + hasFocus);
        if (hasFocus) {
            advance();
        }
    }

    /** What the activity does once its window has focus or its configuration changed: the script of turns. */
    private void advance() {
        final int orientation = getResources().getConfiguration().orientation;
        if (sState == 0) {
            sState = 1;
            log("ready");
            HANDLER.postDelayed(() -> sCurrent.log("ready for turn 1"), SETTLE_MS);
        } else if (sState == 1 && orientation == Configuration.ORIENTATION_LANDSCAPE) {
            sState = 2;
            HANDLER.postDelayed(() -> {
                sCurrent.settled("in landscape");
                sCurrent.log("ready for turn 2");
            }, SETTLE_MS);
        } else if (sState == 2 && orientation == Configuration.ORIENTATION_PORTRAIT) {
            sState = 3;
            HANDLER.postDelayed(() -> {
                final TurnActivity activity = sCurrent;
                activity.settled("in portrait");
                final Class<?> next = activity.next();
                if (next != null) {
                    sState = 0;
                    activity.log("starts the next activity");
                    activity.startActivity(new Intent(activity, next));
                    activity.finish();
                } else {
                    activity.log("done");
                }
            }, SETTLE_MS);
        }
    }

    /** Where this instance's window is, as the decor and the display metrics tell it. */
    private void settled(String what) {
        View decor = getWindow().getDecorView();
        log(what + ": decor " + decor.getWidth() + " x " + decor.getHeight() + ", display "
                + getResources().getDisplayMetrics().widthPixels + " x "
                + getResources().getDisplayMetrics().heightPixels + ", " + config(getResources().getConfiguration()));
    }
}
