package io.github.kisame76.minato.test.windowsreturn;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.widget.PopupWindow;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

/**
 * The windows at the background and the return (ADR-0041 gap 4 (b5a)): once the launcher activity first has the
 * focus, a dialog over it (not canceled by a touch outside), a popup on the dialog's button, and a translucent activity
 * with a SurfaceView above them; the translucent one says "ready" before HOME and logs the windows after the return,
 * then finishes, and the popup and the dialog go. The activities' lifecycle and the windows' focus are logged; the
 * focus changes are kept for the next line, as they come asynchronously.
 */
public class ReturnActivity extends Activity {
    static final String TAG = "windows-return-test";
    static final int SETTLE_MS = 600;
    static ReturnActivity sInstance;
    final Handler mHandler = new Handler(Looper.getMainLooper());
    private final List<String> mFocus = new ArrayList<>();
    private boolean mStarted;
    AlertDialog mDialog;
    PopupWindow mPopup;

    static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;
        TextView text = new TextView(this);
        text.setText("Return");
        setContentView(text);
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        log("activity onRestart");
    }

    @Override
    protected void onStart() {
        super.onStart();
        log("activity onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        log("activity onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        log("activity onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        log("activity onStop");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!mStarted) {
            if (hasFocus) {
                mStarted = true;
                showWindows();
            }
            return;
        }
        mFocus.add("activity " + hasFocus);
    }

    /** A window's focus changes, kept for the next line. */
    void watchFocus(String name, View view) {
        view.getViewTreeObserver().addOnWindowFocusChangeListener(
                (ViewTreeObserver.OnWindowFocusChangeListener) hasFocus -> mFocus.add(name + " " + hasFocus));
    }

    /** The focus changes since the last line, in order. */
    void logFocus() {
        log("focus: " + (mFocus.isEmpty() ? "no change" : String.join(", ", mFocus)));
        mFocus.clear();
    }

    private void showWindows() {
        mDialog = new AlertDialog.Builder(this).setTitle("Title").setMessage("Message").setPositiveButton("OK", null)
                .create();
        mDialog.setCanceledOnTouchOutside(false);
        mDialog.show();
        watchFocus("dialog", mDialog.getWindow().getDecorView());
        mHandler.postDelayed(() -> {
            TextView content = new TextView(this);
            content.setText("Popup");
            content.setPadding(12, 12, 12, 12);
            mPopup = new PopupWindow(content, ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            mPopup.showAsDropDown(mDialog.getButton(AlertDialog.BUTTON_POSITIVE));
            startActivity(new Intent(this, ReturnTranslucentActivity.class));
        }, 300);
    }

    /** The windows below the translucent activity, as the app sees them. */
    String windows() {
        View decor = mDialog.getWindow().getDecorView();
        return "dialog showing " + mDialog.isShowing() + ", attached " + decor.isAttachedToWindow() + ", size "
                + decor.getWidth() + "x" + decor.getHeight() + "; popup showing " + mPopup.isShowing() + ", attached "
                + mPopup.getContentView().isAttachedToWindow();
    }

    /** ReturnTranslucentActivity's, at its destroy: the popup and the dialog go, and the end. */
    void afterTranslucent() {
        mHandler.postDelayed(() -> {
            logFocus();
            mPopup.dismiss();
            mDialog.dismiss();
            log("done");
        }, SETTLE_MS);
    }
}
