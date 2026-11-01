package io.github.kisame76.minato.test.actionbarmenu;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * Compatibility plan gap 5, step 4: the options menu of the platform action bar, driven from outside by keys. The MENU
 * key opens the overflow (onMenuOpened), the arrow down and enter keys walk the popup and choose its first item
 * (onOptionsItemSelected), the app then invalidates the menu (onPrepareOptionsMenu again, with the changed title),
 * and the MENU key opens and a second one closes the overflow. The script behind the keys: the AVD's adb shell input
 * keyevent in tools/android-reference.sh, apk-run's key steps in tools/action-bar-menu-check.sh. Ends with "done".
 */
public class MenuActivity extends Activity {
    private static String TAG;
    private static final int FIRST = 1;
    private static final int THIRD = 3;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private int mPrepared;
    private int mOpened;
    private int mClosed;
    private boolean mSelected;
    private boolean mInvalidated;
    private int mOpenedAfterInvalidation;
    private final Runnable mDone = () -> log("done");
    private String mLabel = "Third";

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
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            log("ready");
        }
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        log("onCreateOptionsMenu");
        menu.add(Menu.NONE, FIRST, 1, "First").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        menu.add(Menu.NONE, 2, 2, "Second").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        menu.add(Menu.NONE, THIRD, 3, mLabel).setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        menu.add(Menu.NONE, 4, 4, "Fourth").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        return true;
    }

    @Override
    public boolean onPrepareOptionsMenu(Menu menu) {
        log("onPrepareOptionsMenu " + (++mPrepared) + ", items " + menu.size() + ", third \""
                + menu.findItem(THIRD).getTitle() + "\"");
        return true;
    }

    @Override
    public boolean onMenuOpened(int featureId, Menu menu) {
        log("onMenuOpened " + (++mOpened) + " feature " + featureId + ", items " + (menu != null ? menu.size() : -1));
        if (mInvalidated) {
            mOpenedAfterInvalidation++;
        }
        return super.onMenuOpened(featureId, menu);
    }

    @Override
    public void onPanelClosed(int featureId, Menu menu) {
        log("onPanelClosed " + (++mClosed) + " feature " + featureId);
        super.onPanelClosed(featureId, menu);
        // The MENU key that opens the overflow after the invalidation and the one that closes it: the run is over once
        // the closes have stopped.
        if (mInvalidated && mOpenedAfterInvalidation >= 2) {
            mHandler.removeCallbacks(mDone);
            mHandler.postDelayed(mDone, 1500);
        }
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        log("onOptionsItemSelected " + item.getTitle());
        if (!mSelected) {
            mSelected = true;
            mLabel = "Third (changed)";
            // After the popup has gone, as an app does from a click.
            mHandler.postDelayed(() -> {
                log("invalidateOptionsMenu");
                mInvalidated = true;
                invalidateOptionsMenu();
            }, 600);
        }
        return true;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        log("onKeyDown " + KeyEvent.keyCodeToString(keyCode));
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        log("onKeyUp " + KeyEvent.keyCodeToString(keyCode));
        return super.onKeyUp(keyCode, event);
    }
}
