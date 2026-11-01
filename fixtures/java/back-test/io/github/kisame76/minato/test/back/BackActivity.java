package io.github.kisame76.minato.test.back;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;

/**
 * back-test's activities (ADR-0041 decision 10): each logs its lifecycle, the BACK keys that reach it and
 * onBackPressed, under the app's label. Once its window first has the focus, Root's only after its restart, it logs
 * "ready: <name>" under the label with "-steps", which the reference's steps wait for: when the focus comes among the
 * other activity's callbacks is a matter of timing. The log ends with "done" at Root's last BACK:
 * what Android does after it, the task moved to the back, is Minato's menu instead.
 */
public abstract class BackActivity extends Activity {
    private static String sTag;
    private static boolean sDone;
    private boolean mRestarted;
    private boolean mReady;

    /** The activity's name in the log. */
    abstract String name();

    /** Whether the app opted in to OnBackInvokedCallback: back-test-callback, by its label. */
    boolean callbacks() {
        return sTag.endsWith("-callback");
    }

    void log(String text) {
        if (!sDone) {
            Log.i(sTag, name() + " " + text);
        }
    }

    void done() {
        if (!sDone) {
            Log.i(sTag, "done");
            sDone = true;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sTag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        super.onCreate(savedInstanceState);
        log("onCreate");
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        mRestarted = true;
        log("onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        log("onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        log("onResume");
    }

    @Override
    protected void onPause() {
        log("onPause");
        super.onPause();
    }

    @Override
    protected void onStop() {
        log("onStop");
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        log("onDestroy");
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mReady && (mRestarted || !(this instanceof RootActivity))) {
            mReady = true;
            Log.i(sTag + "-steps", "ready: " + name());
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        log(key(event));
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        log(key(event));
        return super.onKeyUp(keyCode, event);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        log("onBackPressed");
        super.onBackPressed();
    }

    /** A key as the views get it, without its flags, which the system's ways of making BACK set differently. */
    private static String key(KeyEvent event) {
        return KeyEvent.keyCodeToString(event.getKeyCode())
                + (event.getAction() == KeyEvent.ACTION_DOWN ? " down" : " up") + " repeat " + event.getRepeatCount()
                + " source 0x" + Integer.toHexString(event.getSource()) + " device " + event.getDeviceId()
                + (event.isTracking() ? " tracking" : "");
    }
}
