package io.github.kisame76.minato.test.imepan;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

/**
 * ADR-0068's field at the bottom: a magenta EditText at the window's bottom, its window's soft input mode unspecified,
 * adjustPan and adjustResize in turn. For each the keyboard comes and stays up for HOLD_MS ("<mode> shown", the
 * moment a screenshot is taken), then goes. On Android it runs with the image's own keyboard, so its screenshots show
 * where the system puts the field. Ends with "done".
 */
public class PanActivity extends Activity {
    // Long enough for an iPhone's screenshot: the check sees "shown" in the log seconds later over devicectl.
    private static final long HOLD_MS = 12000;
    private static final long SETTLE_MS = 2000;
    private static final String[] NAMES = {"unspecified", "pan", "resize"};
    private static final int[] MODES = {WindowManager.LayoutParams.SOFT_INPUT_ADJUST_UNSPECIFIED,
            WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN, WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE};
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private String mTag;
    private EditText mEdit;
    private boolean mStarted;
    private int mAt;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        final FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        mEdit = new EditText(this);
        mEdit.setBackgroundColor(Color.MAGENTA);
        mEdit.setMinHeight(200);
        root.addView(mEdit, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM));
        setContentView(root);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        Log.i(mTag, "ready");
        mHandler.postDelayed(this::step, SETTLE_MS);
    }

    private void step() {
        final InputMethodManager keyboard = getSystemService(InputMethodManager.class);
        if (mAt == MODES.length) {
            Log.i(mTag, "done");
            return;
        }
        final String name = NAMES[mAt];
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_UNSPECIFIED | MODES[mAt]);
        mAt++;
        mEdit.requestFocus();
        keyboard.showSoftInput(mEdit, 0);
        mHandler.postDelayed(() -> {
            Log.i(mTag, name + " shown");
            mHandler.postDelayed(() -> {
                keyboard.hideSoftInputFromWindow(mEdit.getWindowToken(), 0);
                mHandler.postDelayed(this::step, SETTLE_MS);
            }, HOLD_MS);
        }, SETTLE_MS);
    }
}
