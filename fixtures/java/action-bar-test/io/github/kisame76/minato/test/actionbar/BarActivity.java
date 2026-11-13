package io.github.kisame76.minato.test.actionbar;

import android.app.Activity;
import android.app.ActionBar;
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
 * The platform action bar: an activity of Theme.DeviceDefault.Light with no AppCompat
 * does what a default-theme app does in onCreate and later. Each step logs what Android 15 answers: whether
 * getActionBar() has a bar, its title, subtitle, display options and height, where the content starts, the options menu
 * (items, the prepare calls, clicks on an action item and an overflow item, the overflow opening and closing), hide and
 * show, and an action mode. Times and pixel values that depend on the display are left out of the lines or compared as
 * the same on both sides; "done" ends the run.
 */
public class BarActivity extends Activity {
    /** The app's label, which names the app under test: the log tag android-reference.sh waits on. */
    private static String TAG = "action-bar-test";
    private static final int FIRST = 1, SECOND = 2, THIRD = 3, FOURTH = 4;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private View mContent;
    private int mPrepared;
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TAG = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        final ActionBar bar = getActionBar();
        // What apps write in onCreate; with no bar every call is a NullPointerException on this line.
        log("onCreate getActionBar " + (bar != null));
        bar.setDisplayHomeAsUpEnabled(true);
        bar.setSubtitle("sub");
        final FrameLayout root = new FrameLayout(this);
        mContent = root;
        final TextView text = new TextView(this);
        text.setText("content");
        root.addView(text);
        setContentView(root);
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        log("onCreateOptionsMenu");
        menu.add(Menu.NONE, FIRST, 1, "First").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        menu.add(Menu.NONE, SECOND, 2, "Second").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        menu.add(Menu.NONE, THIRD, 3, "Third").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        menu.add(Menu.NONE, FOURTH, 4, "Fourth").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        return true;
    }

    @Override
    public boolean onPrepareOptionsMenu(Menu menu) {
        log("onPrepareOptionsMenu " + (++mPrepared) + ", items " + menu.size());
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        log("onOptionsItemSelected " + (item.getItemId() == android.R.id.home ? "home" : item.getTitle()));
        return true;
    }

    @Override
    public boolean onMenuOpened(int featureId, Menu menu) {
        log("onMenuOpened feature " + featureId + ", items " + (menu != null ? menu.size() : -1));
        return super.onMenuOpened(featureId, menu);
    }

    @Override
    public void onPanelClosed(int featureId, Menu menu) {
        log("onPanelClosed feature " + featureId);
        super.onPanelClosed(featureId, menu);
    }

    @Override
    public void onOptionsMenuClosed(Menu menu) {
        log("onOptionsMenuClosed");
        super.onOptionsMenuClosed(menu);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            step(0);
        }
    }

    private int contentTop() {
        final int[] at = new int[2];
        mContent.getLocationInWindow(at);
        return at[1];
    }

    private void report(String what) {
        final ActionBar bar = getActionBar();
        // Where the content starts against the bar, which does not depend on the insets of the display: below the bar,
        // or under it (a window that is edge to edge from target SDK 35 on). The numbers go under the device tag.
        final View container = getWindow().getDecorView().findViewById(
                getResources().getIdentifier("action_bar_container", "id", "android"));
        final int[] containerAt = new int[2];
        if (container != null) {
            container.getLocationInWindow(containerAt);
        }
        final int containerBottom = container != null ? containerAt[1] + container.getHeight() : -1;
        Log.i(TAG + "-device", what + ": bar height " + bar.getHeight() + ", content top " + contentTop()
                + ", container " + (container != null ? containerAt[1] + "+" + container.getHeight() : "none")
                + ", decor height " + getWindow().getDecorView().getHeight() + ", content height "
                + mContent.getHeight());
        log(what + ": showing " + bar.isShowing() + ", bar " + (bar.getHeight() > 0 ? "has a height" : "has none")
                + ", content " + (contentTop() >= containerBottom ? "below" : "under") + " the bar, title "
                + bar.getTitle() + ", subtitle " + bar.getSubtitle() + ", home as up "
                + ((bar.getDisplayOptions() & ActionBar.DISPLAY_HOME_AS_UP) != 0));
    }

    private void step(int number) {
        final long delay = 1200;
        switch (number) {
            case 0:
                report("start");
                break;
            case 1:
                log("performIdentifierAction first: " + getWindow().performPanelIdentifierAction(
                        android.view.Window.FEATURE_OPTIONS_PANEL, FIRST, 0));
                break;
            case 2:
                log("performIdentifierAction third (overflow): " + getWindow().performPanelIdentifierAction(
                        android.view.Window.FEATURE_OPTIONS_PANEL, THIRD, 0));
                break;
            case 3:
                openOptionsMenu();
                break;
            case 4:
                closeOptionsMenu();
                break;
            case 5:
                invalidateOptionsMenu();
                break;
            case 6:
                getActionBar().hide();
                break;
            case 7:
                report("hidden");
                getActionBar().show();
                break;
            case 8:
                report("shown again");
                startActionMode(new ActionMode.Callback() {
                    @Override
                    public boolean onCreateActionMode(ActionMode mode, Menu menu) {
                        log("onCreateActionMode");
                        mode.setTitle("mode");
                        menu.add("Act");
                        return true;
                    }

                    @Override
                    public boolean onPrepareActionMode(ActionMode mode, Menu menu) {
                        log("onPrepareActionMode");
                        return false;
                    }

                    @Override
                    public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
                        return false;
                    }

                    @Override
                    public void onDestroyActionMode(ActionMode mode) {
                        log("onDestroyActionMode");
                    }
                });
                break;
            default:
                log("done");
                return;
        }
        if (number == 8) {
            mHandler.postDelayed(() -> {
                // The mode ends with the back key's way, finish().
                finishActionModeIfAny();
                step(9);
            }, delay);
        } else {
            mHandler.postDelayed(() -> step(number + 1), delay);
        }
    }

    private ActionMode mMode;

    @Override
    public void onActionModeStarted(ActionMode mode) {
        mMode = mode;
        log("onActionModeStarted");
        super.onActionModeStarted(mode);
    }

    @Override
    public void onActionModeFinished(ActionMode mode) {
        log("onActionModeFinished");
        super.onActionModeFinished(mode);
    }

    private void finishActionModeIfAny() {
        if (mMode != null) {
            mMode.finish();
        }
    }
}
