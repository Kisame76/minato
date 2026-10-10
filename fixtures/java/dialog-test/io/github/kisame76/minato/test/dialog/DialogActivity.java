package io.github.kisame76.minato.test.dialog;

import android.app.Activity;
import android.app.Dialog;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Window;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * android.app.Dialog as an app uses it (the Unity test APK probe: UnityPlayerActivity.onCreate resolves the class): the
 * callbacks in the order Android calls them, the listeners, cancel, dismiss and hide, the back key, the window it makes,
 * the state it saves and the managed dialogs of Activity (showDialog). It runs on the main thread when the launcher
 * activity first has the window focus; what Android does after the first traversal (onAttachedToWindow) is logged after
 * the dialog had time to settle.
 */
public class DialogActivity extends Activity {
    static final String TAG = "dialog-test";
    private static final int SETTLE_MS = 600;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    /** Compared with Android's log. */
    private static void log(String line) {
        Log.i(TAG, line);
    }

    /**
     * Only shown: what needs the dialog's own window, which Minato does not show yet (ADR-0041 decision 2, gap 4): its
     * attach, focus and detach, and the back key, with what they cause.
     */
    private static void logDevice(String line) {
        Log.i(TAG + "-device", line);
    }

    private interface Call {
        Object run() throws Exception;
    }

    private static String outcome(Call call) {
        try {
            return String.valueOf(call.run());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    /** A dialog that logs each of its callbacks. */
    static class LoggedDialog extends Dialog {
        final String mName;
        /** Whether this dialog's lines are compared: a and b; c and d depend on the back key. */
        final boolean mCompared;

        LoggedDialog(Activity owner, String name, boolean compared) {
            super(owner);
            mName = name;
            mCompared = compared;
        }

        void say(String line) {
            if (mCompared) {
                log(line);
            } else {
                logDevice(line);
            }
        }

        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            say(mName + " onCreate state=" + (state == null ? "null" : "bundle"));
        }

        @Override
        protected void onStart() {
            super.onStart();
            say(mName + " onStart");
        }

        @Override
        protected void onStop() {
            super.onStop();
            say(mName + " onStop");
        }

        @Override
        public void onAttachedToWindow() {
            super.onAttachedToWindow();
            logDevice(mName + " onAttachedToWindow");
        }

        @Override
        public void onDetachedFromWindow() {
            super.onDetachedFromWindow();
            logDevice(mName + " onDetachedFromWindow");
        }

        @Override
        public void onBackPressed() {
            logDevice(mName + " onBackPressed");
            super.onBackPressed();
        }

        @Override
        public void onWindowFocusChanged(boolean hasFocus) {
            super.onWindowFocusChanged(hasFocus);
            logDevice(mName + " onWindowFocusChanged " + hasFocus);
        }
    }

    private LoggedDialog make(String name) {
        LoggedDialog dialog = new LoggedDialog(this, name, name.equals("a") || name.equals("b"));
        dialog.setOnShowListener(d -> dialog.say(name + " show-listener"));
        dialog.setOnDismissListener(d -> dialog.say(name + " dismiss-listener"));
        dialog.setOnCancelListener(d -> dialog.say(name + " cancel-listener"));
        return dialog;
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            mHandler.post(this::first);
        }
    }

    private void settle(Runnable next) {
        mHandler.postDelayed(next, SETTLE_MS);
    }

    private void first() {
        LoggedDialog a = make("a");
        log("new: owner is this " + (a.getOwnerActivity() == this) + ", showing " + a.isShowing()
                + ", window " + (a.getWindow() != null) + ", context is this "
                + (a.getContext() != null) + ", action bar " + a.getActionBar());
        log("window feature before content: " + outcome(() -> a.requestWindowFeature(Window.FEATURE_NO_TITLE)));
        log("volume stream " + a.getVolumeControlStream());
        TextView text = new TextView(this);
        text.setText("hello");
        text.setId(4242);
        a.setContentView(text);
        log("content view found by id " + (a.findViewById(4242) == text) + ", inflater " + (a.getLayoutInflater() != null));
        a.setTitle("title-a");
        log("window title " + a.getWindow().getAttributes().getTitle() + ", type " + a.getWindow().getAttributes().type);
        log("show:");
        a.show();
        log("after show: showing " + a.isShowing());
        log("second show is a no-op: " + outcome(() -> { a.show(); return a.isShowing(); }));
        Bundle saved = a.onSaveInstanceState();
        log("saved keys " + new java.util.TreeSet<>(saved.keySet()) + ", showing flag " + saved.getBoolean("android:dialogShowing"));
        settle(() -> second(a));
    }

    private void second(LoggedDialog a) {
        log("hide/show:");
        a.hide();
        log("after hide: showing " + a.isShowing());
        a.show();
        log("dismiss:");
        a.dismiss();
        log("after dismiss: showing " + a.isShowing());
        a.dismiss();
        log("dismissed twice");
        settle(() -> third());
    }

    private void third() {
        LoggedDialog b = make("b");
        b.setContentView(new TextView(this));
        b.show();
        log("cancel:");
        b.cancel();
        log("after cancel: showing " + b.isShowing());
        settle(this::third2);
    }

    private void third2() {
        LoggedDialog c = make("c");
        c.setContentView(new TextView(this));
        c.show();
        settle(() -> fourth(c));
    }

    private void fourth(LoggedDialog c) {
        logDevice("back key down/up on a cancelable dialog:");
        c.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK));
        c.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK));
        logDevice("after back: showing " + c.isShowing());
        c.dismiss();
        settle(this::fourth2);
    }

    private void fourth2() {
        LoggedDialog d = make("d");
        d.setContentView(new TextView(this));
        d.setCancelable(false);
        d.show();
        logDevice("back key on a not cancelable dialog:");
        d.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK));
        d.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK));
        logDevice("after back: showing " + d.isShowing());
        d.dismiss();
        settle(this::fifth);
    }

    private final List<String> mManaged = new ArrayList<>();

    @Override
    protected Dialog onCreateDialog(int id) {
        log("onCreateDialog " + id);
        Dialog dialog = new Dialog(this);
        dialog.setContentView(new TextView(this));
        return dialog;
    }

    @Override
    protected void onPrepareDialog(int id, Dialog dialog) {
        log("onPrepareDialog " + id);
    }

    @SuppressWarnings("deprecation")
    private void fifth() {
        log("managed dialog:");
        showDialog(7);
        showDialog(7);
        dismissDialog(7);
        removeDialog(7);
        log("an unknown id: " + outcome(() -> { dismissDialog(9); return "no exception"; }));
        log("done");
    }
}
